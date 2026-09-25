/* The sign-in screen. See signin.h.
 *
 * Two windows in the game's art: Sign in (the ID - a LandSandBoat username or a PlayOnline ID,
 * one field either way - the password, LandSandBoat's one-time code, Sign in and Settings) and
 * Settings (the method, the server, remembering the password, the window theme). Keys: Tab and the
 * arrows move, Enter acts, Left/Right switch a choice, Escape goes back or quits; the mouse clicks.
 *
 * What it remembers is in signin.cfg (key=value lines) beside settings.reg, the password in the
 * keychain (keychain.h). The sign-in runs on a worker thread so the window keeps drawing; a
 * PlayOnline session then stays open for the run, its keepalives answered on a thread of its own. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datui.h"
#include "keychain.h"
#include "lsb_login.h"
#include "plat.h"
#include "polcore_config.h"
#include "polsession.h"
#include "signin.h"
#include "ui_art.h"
#include "uidraw.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"

/* ---- what is remembered ---- */

typedef struct Config
{
    int method;
    char server[128];
    char user[64];
    int remember;
    int theme;
    uint16_t auth_port, data_port, view_port;
    char background[1024]; /* a picture behind the screen; "": background.png/.jpg beside signin.cfg */
} Config;

static void config_path(const char* dir, const char* name, char* out, size_t n)
{
    size_t len = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, len && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/", name);
}

static void config_load(const char* path, Config* c)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f))
    {
        line[strcspn(line, "\r\n")] = 0;
        char* v = strchr(line, '=');
        if (!v)
            continue;
        *v++ = 0;
        if (!strcmp(line, "method"))
            c->method = !strcmp(v, "pol") ? SIGNIN_POL : SIGNIN_LSB;
        else if (!strcmp(line, "server"))
            SDL_strlcpy(c->server, v, sizeof c->server);
        else if (!strcmp(line, "user"))
            SDL_strlcpy(c->user, v, sizeof c->user);
        else if (!strcmp(line, "remember"))
            c->remember = atoi(v) != 0;
        else if (!strcmp(line, "theme"))
            c->theme = atoi(v);
        else if (!strcmp(line, "background"))
            SDL_strlcpy(c->background, v, sizeof c->background);
        else if (!strcmp(line, "auth_port"))
            c->auth_port = (uint16_t)atoi(v);
        else if (!strcmp(line, "data_port"))
            c->data_port = (uint16_t)atoi(v);
        else if (!strcmp(line, "view_port"))
            c->view_port = (uint16_t)atoi(v);
    }
    fclose(f);
}

static void config_save(const char* path, const Config* c)
{
    FILE* f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "[signin] cannot write %s\n", path);
        return;
    }
    fprintf(f, "method=%s\nserver=%s\nuser=%s\nremember=%d\ntheme=%d\n", c->method == SIGNIN_POL ? "pol" : "lsb",
        c->server, c->user, c->remember, c->theme);
    if (c->background[0])
        fprintf(f, "background=%s\n", c->background);
    if (c->auth_port || c->data_port || c->view_port)
        fprintf(f, "auth_port=%u\ndata_port=%u\nview_port=%u\n", c->auth_port, c->data_port, c->view_port);
    fclose(f);
}

static void keychain_key(const Config* c, char* out, size_t n)
{
    snprintf(out, n, "%s:%s:%s", c->method == SIGNIN_POL ? "pol" : "lsb", c->server, c->user);
}

/* The game's display settings when nothing else gives them: a window of 1920x1080, as the
 * launcher's defaults, or the app's own. Written once; the player's own edits to it stay. */
static void default_settings(const char* path, const SigninSetup* su)
{
    FILE* f = fopen(path, "r");
    if (f)
    {
        fclose(f);
        return;
    }
    f = fopen(path, "w");
    if (!f)
        return;
    static const struct
    {
        const char* name;
        uint32_t v;
    } VALUES[] = {
        { "0000", 6 }, { "0001", 1920 }, { "0002", 1080 }, { "0003", 4096 }, { "0004", 4096 }, { "0007", 1 },
        { "0011", 1 }, { "0017", 1 }, { "0018", 1 }, { "0019", 1 }, { "0021", 0 }, { "0022", 1 },
        { "0023", 0 }, { "0029", 12 }, { "0034", 1 }, { "0035", 1 }, { "0036", 0 }, { "0037", 960 },
        { "0038", 540 }, { "0040", 0 },
    };
    fprintf(f, "REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\r\n");
    for (size_t i = 0; i < sizeof VALUES / sizeof *VALUES; ++i)
    {
        uint32_t v = VALUES[i].v;
        const char* n = VALUES[i].name;
        if (!strcmp(n, "0034") && su->default_mode >= 0 && su->default_mode <= 3)
            v = (uint32_t)su->default_mode;
        else if (!strcmp(n, "0001") && su->default_w >= 640)
            v = (uint32_t)su->default_w;
        else if (!strcmp(n, "0002") && su->default_h >= 480)
            v = (uint32_t)su->default_h;
        else if (!strcmp(n, "0037") && su->default_menu_w >= 512)
            v = (uint32_t)su->default_menu_w;
        else if (!strcmp(n, "0038") && su->default_menu_h >= 384)
            v = (uint32_t)su->default_menu_h;
        fprintf(f, "\"%s\"=dword:%08x\r\n", n, v);
    }
    fclose(f);
}

/* A DWORD of the display settings (settings.reg: "name"=dword:hex), or dflt. */
static uint32_t settings_dword(const char* path, const char* name, uint32_t dflt)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return dflt;
    char line[256], want[16];
    snprintf(want, sizeof want, "\"%s\"=dword:", name);
    uint32_t v = dflt;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, want, strlen(want)))
            v = (uint32_t)strtoul(line + strlen(want), NULL, 16);
    fclose(f);
    return v;
}

