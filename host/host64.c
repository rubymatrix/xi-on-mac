/* The game host for 64-bit machines (R3: Windows x64 now, arm64 macOS next).
 *
 * What pol.exe and COM do for retail FFXI on Windows, with no x86 anywhere: the retail
 * FFXiMain.dll and FFXi.dll are mapped into the guest window (FFXiMain at its preferred
 * 0x10000000, FFXi.dll - which prefers the same base - relocated), both CRTs start, FFXi.dll's
 * FFXiEntry is created through its class factory, and IFFXiEntry::GameStart runs the game with
 * our own polcore (runtime/portable/polcore.c) in place of PlayOnline's.
 *
 * usage: host64 --game <FINAL FANTASY XI folder> [--reg <file.reg>]... [--reg-overlay <file.reg>]
 *               [--server <name or a.b.c.d>]
 *               [--session <V: 16 characters, or 32 hex digits>]                   PlayOnline servers
 *               [--user <name> [--pass <password>] [--otp <code>] [--login-token <t>]   LandSandBoat servers
 *                [--authport 54231] [--dataport 54230] [--viewport 54001]]
 *
 * --server is where the game's servers are: "ffxi00.pol.com" (the lobby) and every other
 * "*.pol.com" name resolve to it instead of through DNS. Default 127.0.0.1 (this
 * machine); --pol-server and --lobby are older names for it.
 *
 * --fps-divisor: FFXI's frames are 60 / divisor per second; 1 (60 fps) here, 2 (30) as shipped.
 *
 * Two ways in:
 *   - a server with PlayOnline behind it: the session value V its lobby checks
 *     (pol_accounts.session_value). For now --session; a PlayOnline sign-in client supplies it.
 *   - a LandSandBoat server with none (xiloader's path, host/lsb_login.c): --user signs in on the
 *     server's auth port first. The password comes from --pass, else FFXI_PASSWORD, else a
 *     prompt; --otp is the two-factor code, if the account has one. --login-token is a launch token
 *     from the server's own launcher, in place of the password and code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "polcore.h"
#include "polcore_config.h"
#include "lsb_login.h"
#include "thunk.h"
#include "user32.h"
#include "d3d8.h"
#include "gfx.h"
#include "dsound.h"
#include "dinput.h"
#include "ws2.h"
#include "plat.h"
#include "vfs.h"
#include "build.h" /* FFXI_VERSION */

extern const RtModule rt_module_ffxi; /* recomp.py --module ffxi */

#define FFXI_BASE 0x0F000000u /* where FFXi.dll goes: free, below FFXiMain */
#define DEFAULT_POL_SERVER 0x7F000001u /* 127.0.0.1: a server on this machine */

/* FFXiEntry {989D790D-6236-11D4-80E9-00105A81E890}, IFFXiEntry {989D790C-...} */
static const uint8_t CLSID_FFXiEntry[16] = { 0x0D, 0x79, 0x9D, 0x98, 0x36, 0x62, 0xD4, 0x11,
                                             0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90 };
static const uint8_t IID_IFFXiEntry[16] = { 0x0C, 0x79, 0x9D, 0x98, 0x36, 0x62, 0xD4, 0x11,
                                            0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90 };
/* GameMain {1027DC46-750D-4B1F-8834-1D25B8BEBAB8} (FFXiMain), FxFileManager
 * {0DF0E951-D03C-4A94-90EF-40AE60668F5F} (FFXi.dll): the classes FFXi.dll creates */
static const uint8_t CLSID_GameMain[16] = { 0x46, 0xDC, 0x27, 0x10, 0x0D, 0x75, 0x1F, 0x4B,
                                            0x88, 0x34, 0x1D, 0x25, 0xB8, 0xBE, 0xBA, 0xB8 };
static const uint8_t CLSID_FxFileManager[16] = { 0x51, 0xE9, 0xF0, 0x0D, 0x3C, 0xD0, 0x94, 0x4A,
                                                 0x90, 0xEF, 0x40, 0xAE, 0x60, 0x66, 0x8F, 0x5F };
static const uint8_t IID_IClassFactory[16] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

static uint32_t guest_bytes(const void* p, uint32_t n)
{
    uint32_t a = gheap_alloc(n, 1);
    memcpy(GUEST_PTR(a), p, n);
    return a;
}

static uint32_t com_call(uint32_t obj, unsigned slot, unsigned nargs, const uint32_t* args)
{
    uint32_t all[8] = { obj };
    for (unsigned i = 0; i < nargs && i < 7; ++i)
        all[i + 1] = args[i];
    return guest_call(rd32(rd32(obj) + 4u * slot), nargs + 1, all);
}

