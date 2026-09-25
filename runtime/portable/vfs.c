/* Guest paths to host paths. See vfs.h. */
#include <stdio.h>
#include <string.h>

#include "plat.h"
#include "vfs.h"

#define MAX_MOUNTS 16

static char g_cwd[512] = "C:\\";
static struct
{
    char guest[256];
    char host[512];
} g_mounts[MAX_MOUNTS];
static unsigned g_nmounts;

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

void vfs_init(const char* guest_cwd)
{
    char full[512];
    if (guest_cwd && vfs_full_path(guest_cwd, full, sizeof full))
        snprintf(g_cwd, sizeof g_cwd, "%s", full);
}

void vfs_mount(const char* guest_prefix, const char* host_prefix)
{
    if (g_nmounts < MAX_MOUNTS)
    {
        vfs_full_path(guest_prefix, g_mounts[g_nmounts].guest, sizeof g_mounts[0].guest);
        snprintf(g_mounts[g_nmounts].host, sizeof g_mounts[0].host, "%s", host_prefix);
        g_nmounts++;
    }
}

/* Appends one path onto an absolute one ("C:\a\b"), resolving ".", "..", empty components and
 * both separators. The drive root keeps its trailing backslash ("C:\"); nothing else does. */
static int join(char* out, size_t n, const char* base, const char* rel)
{
    char buf[1024];
    size_t len = strlen(base);
    if (len >= sizeof buf)
        return 0;
    memcpy(buf, base, len + 1);
    if (len > 3 && buf[len - 1] == '\\')
        buf[--len] = 0;
    const char* p = rel;
    while (*p)
    {
        while (*p == '\\' || *p == '/')
            p++;
        const char* e = p;
        while (*e && *e != '\\' && *e != '/')
            e++;
        size_t c = (size_t)(e - p);
        if (c == 0)
            break;
        if (c == 1 && p[0] == '.')
        {
        }
        else if (c == 2 && p[0] == '.' && p[1] == '.')
        {
            char* s = strrchr(buf, '\\');
            if (s && s > buf + 2)
                *s = 0, len = (size_t)(s - buf);
            else
                buf[3] = 0, len = 3; /* stays at the root */
        }
        else
        {
            if (len + c + 2 >= sizeof buf)
                return 0;
            if (buf[len - 1] != '\\')
                buf[len++] = '\\';
            memcpy(buf + len, p, c);
            len += c;
            buf[len] = 0;
        }
        p = e;
    }
    if (strlen(buf) + 1 > n)
        return 0;
    strcpy(out, buf);
    return 1;
}

int vfs_full_path(const char* guest, char* out, size_t n)
{
    char root[4] = "C:\\";
    if (((guest[0] >= 'A' && guest[0] <= 'Z') || (guest[0] >= 'a' && guest[0] <= 'z')) && guest[1] == ':')
    {
        root[0] = (char)(guest[0] & ~0x20);
        if (guest[2] == '\\' || guest[2] == '/')
            return join(out, n, root, guest + 2);
        /* "C:foo" - relative to that drive's current directory; there is only one */
        return join(out, n, g_cwd[0] == root[0] ? g_cwd : root, guest + 2);
    }
    if (guest[0] == '\\' || guest[0] == '/')
    {
        root[0] = g_cwd[0];
        return join(out, n, root, guest);
    }
    return join(out, n, g_cwd, guest);
}

/* Guest strings are code page 1252: bytes 0x80-0xFF become their Unicode code points in UTF-8
 * (the 0x80-0x9F block of 1252 is not special-cased yet, as in k32.c). */
static int to_utf8(const char* in, char* out, size_t n)
{
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p)
    {
        if (*p < 0x80)
        {
            if (o + 1 >= n)
                return 0;
            out[o++] = (char)*p;
        }
        else
        {
            if (o + 2 >= n)
                return 0;
            out[o++] = (char)(0xC0 | (*p >> 6));
            out[o++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    out[o] = 0;
    return 1;
}

int vfs_host_path(const char* guest, char* host, size_t n)
{
    char full[1024], mapped[1400];
    if (!vfs_full_path(guest, full, sizeof full))
        return 0;
    int best = -1;
    size_t best_len = 0;
    for (unsigned i = 0; i < g_nmounts; ++i)
    {
        size_t l = strlen(g_mounts[i].guest);
        int match = l > best_len && strlen(full) >= l && (full[l] == 0 || full[l] == '\\' || g_mounts[i].guest[l - 1] == '\\');
        for (size_t k = 0; match && k < l; ++k)
            match = lower((unsigned char)full[k]) == lower((unsigned char)g_mounts[i].guest[k]);
        if (match)
            best = (int)i, best_len = l;
    }
    if (best >= 0)
        snprintf(mapped, sizeof mapped, "%s%s", g_mounts[best].host, full + best_len);
    else if (plat_path_sep == '\\')
        snprintf(mapped, sizeof mapped, "%s", full); /* a Windows host: the guest path is the host path */
    else
        return 0;
    if (plat_path_sep != '\\')
        for (char* p = mapped; *p; ++p)
            if (*p == '\\')
                *p = plat_path_sep;
    return to_utf8(mapped, host, n);
}

const char* vfs_cwd(void)
{
    return g_cwd;
}

int vfs_set_cwd(const char* guest)
{
    char full[512], host[1400];
    PlatStat st;
    if (!vfs_full_path(guest, full, sizeof full) || !vfs_host_path(full, host, sizeof host) || !plat_stat(host, &st) || !st.is_dir)
        return 0;
    strcpy(g_cwd, full);
    return 1;
}