/* ---- the sign-in, on a worker thread ---- */

enum
{
    JOB_IDLE,
    JOB_RUNNING,
    JOB_DONE,
    JOB_FAILED,
};

typedef struct Job
{
    SDL_AtomicInt state;
    int method;
    char server[128], user[64], password[128], otp[32];
    uint16_t auth_port, data_port, view_port;
    uint32_t ip;
    char error[512];
} Job;

static Job g_job;

/* Keeps a PlayOnline session alive for the run: the server drops a member whose keepalives go
 * unanswered, and the lobby checks the session value against the live session. */
static void pol_keepalive(void* arg)
{
    PolSession* s = arg;
    while (pol_pump(s, 5000))
        ;
    fprintf(stderr, "[signin] the PlayOnline session closed: %s\n", s->error[0] ? s->error : "by the server");
}

static void job_thread(void* arg)
{
    Job* j = arg;
    int ok = 0;
    if (!net_resolve_ipv4(j->server, &j->ip))
        snprintf(j->error, sizeof j->error, "Cannot find the server \"%s\".", j->server);
    else if (j->method == SIGNIN_LSB)
    {
        LsbLogin l = { j->ip,
            j->auth_port ? j->auth_port : 54231,
            j->data_port ? j->data_port : 54230,
            j->view_port ? j->view_port : 54001,
            j->user,
            j->password,
            j->otp,
            NULL };
        ok = lsb_login(&l, j->error, sizeof j->error);
    }
    else
    {
        PolSession* s = calloc(1, sizeof *s);
        uint8_t v[16];
        if (!s)
            snprintf(j->error, sizeof j->error, "Out of memory.");
        else
        {
            s->irc = POL_NO_SOCKET;
            if (!pol_signin(s, j->server, POL_PORT_IRC, j->user, j->password) ||
                !pol_ffxi_session_value(s, POL_PORT_POLPRO, v))
            {
                snprintf(j->error, sizeof j->error, "%s", s->error[0] ? s->error : "The sign-in failed.");
                pol_signout(s);
                free(s);
            }
            else
            {
                polcore_set_session(v);
                ok = plat_thread_start(pol_keepalive, s);
                if (!ok)
                    snprintf(j->error, sizeof j->error, "Cannot keep the PlayOnline session open.");
            }
        }
    }
    memset(j->password, 0, sizeof j->password);
    if (ok)
        fprintf(stderr, "[signin] signed in as %s on %s (%s)\n", j->user, j->server,
            j->method == SIGNIN_POL ? "PlayOnline" : "LandSandBoat");
    else
        fprintf(stderr, "[signin] sign-in as %s on %s failed: %s\n", j->user, j->server, j->error);
    SDL_SetAtomicInt(&j->state, ok ? JOB_DONE : JOB_FAILED);
}

/* ---- the keychain, off the main thread: macOS asks the player whether this app may read or
 * change a saved password (again after every rebuild: an ad-hoc signature changes), and the
 * window has to keep drawing while it asks ---- */

typedef struct Keyjob
{
    SDL_AtomicInt state; /* 0 idle, 1 reading, 2 read */
    char key[256], password[128];
    int found;
} Keyjob;

static Keyjob g_keyread;

static void keyread_thread(void* arg)
{
    Keyjob* k = arg;
    k->found = keychain_get(k->key, k->password, sizeof k->password);
    SDL_SetAtomicInt(&k->state, 2);
}

typedef struct Keysave
{
    char key[256], password[128];
    int remember;
} Keysave;

static void keysave_thread(void* arg)
{
    Keysave* k = arg;
    if (k->remember)
        keychain_set(k->key, k->password);
    else
        keychain_delete(k->key);
    memset(k->password, 0, sizeof k->password);
    free(k);
}

/* ---- the screen ---- */

typedef struct Dat
{
    DatFile file;
    UiTexSet tex;
} Dat;

static int open_dat_at(const char* host_game, const char* rel, Dat* d, const char* category, const char* const* names)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", host_game, rel);
    memset(d, 0, sizeof *d);
    if (!dat_read(path, &d->file))
    {
        fprintf(stderr, "[signin] cannot read %s\n", path);
        return 0;
    }
    uidraw_load(&d->tex, &d->file, category, names);
    return 1;
}

static int open_dat(const char* host_game, int n, Dat* d, const char* category, const char* const* names)
{
    char rel[32];
    snprintf(rel, sizeof rel, "ROM/0/%d.DAT", n);
    return open_dat_at(host_game, rel, d, category, names);
}

static void close_dat(Dat* d)
{
    uidraw_free(&d->tex);
    dat_free(&d->file);
}

enum
{
    W_TEXT,
    W_SECRET,
    W_CHOICE,
    W_BUTTON,
};

enum
{
    SCREEN_SIGNIN,
    SCREEN_SETTINGS,
};

/* What a control does: its id, for the code that acts on it */
enum
{
    ID_USER,
    ID_PASSWORD,
    ID_OTP,
    ID_SIGNIN,
    ID_SETTINGS,
    ID_METHOD,
    ID_SERVER,
    ID_REMEMBER,
    ID_THEME,
    ID_BACK,
};

typedef struct Widget
{
    int kind, id;
    const char* label;
    char* text; /* W_TEXT / W_SECRET: the buffer */
    size_t cap;
    float x0, y0, x1, y1; /* where it was drawn, for the mouse */
} Widget;