/* --- the frame rate ---------------------------------------------------------------------------------
 * FFXiMain paces its frames by a divisor of 60: 2 (30 fps) as shipped, 1 for 60. It is a field
 * of an object FFXiMain creates at startup; Ashita's fps addon finds it the same way: the code
 * `sub esp, 0x100; cmp eax, ecx; je +0x21; mov ecx, [global]` names the global that points at
 * the object, and the divisor is at +0x30. The game may reset it (zoning), so every frame puts
 * it back. */
static uint32_t g_fps_divisor = 1, g_fps_global;

static void find_fps_global(void)
{
    static const uint8_t PAT[] = { 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x3B, 0xC1, 0x74, 0x21, 0x8B, 0x0D };
    uint32_t start = rt_image_base + rt_image_text_rva, end = start + rt_image_text_size;
    for (uint32_t a = start; a + sizeof PAT + 4 <= end; ++a)
        if (!memcmp(GUEST_PTR(a), PAT, sizeof PAT))
        {
            g_fps_global = rd32(a + sizeof PAT);
            rt_log("[recomp] frame-rate divisor: global %08x (code at %08x), set to %u (%u fps)\n", g_fps_global, a,
                g_fps_divisor, 60 / g_fps_divisor);
            return;
        }
    rt_log("[recomp] frame-rate divisor: not found; the game keeps its own frame rate\n");
    g_fps_global = 0xFFFFFFFFu;
}

static int g_profile_shims;

static void present_hook(void)
{
    if (g_profile_shims)
    {
        static uint64_t last;
        uint64_t now = rt_monotonic_ns();
        thunk_prof_thread = plat_thread_id();
        if (!last)
            last = now;
        if (now - last >= 2000000000ull)
            thunk_prof_report(), last = now;
    }
    if (!g_fps_global)
        find_fps_global();
    if (g_fps_global == 0xFFFFFFFFu)
        return;
    uint32_t object = rd32(g_fps_global);
    if (object && rd32(object + 0x30) != g_fps_divisor)
        wr32(object + 0x30, g_fps_divisor);
}

static int parse_session(const char* s, uint8_t v[16])
{
    size_t n = strlen(s);
    if (n == 16)
    {
        memcpy(v, s, 16);
        return 1;
    }
    if (n != 32)
        return 0;
    for (int i = 0; i < 16; ++i)
    {
        unsigned b;
        if (sscanf(s + 2 * i, "%2x", &b) != 1)
            return 0;
        v[i] = (uint8_t)b;
    }
    return 1;
}

