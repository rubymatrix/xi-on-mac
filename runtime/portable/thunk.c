/* Synthetic import addresses and shim dispatch. See thunk.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thunk.h"

#if defined(_MSC_VER)
#define stricmp_ _stricmp
#else
#include <strings.h>
#define stricmp_ strcasecmp
#endif

typedef struct Thunk
{
    char name[96]; /* DLL!name */
    Shim shim;
} Thunk;

static Thunk* g_thunks;
static unsigned g_count, g_cap;
static const ShimDef* g_tables[32];
static unsigned g_ntables;

void thunk_register(const ShimDef* defs)
{
    if (g_ntables < sizeof g_tables / sizeof g_tables[0])
        g_tables[g_ntables++] = defs;
}

static Shim find_shim(const char* dll, const char* name)
{
    for (unsigned t = 0; t < g_ntables; ++t)
        for (const ShimDef* d = g_tables[t]; d->name; ++d)
            if (!strcmp(d->name, name) && (!d->dll || !stricmp_(d->dll, dll)))
                return d->fn;
    return NULL;
}

uint32_t thunk_for(const char* dll, const char* name)
{
    char full[96];
    snprintf(full, sizeof full, "%s!%s", dll, name);
    for (unsigned i = 0; i < g_count; ++i)
        if (!stricmp_(g_thunks[i].name, full))
            return THUNK_BASE + i * THUNK_STRIDE;
    if (g_count == g_cap)
    {
        g_cap = g_cap ? g_cap * 2 : 512;
        if (g_cap > THUNK_MAX)
            return 0;
        g_thunks = (Thunk*)realloc(g_thunks, g_cap * sizeof *g_thunks);
    }
    Thunk* t = &g_thunks[g_count];
    strcpy(t->name, full);
    t->shim = find_shim(dll, name);
    return THUNK_BASE + g_count++ * THUNK_STRIDE;
}

const char* thunk_name(uint32_t addr)
{
    if (addr < THUNK_BASE || (addr - THUNK_BASE) % THUNK_STRIDE)
        return NULL;
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < g_count ? g_thunks[i].name : NULL;
}

/* FFXI_RECOMP_TRACE=1: every shim call, with its call site, first four arguments and the result
 * (the portable counterpart of R2's boundary trace, runtime/win32/bridge.c). */
static int g_trace = -1;

int thunk_dispatch(Guest* g, uint32_t target)
{
    const char* name = thunk_name(target);
    if (!name)
        return 0;
    Shim s = g_thunks[(target - THUNK_BASE) / THUNK_STRIDE].shim;
    if (!s)
    {
        char buf[160];
        snprintf(buf, sizeof buf, "no shim yet for %s (called from %08x)", name, rd32(g->esp));
        rt_fatal(g, target, buf);
    }
    if (g_trace < 0)
    {
        const char* e = getenv("FFXI_RECOMP_TRACE");
        g_trace = e && *e == '1';
    }
    if (g_trace)
    {
        uint32_t site = rd32(g->esp), a0 = rd32(g->esp + 4), a1 = rd32(g->esp + 8), a2 = rd32(g->esp + 12), a3 = rd32(g->esp + 16);
        s(g);
        rt_log("[trace] %08x %-44s (%08x %08x %08x %08x) = %08x\n", site, name, a0, a1, a2, a3, g->eax);
        return 1;
    }
    s(g);
    return 1;
}

unsigned thunk_report_missing(void)
{
    /* polcore's 1,560 table slots are thunks too; most are never called, so they are only counted */
    unsigned n = 0, slots = 0;
    for (unsigned i = 0; i < g_count; ++i)
        if (!g_thunks[i].shim)
        {
            if (!strncmp(g_thunks[i].name, "polcore.dll!", 12))
                slots++;
            else
            {
                rt_log("[recomp]   no shim: %s\n", g_thunks[i].name);
                n++;
            }
        }
    rt_log("[recomp] %u bound imports have no shim yet (and %u polcore slots)\n", n, slots);
    return n;
}