typedef struct Ui
{
    Config cfg;
    char password[128], otp[32];
    int screen, focus;
    Widget w[8];
    int nw;
    char status[512];
    int status_error;
    Dat lobby, frame, fonts, menu, pc;
    UiTexSet backdrop; /* the player's picture, if any */
    UiTexSet art;      /* the screen's own (ui_art.h): the buttons */
    float backdrop_w, backdrop_h;
    DatSheet title, win00, lobbywin;
    int have_art; /* the PC title's art (lobbywin's titlwin) */
    UiFont font, ink; /* the game's font, and its glyphs' cores alone ("mojiink"): dark text */
    const char* host_game;
    int theme_loaded;
    Uint64 focus_time;
} Ui;

static const uint8_t WHITE[4] = { 255, 255, 255, 255 }, GREY[4] = { 150, 150, 160, 255 },
                     RED[4] = { 255, 140, 120, 255 }, GOLD[4] = { 240, 210, 130, 255 },
                     INK[4] = { 34, 42, 70, 255 }, INK_ON[4] = { 70, 44, 8, 255 };

static int load_theme(Ui* u)
{
    static const char* const THEME[] = { "newtex", "corner", "hfr1", "vfr1", NULL };
    int t = u->cfg.theme >= 1 && u->cfg.theme <= 8 ? u->cfg.theme : 1;
    if (u->theme_loaded == t)
        return 1;
    if (u->theme_loaded)
    {
        dat_sheet_free(&u->win00);
        close_dat(&u->frame);
    }
    u->theme_loaded = 0;
    if (!open_dat(u->host_game, 13 + t, &u->frame, "menu", THEME) || !dat_sheet(&u->frame.file, "win00", &u->win00))
        return 0;
    u->theme_loaded = t;
    return 1;
}

/* A button's pill (ui_art.h) at x, y, w by h: its shadow spills around that. Three slices: the
 * round caps (the image's padding and half its pill's height) at their size, the middle stretched. */
static void pill(Ui* u, float x, float y, float w, float h, int on)
{
    float k = h / (float)ui_art_pill_h, pad = ui_art_pad * k;
    float iw = (float)(ui_art_pill_w + 2 * ui_art_pad), ih = (float)(ui_art_pill_h + 2 * ui_art_pad);
    float cap = (float)(ui_art_pad + ui_art_pill_h / 2), capw = cap * k;
    if (w < h)
        w = h;
    float X0 = x - pad, X1 = x + w + pad, Y0 = y - pad, Y1 = y + h + pad;
    UiQuad q[3];
    memset(q, 0, sizeof q);
    float xs[4] = { X0, X0 + capw, X1 - capw, X1 }, us[4] = { 0, cap, iw - cap, iw };
    for (int i = 0; i < 3; ++i)
    {
        q[i].x0 = xs[i], q[i].x1 = xs[i + 1], q[i].y0 = Y0, q[i].y1 = Y1;
        q[i].u0 = us[i], q[i].u1 = us[i + 1], q[i].v0 = 0, q[i].v1 = ih;
        memset(q[i].color, 255, sizeof q[i].color);
        q[i].image = on ? "button_on" : "button";
    }
    uidraw_quads(&u->art, q, 3);
}

/* Text with the game's drop shadow (dark text, on the light buttons, a faint light one); menu
 * text (the buttons') is also bold and italic, as the game's menus draw the same font: slanted a
 * fifth of its height, struck twice a pixel apart. */
static void styled(Ui* u, const char* s, float x, float y, float scale, const uint8_t rgba[4], int menu)
{
    static const uint8_t DARK[4] = { 0, 0, 0, 200 }, LIGHT[4] = { 255, 255, 255, 110 };
    const uint8_t* SHADOW = rgba[0] + rgba[1] + rgba[2] < 384 ? LIGHT : DARK;
    /* the glyphs' cores (ink), so white is white: the font's own are grey (173) inside an outline */
    UiQuad q[128];
    for (int pass = 0; pass < (menu ? 3 : 2); ++pass)
    {
        float dx = pass == 0 ? scale : pass == 2 ? scale * 0.75f : 0, dy = pass == 0 ? scale : 0;
        unsigned n = ui_text(&u->ink, s, x + dx, y + dy, scale, pass ? rgba : SHADOW, q, 128);
        if (menu)
            for (unsigned i = 0; i < n; ++i)
                q[i].skew = u->ink.cell * scale * 0.2f;
        uidraw_quads(&u->fonts.tex, q, n);
    }
}

static void text(Ui* u, const char* s, float x, float y, float scale, const uint8_t rgba[4])
{
    styled(u, s, x, y, scale, rgba, 0);
}

static void frame(Ui* u, float x, float y, float w, float h, float scale, int focused)
{
    UiQuad q[UI_WINDOW_QUADS];
    uidraw_quads(&u->frame.tex, q, ui_window(&u->win00, x, y, w, h, scale, focused, q));
}

static void add(Ui* u, int kind, int id, const char* label, char* buf, size_t cap)
{
    Widget* w = &u->w[u->nw++];
    memset(w, 0, sizeof *w);
    w->kind = kind, w->id = id, w->label = label, w->text = buf, w->cap = cap;
}

/* The controls of the current screen, in focus order */
static void build(Ui* u)
{
    u->nw = 0;
    if (u->screen == SCREEN_SIGNIN)
    {
        add(u, W_TEXT, ID_USER, u->cfg.method == SIGNIN_POL ? "PlayOnline ID" : "Username", u->cfg.user,
            sizeof u->cfg.user);
        add(u, W_SECRET, ID_PASSWORD, "Password", u->password, sizeof u->password);
        if (u->cfg.method == SIGNIN_LSB)
            add(u, W_TEXT, ID_OTP, "One-time code", u->otp, sizeof u->otp);
        add(u, W_BUTTON, ID_SIGNIN, "Sign in", NULL, 0);
        add(u, W_BUTTON, ID_SETTINGS, "Settings", NULL, 0);
    }
    else
    {
        add(u, W_CHOICE, ID_METHOD, "Sign-in method", NULL, 0);
        add(u, W_TEXT, ID_SERVER, "Server", u->cfg.server, sizeof u->cfg.server);
        add(u, W_CHOICE, ID_REMEMBER, "Remember password", NULL, 0);
        add(u, W_CHOICE, ID_THEME, "Window theme", NULL, 0);
        add(u, W_BUTTON, ID_BACK, "Back", NULL, 0);
    }
    if (u->focus >= u->nw)
        u->focus = u->nw - 1;
}