int main(int argc, char** argv)
{
    const char* game = NULL;
    const char* regs[8];
    unsigned nregs = 0;
    const char* overlay = NULL;
    uint32_t pol_server = DEFAULT_POL_SERVER;
    LsbLogin lsb = { 0, 54231, 54230, 54001, NULL, NULL, "", NULL };
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (!strcmp(argv[i], "--game"))
            game = argv[i + 1];
        else if (!strcmp(argv[i], "--reg") && nregs < 8)
            regs[nregs++] = argv[i + 1];
        else if (!strcmp(argv[i], "--reg-overlay"))
            overlay = argv[i + 1];
        else if (!strcmp(argv[i], "--session"))
        {
            uint8_t v[16];
            if (!parse_session(argv[i + 1], v))
            {
                fprintf(stderr, "--session: 16 characters or 32 hex digits\n");
                return 2;
            }
            polcore_set_session(v);
        }
        else if (!strcmp(argv[i], "--server") || !strcmp(argv[i], "--pol-server") || !strcmp(argv[i], "--lobby"))
        {
            if (!net_resolve_ipv4(argv[i + 1], &pol_server))
            {
                fprintf(stderr, "%s: cannot resolve %s\n", argv[i], argv[i + 1]);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--user"))
            lsb.user = argv[i + 1];
        else if (!strcmp(argv[i], "--pass"))
            lsb.password = argv[i + 1];
        else if (!strcmp(argv[i], "--otp"))
            lsb.otp = argv[i + 1];
        else if (!strcmp(argv[i], "--login-token"))
            lsb.login_token = argv[i + 1];
        else if (!strcmp(argv[i], "--fps-divisor"))
        {
            long d = strtol(argv[i + 1], NULL, 10);
            if (d < 1 || d > 60)
            {
                fprintf(stderr, "--fps-divisor: 1 (60 fps), 2 (30 fps, as shipped), ...\n");
                return 2;
            }
            g_fps_divisor = (uint32_t)d;
        }
        else if (!strcmp(argv[i], "--authport") || !strcmp(argv[i], "--dataport") || !strcmp(argv[i], "--viewport"))
        {
            long port = strtol(argv[i + 1], NULL, 10);
            if (port <= 0 || port > 65535)
            {
                fprintf(stderr, "%s: a port number\n", argv[i]);
                return 2;
            }
            *(argv[i][2] == 'a' ? &lsb.auth_port : argv[i][2] == 'd' ? &lsb.data_port : &lsb.view_port) = (uint16_t)port;
        }
    }
    if (!game)
    {
        fprintf(stderr, "usage: host64 --game <FINAL FANTASY XI folder> [--reg f.reg]... [--reg-overlay f.reg] "
                        "[--server name] [--session V | --user name [--pass p] [--otp code] [--authport n] "
                        "[--dataport n] [--viewport n]]\n");
        return 2;
    }
    if (lsb.user)
    {
        /* a LandSandBoat server: sign in before anything is loaded, so a refusal costs nothing */
        char pw[256], err[512];
        if (!lsb.password)
            lsb.password = getenv("FFXI_PASSWORD");
        if (!lsb.password && lsb.login_token)
            lsb.password = ""; /* the token replaces it */
        if (!lsb.password)
        {
            if (!read_secret("Password: ", pw, sizeof pw))
            {
                fprintf(stderr, "--user: no password (--pass, FFXI_PASSWORD, or a terminal to ask on)\n");
                return 2;
            }
            lsb.password = pw;
        }
        lsb.server = pol_server;
        int ok = lsb_login(&lsb, err, sizeof err);
        memset(pw, 0, sizeof pw);
        if (!ok)
        {
            fprintf(stderr, "sign-in failed: %s\n", err);
            return 1;
        }
    }

    if (!gwin_init())
    {
        fprintf(stderr, "cannot reserve the guest window\n");
        return 1;
    }
    gt_init();
    rt_set_native_handler(thunk_dispatch);
    /* The game sees Windows paths. On Windows they are the host's own; elsewhere the install
     * (FINAL FANTASY XI and PlayOnlineViewer, side by side as retail installs them) is mounted at
     * C:\PlayOnline\SquareEnix. */
    char exe[700], path[700], guest_game[700], host_game[700];
    snprintf(host_game, sizeof host_game, "%s", game);
    for (size_t n = strlen(host_game); n > 1 && (host_game[n - 1] == '/' || host_game[n - 1] == '\\'); --n)
        host_game[n - 1] = 0;
    if (plat_path_sep == '\\')
        snprintf(guest_game, sizeof guest_game, "%s", host_game);
    else
    {
        char host_viewer[760];
        snprintf(guest_game, sizeof guest_game, "C:\\PlayOnline\\SquareEnix\\FINAL FANTASY XI");
        snprintf(host_viewer, sizeof host_viewer, "%s/../PlayOnlineViewer", host_game);
        vfs_mount(guest_game, host_game);
        vfs_mount("C:\\PlayOnline\\SquareEnix\\PlayOnlineViewer", host_viewer);
    }
    game = guest_game;
    snprintf(exe, sizeof exe, "%s\\..\\PlayOnlineViewer\\pol.exe", game);
    k32_init(game, exe);
    k32_io_init();
    k32_misc_init();
    ole_init();
    user32_init();
    d3d8_init();
    dsound_init();
    dinput_init();
    ws2_init();
    /* PlayOnline's hosts: the lobby through polcore's resolver, every other *.pol.com through
     * gethostbyname */
    ws2_set_pol_server(pol_server);
    polcore_set_lobby(pol_server);
    rt_log("[recomp] *.pol.com -> %u.%u.%u.%u\n", pol_server >> 24, (pol_server >> 16) & 255, (pol_server >> 8) & 255,
        pol_server & 255);
    reg_init(regs, nregs, overlay);
    {
        /* The install folders are where the game is now, whatever the imported registry says (it
         * comes from another machine or folder); the game checks them (FFXI-9001). Retail writes
         * 0001 with a trailing backslash and 1000 without. */
        char p[760];
        snprintf(p, sizeof p, "%s\\", game);
        reg_set_string("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\InstallFolder", "0001", p);
        snprintf(p, sizeof p, "%s\\..\\PlayOnlineViewer", game);
        char viewer[760];
        if (vfs_full_path(p, viewer, sizeof viewer))
            snprintf(p, sizeof p, "%s", viewer);
        reg_set_string("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\InstallFolder", "1000", p);
    }
    vfs_init(game);
    {
        /* The game reads patch.ver from its folder and will not start without it; the lobby sees
         * the version inside. Installs launched without the PlayOnline Viewer (private servers'
         * xiloader) ship none: then one is made for this build's version, next to the host, and
         * mounted over the game's path. The install itself is never written. */
        char pv[760];
        PlatStat st;
        snprintf(pv, sizeof pv, "%s%cpatch.ver", host_game, plat_path_sep);
        if (!plat_stat(pv, &st))
        {
            uint8_t file[0x120];
            const char *dir_end = argv[0], *p;
            for (p = argv[0]; *p; ++p)
                if (*p == '/' || *p == plat_path_sep)
                    dir_end = p + 1;
            snprintf(pv, sizeof pv, "%.*spatch.%s.ver", (int)(dir_end - argv[0]), argv[0], FFXI_VERSION);
            PlatFile* f = polcore_make_patch_ver(FFXI_VERSION, file) ? plat_file_open(pv, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE) : NULL;
            if (!f || plat_file_write(f, file, sizeof file) != sizeof file)
            {
                fprintf(stderr, "cannot write %s\n", pv);
                return 1;
            }
            plat_file_close(f);
            char guest_pv[760];
            snprintf(guest_pv, sizeof guest_pv, "%s\\patch.ver", game);
            vfs_mount(guest_pv, pv);
            printf("[recomp] no patch.ver in the install: version %s from %s\n", FFXI_VERSION, pv);
        }
    }
    polcore_slots_init();
    polcore_files_init();
    polcore_polpro_init();
    {
        char viewer[700];
        snprintf(viewer, sizeof viewer, "%s\\..\\PlayOnlineViewer", game);
        char full[700];
        if (vfs_full_path(viewer, full, sizeof full))
            polcore_set_root(full);
    }

    /* the images first, before the heap spreads through the low window */
    snprintf(path, sizeof path, "%s%cFFXiMain.dll", host_game, plat_path_sep);
    if (!pe_load(path))
        return 1;
    snprintf(path, sizeof path, "%s%cFFXi.dll", host_game, plat_path_sep);
    if (!pe_load_module(path, &rt_module_ffxi, FFXI_BASE))
        return 1;
    k32_add_module("FFXi.dll", FFXI_BASE);
    ole_register_class(CLSID_GameMain, rt_image_base);
    ole_register_class(CLSID_FxFileManager, FFXI_BASE);
    ole_register_class(CLSID_FFXiEntry, FFXI_BASE);
    polcore_init();
    d3d8_setup();
    d3d8_set_present_hook(present_hook);
    if (getenv("FFXI_PROFILE") && getenv("FFXI_PROFILE")[0] && getenv("FFXI_PROFILE")[0] != '0')
        thunk_timer = gfx_prof_shim, g_profile_shims = 1; /* the profile splits the game's time into its code and our API calls */
    dsound_setup();
    dinput_setup();
    if (getenv("FFXI_RECOMP_MISSING"))
        thunk_report_missing();

    /* both CRTs, as the Windows loader would run them */
    uint32_t attach[3] = { rt_image_base, 1, 0 };
    if (!guest_call(rt_image_oep, 3, attach))
    {
        fprintf(stderr, "FFXiMain's DllMain failed\n");
        return 1;
    }
    uint32_t attach_ffxi[3] = { FFXI_BASE, 1, 0 };
    if (!guest_call(rt_module_ffxi.oep + *rt_module_ffxi.delta, 3, attach_ffxi))
    {
        fprintf(stderr, "FFXi.dll's DllMain failed\n");
        return 1;
    }

    /* FFXiEntry, through FFXi.dll's class factory */
    gt_lock();
    uint32_t clsid = guest_bytes(CLSID_FFXiEntry, 16), iid_cf = guest_bytes(IID_IClassFactory, 16),
             iid_entry = guest_bytes(IID_IFFXiEntry, 16), out = gheap_alloc(4, 1);
    gt_unlock();
    uint32_t gco[3] = { clsid, iid_cf, out };
    uint32_t hr = guest_call(pe_export_at(FFXI_BASE, "DllGetClassObject"), 3, gco);
    uint32_t cf = rd32(out);
    if (hr || !cf)
    {
        fprintf(stderr, "FFXi.dll DllGetClassObject(FFXiEntry): %08x\n", hr);
        return 1;
    }
    uint32_t ci[3] = { 0, iid_entry, out };
    hr = com_call(cf, 3, 3, ci);
    uint32_t entry = rd32(out);
    if (hr || !entry)
    {
        fprintf(stderr, "IClassFactory::CreateInstance(IFFXiEntry): %08x\n", hr);
        return 1;
    }

    /* IFFXiEntry::GameStart(pPol, &pFFXiMessage): the game runs inside this call */
    rt_log("[recomp] GameStart\n");
    uint32_t start[2] = { polcore_object(), out };
    hr = com_call(entry, 3, 2, start);
    const char* message = NULL;
    int32_t code = polcore_exit_code(&message);
    rt_log("[recomp] GameStart returned %08x; exit code %d%s%s\n", hr, code, message && *message ? ", message: " : "",
        message ? message : "");
    return hr ? 1 : 0;
}