static const char* choice_text(const Ui* u, int id, char* buf, size_t n)
{
    if (id == ID_METHOD)
        return u->cfg.method == SIGNIN_POL ? "PlayOnline" : "LandSandBoat";
    if (id == ID_REMEMBER)
        return u->cfg.remember ? "Yes" : "No";
    snprintf(buf, n, "%d", u->cfg.theme);
    return buf;
}

static int busy(void)
{
    return SDL_GetAtomicInt(&g_job.state) == JOB_RUNNING;
}

static void set_status(Ui* u, const char* s, int error)
{
    snprintf(u->status, sizeof u->status, "%s", s);
    u->status_error = error;
}

static void start_signin(Ui* u)
{
    if (busy())
        return;
    if (!u->cfg.server[0])
    {
        set_status(u, "Set the server in Settings first.", 1);
        return;
    }
    if (!u->cfg.user[0] || !u->password[0])
    {
        set_status(u, u->cfg.method == SIGNIN_POL ? "Enter your PlayOnline ID and password." :
                                                    "Enter your username and password.", 1);
        u->focus = !u->cfg.user[0] ? 0 : 1;
        return;
    }
    if (u->cfg.method == SIGNIN_POL)
    {
        /* PlayOnline IDs are upper case */
        for (char* p = u->cfg.user; *p; ++p)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 32);
        char nick[10];
        if (!pol_nick_from_id(u->cfg.user, nick))
        {
            set_status(u, "That is not a PlayOnline ID (8 letters and digits).", 1);
            u->focus = 0;
            return;
        }
    }
    Job* j = &g_job;
    j->method = u->cfg.method;
    SDL_strlcpy(j->server, u->cfg.server, sizeof j->server);
    SDL_strlcpy(j->user, u->cfg.user, sizeof j->user);
    SDL_strlcpy(j->password, u->password, sizeof j->password);
    SDL_strlcpy(j->otp, u->otp, sizeof j->otp);
    j->auth_port = u->cfg.auth_port, j->data_port = u->cfg.data_port, j->view_port = u->cfg.view_port;
    j->error[0] = 0;
    SDL_SetAtomicInt(&j->state, JOB_RUNNING);
    fprintf(stderr, "[signin] signing in as %s on %s (%s)\n", j->user, j->server,
        j->method == SIGNIN_POL ? "PlayOnline" : "LandSandBoat");
    if (!plat_thread_start(job_thread, j))
    {
        snprintf(j->error, sizeof j->error, "Cannot start the sign-in.");
        SDL_SetAtomicInt(&j->state, JOB_FAILED);
    }
    set_status(u, "", 0);
}

static void cycle(Ui* u, int id, int dir)
{
    if (id == ID_METHOD)
    {
        u->cfg.method = u->cfg.method == SIGNIN_POL ? SIGNIN_LSB : SIGNIN_POL;
        u->password[0] = 0; /* another account's */
    }
    else if (id == ID_REMEMBER)
        u->cfg.remember = !u->cfg.remember;
    else if (id == ID_THEME)
    {
        u->cfg.theme = (u->cfg.theme - 1 + dir + 8) % 8 + 1;
        if (!load_theme(u))
            set_status(u, "That window theme did not load.", 1);
    }
}

static void activate(Ui* u, int i)
{
    if (i < 0 || i >= u->nw)
        return;
    Widget* w = &u->w[i];
    if (w->kind == W_CHOICE)
        cycle(u, w->id, 1);
    else if (w->id == ID_SETTINGS && !busy())
        u->screen = SCREEN_SETTINGS, u->focus = 0, set_status(u, "", 0);
    else if (w->id == ID_BACK)
        u->screen = SCREEN_SIGNIN, u->focus = 0;
    else if (u->screen == SCREEN_SIGNIN)
        start_signin(u);
}

static void type(Ui* u, const char* s)
{
    if (u->focus < 0 || u->focus >= u->nw || busy())
        return;
    Widget* w = &u->w[u->focus];
    if (w->kind != W_TEXT && w->kind != W_SECRET)
        return;
    size_t len = strlen(w->text);
    for (; *s && len + 1 < w->cap; ++s)
        if (*s >= ' ' && *s <= '~') /* the font's ASCII */
            w->text[len++] = *s;
    w->text[len] = 0;
    u->focus_time = SDL_GetTicks();
}

/* Draws the screen and records where each control is */
static void draw(Ui* u, int w, int h)
{
    float s = (float)h / 720.0f, L = 1.25f * s;
    UiQuad q[96];
    if (u->backdrop.n)
    {
        /* the player's picture, covering the window, a touch darker toward the bottom */
        float k = w / u->backdrop_w > h / u->backdrop_h ? w / u->backdrop_w : h / u->backdrop_h;
        float bw = u->backdrop_w * k, bh = u->backdrop_h * k, bx = (w - bw) * 0.5f, by = (h - bh) * 0.5f;
        UiQuad b = { bx, by, bx + bw, by + bh, 0, 0, u->backdrop_w, u->backdrop_h,
            { { 235, 235, 235, 255 }, { 235, 235, 235, 255 }, { 220, 220, 220, 255 }, { 220, 220, 220, 255 } },
            "backdrop", 0 };
        uidraw_quads(&u->backdrop, &b, 1);
    }
    int rows = 0;
    for (int i = 0; i < u->nw; ++i)
        rows += u->w[i].kind != W_BUTTON;
    float row = 50 * s, ww = 540 * s, wh = (60 + 30 + 58) * s + rows * row, wx, wy;
    if (u->have_art)
    {
        /* The PC title (lobbywin sprite 1): Amano's warriors over the logo, centred above the
         * window, as large as fits; its copyright line along the bottom. The window's top stays
         * where the tallest screen (Settings: 4 rows) needs it, so the art does not move. */
        const DatSprite* t = &u->lobbywin.sprites[1];
        const DatPart* art = NULL;
        DatPart words[8];
        DatSprite line = { 0, words };
        for (uint32_t i = 0; i < t->nparts; ++i)
            if (!strcmp(t->parts[i].name, "titlwin"))
            {
                if (t->parts[i].uw >= 1024)
                    art = &t->parts[i];
                else if (line.nparts < 8)
                    words[line.nparts++] = t->parts[i];
            }
        float tallest = (60 + 30 + 58) * s + 4 * row;
        wx = (w - ww) * 0.5f, wy = h - 50 * s - tallest;
        float room_w = w - 80 * s, room_h = wy - 24 * s, k = room_w / art->uw;
        if (art->uh * k > room_h)
            k = room_h / art->uh;
        float aw = art->uw * k, ah = art->uh * k, ax = (w - aw) * 0.5f, ay = 12 * s + (room_h - ah) * 0.5f;
        UiQuad a = { ax, ay, ax + aw, ay + ah, art->u, art->v, art->u + art->uw, art->v + art->uh,
            { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } },
            "titlwin", 0 };
        uidraw_quads(&u->pc.tex, &a, 1);
        /* the copyright's pieces, as the sheet lays them out, centred at the bottom */
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f;
        for (uint32_t i = 0; i < line.nparts; ++i)
            for (int v = 0; v < 4; ++v)
            {
                minx = line.parts[i].x[v] < minx ? line.parts[i].x[v] : minx;
                maxx = line.parts[i].x[v] > maxx ? line.parts[i].x[v] : maxx;
                miny = line.parts[i].y[v] < miny ? line.parts[i].y[v] : miny;
            }
        float cs = 1.2f * s;
        unsigned n = ui_sprite(&line, (w - (maxx - minx) * cs) * 0.5f - minx * cs, h - 34 * s - miny * cs, cs, q, 96);
        uidraw_quads(&u->pc.tex, q, n);
    }
    else
    {
        /* the PS2 lobby's title: its logo at the top and its copyright line at the bottom */
        unsigned n = ui_sprite(&u->title.sprites[1], w * 0.5f + 96 * L, 100 * s + 100 * L, L, q, 96), keep = 0;
        for (unsigned i = 0; i < n; ++i)
            if (!strcmp(q[i].image, "xilogo") && q[i].y1 < 100 * s + 100 * L)
                q[keep++] = q[i];
        uidraw_quads(&u->lobby.tex, q, keep);
        float base = h - 34 * s - 104 * s;
        n = ui_sprite(&u->title.sprites[1], w * 0.5f, base, s, q, 96), keep = 0;
        for (unsigned i = 0; i < n; ++i)
            if (!strcmp(q[i].image, "xilogo") && q[i].y0 > base + 90 * s)
                q[keep++] = q[i];
        uidraw_quads(&u->lobby.tex, q, keep);
        wx = (w - ww) * 0.5f, wy = 230 * s;
    }

    /* the window: a heading, a row a field or choice, the status, the buttons */
    frame(u, wx, wy, ww, wh, s, 1);
    text(u, u->screen == SCREEN_SIGNIN ? "Sign in" : "Settings", wx + 24 * s, wy + 18 * s, s, WHITE);
    if (u->screen == SCREEN_SIGNIN)
    {
        char where[200];
        snprintf(where, sizeof where, "%s  %s", u->cfg.method == SIGNIN_POL ? "PlayOnline" : "LandSandBoat",
            u->cfg.server[0] ? u->cfg.server : "(no server)");
        text(u, where, wx + ww - 24 * s - ui_text_width(&u->font, where, s), wy + 18 * s, s, GREY);
    }

    Uint64 now = SDL_GetTicks();
    float fx = wx + 220 * s, fw = ww - 244 * s, y = wy + 52 * s;
    for (int i = 0; i < u->nw; ++i)
    {
        Widget* c = &u->w[i];
        int focused = i == u->focus;
        if (c->kind == W_BUTTON)
            continue;
        text(u, c->label, wx + 34 * s, y + 12 * s, s, focused ? GOLD : WHITE);
        frame(u, fx, y, fw, 40 * s, 0.5f * s, focused);
        c->x0 = fx, c->y0 = y, c->x1 = fx + fw, c->y1 = y + 40 * s;
        float tx = fx + 14 * s, room = fw - 28 * s;
        if (c->kind == W_CHOICE)
        {
            char buf[16];
            const char* v = choice_text(u, c->id, buf, sizeof buf);
            float tw = ui_text_width(&u->font, v, s);
            text(u, "<", fx + 14 * s, y + 12 * s, s, focused ? GOLD : GREY);
            text(u, ">", fx + fw - 24 * s, y + 12 * s, s, focused ? GOLD : GREY);
            text(u, v, fx + (fw - tw) * 0.5f, y + 12 * s, s, WHITE);
        }
        else
        {
            char shown[160];
            size_t len = strlen(c->text);
            if (c->kind == W_SECRET)
            {
                len = len < sizeof shown - 1 ? len : sizeof shown - 1;
                memset(shown, '*', len);
                shown[len] = 0;
            }
            else
                SDL_strlcpy(shown, c->text, sizeof shown);
            /* the end of a long entry, as fields scroll */
            const char* v = shown;
            while (*v && ui_text_width(&u->font, v, s) > room)
                ++v;
            text(u, v, tx, y + 12 * s, s, WHITE);
            if (focused && !busy() && (now - u->focus_time) / 500 % 2 == 0)
            {
                float cx = tx + ui_text_width(&u->font, v, s) + 1 * s;
                uidraw_rect(cx, y + 10 * s, cx + 2 * s, y + 30 * s, 0xFFE0E0E0u);
            }
        }
        y += row;
    }

    /* the status line: the sign-in's progress, or what went wrong */
    if (busy())
    {
        char msg[64];
        snprintf(msg, sizeof msg, "Signing in%.*s", (int)(now / 400 % 4), "...");
        text(u, msg, wx + 34 * s, y + 6 * s, s, GREY);
    }
    else if (u->status[0])
    {
        /* long messages: as much as fits */
        char msg[512];
        SDL_strlcpy(msg, u->status, sizeof msg);
        for (size_t len = strlen(msg); len > 0 && ui_text_width(&u->font, msg, s) > ww - 58 * s; --len)
            msg[len - 1] = 0;
        text(u, msg, wx + 34 * s, y + 6 * s, s, u->status_error ? RED : GREY);
    }

    /* the buttons, right-aligned along the bottom: the screen's own pills (light, gold for the one
     * Enter presses) with dark labels */
    float bh = 30 * s, bw = 170 * s, by = wy + wh - 52 * s, bx = wx + ww - 24 * s;
    for (int i = u->nw - 1; i >= 0; --i)
    {
        Widget* c = &u->w[i];
        if (c->kind != W_BUTTON)
            continue;
        bx -= bw;
        int focused = i == u->focus;
        pill(u, bx, by, bw, bh, focused);
        /* the label, centred, in the font's cores: dark ink on the light pill, struck twice for weight */
        float tw = ui_text_width(&u->ink, c->label, s), tx = bx + (bw - tw) * 0.5f, ty = by + (bh - 16 * s) * 0.5f;
        UiQuad q2[64];
        for (int pass = 0; pass < 2; ++pass)
            uidraw_quads(&u->fonts.tex, q2, ui_text(&u->ink, c->label, tx + pass * 0.6f * s, ty, s, focused ? INK_ON : INK, q2, 64));
        c->x0 = bx, c->y0 = by, c->x1 = bx + bw, c->y1 = by + bh;
        bx -= 44 * s; /* room for the pointing hand */
    }

    /* the game's pointing hand at the focused control */
    if (u->focus >= 0 && u->focus < u->nw)
    {
        Widget* c = &u->w[u->focus];
        float hs = 28 * s, hx = c->kind == W_BUTTON ? c->x0 - hs - 4 * s : wx + 6 * s,
              hy = (c->y0 + c->y1) * 0.5f - hs * 0.5f;
        UiQuad hand = { hx, hy, hx + hs, hy + hs, 0, 0, 32, 32,
            { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } },
            "yubi" };
        uidraw_quads(&u->menu.tex, &hand, 1);
    }
}

static int hit(const Ui* u, float x, float y)
{
    for (int i = 0; i < u->nw; ++i)
        if (x >= u->w[i].x0 && x < u->w[i].x1 && y >= u->w[i].y0 && y < u->w[i].y1)
            return i;
    return -1;
}

static void key(Ui* u, const SDL_KeyboardEvent* e, int* done)
{
    SDL_Keycode k = e->key;
    int shift = (e->mod & SDL_KMOD_SHIFT) != 0, cmd = (e->mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
    Widget* f = u->focus >= 0 && u->focus < u->nw ? &u->w[u->focus] : NULL;
    u->focus_time = SDL_GetTicks();
    /* a held key repeats: fine for moving and deleting, not for acting */
    if (e->repeat && (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE))
        return;
    if (k == SDLK_ESCAPE)
    {
        if (u->screen == SCREEN_SETTINGS)
            u->screen = SCREEN_SIGNIN, u->focus = 0;
        else if (!busy())
            *done = 1;
    }
    else if (k == SDLK_TAB || k == SDLK_DOWN || k == SDLK_UP)
    {
        int back = k == SDLK_UP || (k == SDLK_TAB && shift);
        u->focus = (u->focus + (back ? u->nw - 1 : 1)) % u->nw;
    }
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
    {
        /* in a field of the sign-in screen: sign in; in Settings' server: on to the next */
        if (f && (f->kind == W_TEXT || f->kind == W_SECRET) && u->screen == SCREEN_SIGNIN)
            start_signin(u);
        else if (f && f->kind == W_TEXT)
            u->focus = (u->focus + 1) % u->nw;
        else
            activate(u, u->focus);
    }
    else if (f && f->kind == W_CHOICE && (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_SPACE))
        cycle(u, f->id, k == SDLK_LEFT ? -1 : 1);
    else if (f && f->kind == W_BUTTON && k == SDLK_SPACE)
        activate(u, u->focus);
    else if (f && f->text && k == SDLK_BACKSPACE && !busy())
    {
        size_t len = strlen(f->text);
        f->text[cmd ? 0 : len ? len - 1 : 0] = 0;
    }
    else if (f && f->text && k == SDLK_V && cmd)
    {
        char* clip = SDL_GetClipboardText();
        if (clip)
            type(u, clip);
        SDL_free(clip);
    }
}

int signin_run(const SigninSetup* setup, SigninResult* out)
{
    memset(out, 0, sizeof *out);
    Ui* u = calloc(1, sizeof *u);
    if (!u)
        return -1;
    u->host_game = setup->host_game;
    /* full screen in place, as on Windows: not a macOS Space sliding in (user32 says the same) */
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        fprintf(stderr, "[signin] SDL_Init: %s\n", SDL_GetError());
        free(u);
        return -1;
    }

    /* where it keeps its files */
    char dir[1024], cfg_path[1100];
    if (setup->data_dir)
        SDL_strlcpy(dir, setup->data_dir, sizeof dir);
    else
    {
        char* pref = SDL_GetPrefPath("FFXIRecompile", "FFXI");
        SDL_strlcpy(dir, pref ? pref : ".", sizeof dir);
        SDL_free(pref);
    }
    SDL_strlcpy(out->data_dir, dir, sizeof out->data_dir);
    config_path(dir, "signin.cfg", cfg_path, sizeof cfg_path);
    config_path(dir, "settings.reg", out->settings_reg, sizeof out->settings_reg);
    default_settings(out->settings_reg, setup);

    Config* c = &u->cfg;
    c->method = setup->default_method ? setup->default_method : SIGNIN_LSB, c->remember = 1, c->theme = 1;
    SDL_strlcpy(c->server, setup->default_server ? setup->default_server : "127.0.0.1", sizeof c->server);
    config_load(cfg_path, c);
    if (setup->method)
        c->method = setup->method;
    if (setup->server)
        SDL_strlcpy(c->server, setup->server, sizeof c->server);
    if (setup->user)
        SDL_strlcpy(c->user, setup->user, sizeof c->user);
    if (setup->auth_port)
        c->auth_port = setup->auth_port;
    if (setup->data_port)
        c->data_port = setup->data_port;
    if (setup->view_port)
        c->view_port = setup->view_port;
    int read_keychain = 0;
    if (setup->password)
        SDL_strlcpy(u->password, setup->password, sizeof u->password);
    else if (c->remember && c->user[0])
    {
        keychain_key(c, g_keyread.key, sizeof g_keyread.key);
        read_keychain = 1; /* once the window is up */
    }
    if (setup->otp)
        SDL_strlcpy(u->otp, setup->otp, sizeof u->otp);

    /* the window the game will make of it (settings.reg's 0034 window mode, 0001 x 0002): full
     * screen (0) and borderless full screen (3) cover the display in its own mode, borderless (2)
     * has no frame, windowed (1) does - so the game takes it over without a jump */
    uint32_t mode = settings_dword(out->settings_reg, "0034", 1);
    int ww = (int)settings_dword(out->settings_reg, "0001", 1280), wh = (int)settings_dword(out->settings_reg, "0002", 720);
    if (ww < 640 || wh < 480)
        ww = 1280, wh = 720;
    SDL_Window* win = SDL_CreateWindow("FINAL FANTASY XI", ww, wh, mode >= 2 ? SDL_WINDOW_BORDERLESS : 0);
    if (win && (mode == 0 || mode == 3))
    {
        SDL_SetWindowFullscreenMode(win, NULL);
        SDL_SetWindowFullscreen(win, true);
    }
    if (!win || !uidraw_open(win))
    {
        fprintf(stderr, "[signin] no window to draw in: %s\n", SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        free(u);
        return -1;
    }

    static const char* const LOBBY[] = { "xilogo", NULL };
    static const char* const FONT[] = { "moji", NULL };
    static const char* const MENU[] = { "yubi", "buttonto", NULL };
    DatImage moji;
    int ok = open_dat(u->host_game, 2, &u->lobby, "menu", LOBBY);
    ok = open_dat(u->host_game, 1, &u->fonts, "font", FONT) && ok;
    if (ok)
        uidraw_load(&u->menu.tex, &u->fonts.file, "menu", MENU);
    ok = ok && load_theme(u);
    ok = ok && dat_sheet(&u->lobby.file, "lobbyps2", &u->title) && u->title.nsprites > 1;
    ok = ok && dat_image(&u->fonts.file, "font", "moji", &moji);
    /* the screen's own art */
    for (const UiArt* a = ui_art; a->name; ++a)
    {
        int aw, ah, comp;
        uint8_t* px = stbi_load_from_memory(a->png, (int)a->size, &aw, &ah, &comp, 4);
        if (px)
            uidraw_load_rgba(&u->art, a->name, px, (uint32_t)aw, (uint32_t)ah);
        stbi_image_free(px);
    }
    /* the player's background picture: the one signin.cfg names, else background.* beside it */
    {
        char pic[1100] = "";
        static const char* const NAMES[] = { "background.png", "background.jpg", "background.jpeg", "background.bmp" };
        if (c->background[0])
            SDL_strlcpy(pic, c->background, sizeof pic);
        for (size_t i = 0; !pic[0] && i < sizeof NAMES / sizeof *NAMES; ++i)
        {
            char p[1100];
            config_path(dir, NAMES[i], p, sizeof p);
            FILE* f = fopen(p, "rb");
            if (f)
            {
                fclose(f);
                SDL_strlcpy(pic, p, sizeof pic);
            }
        }
        if (!pic[0] && setup->default_background)
            SDL_strlcpy(pic, setup->default_background, sizeof pic);
        int pw, ph, comp;
        uint8_t* px = pic[0] ? stbi_load(pic, &pw, &ph, &comp, 4) : NULL;
        if (px && pw <= 16384 && ph <= 16384 && uidraw_load_rgba(&u->backdrop, "backdrop", px, (uint32_t)pw, (uint32_t)ph))
            u->backdrop_w = (float)pw, u->backdrop_h = (float)ph;
        else if (pic[0])
            fprintf(stderr, "[signin] cannot load the background %s: %s\n", pic, px ? "too large" : stbi_failure_reason());
        stbi_image_free(px);
    }
    /* the PC client's title art, when its lobby DAT is there (the English one; others have it too) */
    static const char* const PC[] = { "titlwin", NULL };
    if (ok && open_dat_at(u->host_game, "ROM/119/50.DAT", &u->pc, "menu", PC))
    {
        u->have_art = u->pc.tex.n == 1 && dat_sheet(&u->pc.file, "lobbywin", &u->lobbywin) && u->lobbywin.nsprites > 1;
        for (uint32_t i = 0; u->have_art && i < u->lobbywin.sprites[1].nparts; ++i)
            if (!strcmp(u->lobbywin.sprites[1].parts[i].name, "titlwin") && u->lobbywin.sprites[1].parts[i].uw >= 1024)
                u->have_art = 2;
        u->have_art = u->have_art == 2;
        if (!u->have_art)
            fprintf(stderr, "[signin] ROM/119/50.DAT is not the lobby it was: the PS2 title instead\n");
    }
    if (ok)
    {
        ok = ui_font(&moji, &u->font);
        /* the glyphs' cores: the font's white (173) without its baked dark outline (under 60), as
         * the alpha of white, so it takes any colour cleanly */
        for (size_t i = 0; i < (size_t)moji.w * moji.h; ++i)
        {
            uint8_t* p = moji.rgba + i * 4;
            int a = (p[0] - 60) * 255 / (173 - 60);
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            p[3] = (uint8_t)(a * p[3] / 255);
            p[0] = p[1] = p[2] = 255;
        }
        u->ink = u->font;
        u->ink.image = "mojiink";
        uidraw_load_rgba(&u->fonts.tex, "mojiink", moji.rgba, moji.w, moji.h);
        dat_image_free(&moji);
    }
    if (!ok)
    {
        fprintf(stderr, "[signin] the install's UI art did not load (is --game the FINAL FANTASY XI folder?)\n");
        uidraw_close();
        SDL_DestroyWindow(win);
        free(u);
        return -1;
    }

    /* start where there is something to type */
    build(u);
    u->focus = !c->user[0] ? 0 : !u->password[0] ? 1 : c->method == SIGNIN_LSB ? 3 : 2;
    SDL_StartTextInput(win);
    if (read_keychain)
    {
        SDL_SetAtomicInt(&g_keyread.state, 1);
        if (!plat_thread_start(keyread_thread, &g_keyread))
            SDL_SetAtomicInt(&g_keyread.state, 0);
    }
    Uint64 opened = SDL_GetTicks();
    u->focus_time = SDL_GetTicks();
    int result = 0;
    for (int done = 0; !done;)
    {
        build(u);
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                done = 1;
            else if (e.type == SDL_EVENT_TEXT_INPUT)
                type(u, e.text.text);
            else if (e.type == SDL_EVENT_KEY_DOWN)
                key(u, &e.key, &done);
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
            {
                float d = SDL_GetWindowPixelDensity(win);
                int i = hit(u, e.button.x * d, e.button.y * d);
                if (i >= 0)
                {
                    u->focus = i, u->focus_time = SDL_GetTicks();
                    if (u->w[i].kind == W_BUTTON || u->w[i].kind == W_CHOICE)
                        activate(u, i);
                }
            }
            build(u);
        }

        /* the saved password, when macOS has let us read it: into an empty field */
        if (SDL_GetAtomicInt(&g_keyread.state) == 2)
        {
            if (g_keyread.found && !u->password[0])
            {
                SDL_strlcpy(u->password, g_keyread.password, sizeof u->password);
                if (u->screen == SCREEN_SIGNIN && u->focus == 1)
                    u->focus = 2; /* on to the code (LandSandBoat) or Sign in (PlayOnline) */
            }
            memset(g_keyread.password, 0, sizeof g_keyread.password);
            SDL_SetAtomicInt(&g_keyread.state, 0);
            if (!u->status_error)
                set_status(u, "", 0);
        }
        else if (SDL_GetAtomicInt(&g_keyread.state) == 1 && SDL_GetTicks() - opened > 1500 && !u->status[0])
            set_status(u, "Waiting for macOS to allow the saved password...", 0);
        int state = SDL_GetAtomicInt(&g_job.state);
        if (state == JOB_FAILED)
        {
            SDL_SetAtomicInt(&g_job.state, JOB_IDLE);
            set_status(u, g_job.error[0] ? g_job.error : "The sign-in failed.", 1);
            u->otp[0] = 0; /* one-time codes are single use */
        }
        else if (state == JOB_DONE)
        {
            /* remembered (or forgotten) off this thread: macOS may ask first */
            Keysave* ks = calloc(1, sizeof *ks);
            if (ks)
            {
                keychain_key(c, ks->key, sizeof ks->key);
                SDL_strlcpy(ks->password, u->password, sizeof ks->password);
                ks->remember = c->remember;
                if (!plat_thread_start(keysave_thread, ks))
                    free(ks);
            }
            out->server = g_job.ip;
            result = done = 1;
        }

        int w, h;
        uidraw_begin(0xFF000000u, &w, &h);
        draw(u, w, h);
        uidraw_end();
    }
    config_save(cfg_path, c);

    SDL_StopTextInput(win);
    memset(u->password, 0, sizeof u->password);
    dat_sheet_free(&u->title);
    dat_sheet_free(&u->lobbywin);
    uidraw_free(&u->backdrop);
    uidraw_free(&u->art);
    close_dat(&u->pc);
    if (u->theme_loaded)
    {
        dat_sheet_free(&u->win00);
        close_dat(&u->frame);
    }
    uidraw_free(&u->menu.tex);
    close_dat(&u->lobby);
    close_dat(&u->fonts);
    uidraw_close();
    free(u);
    if (!result)
    {
        SDL_DestroyWindow(win);
        return 0;
    }
    out->window = win;
    return 1;
}
