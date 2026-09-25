/* Our own polcore: the IPOLCoreCom object and the common function table. See polcore.h. */
#include <stdio.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "polcore.h"

#define E_NOINTERFACE 0x80004002u
#define S_OK 0u
#define EXE_HANDLE 0x00400000u /* hInstance of the host process, as the guest sees it (k32.c) */

/* IID_IPOLCoreCom, US: {E0516654-EF77-435D-AA7D-50D2C069CE34}. FFXi.dll also tries JP
 * {9A30D565-...} before it and EU {DFEC2E93-...} after it; neither is answered. */
static const uint8_t IID_IPOLCoreCom_US[16] = { 0x54, 0x66, 0x51, 0xE0, 0x77, 0xEF, 0x5D, 0x43,
                                                0xAA, 0x7D, 0x50, 0xD2, 0xC0, 0x69, 0xCE, 0x34 };
static const uint8_t IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

static uint32_t g_object, g_table, g_cmdline;
static uint32_t g_refs = 1;

/* --- IPOLCoreCom ------------------------------------------------------------------------------
 * The methods FFXi.dll calls (FFXI.polcore-surface.txt §1): QueryInterface, Release,
 * GethInstance, GetlpCmdLine, GetCommonFunctionTable, PolViewerExec. All stdcall with `this`. */
static void m_QueryInterface(Guest* g)
{
    const uint8_t* iid = (const uint8_t*)ARGP(1);
    uint32_t out = ARG(2);
    if (!memcmp(iid, IID_IPOLCoreCom_US, 16) || !memcmp(iid, IID_IUnknown, 16))
    {
        g_refs++;
        wr32(out, g_object);
        RET(S_OK, 3);
    }
    wr32(out, 0);
    RET(E_NOINTERFACE, 3);
}

static void m_AddRef(Guest* g) { RET(++g_refs, 1); }
static void m_Release(Guest* g) { RET(g_refs > 1 ? --g_refs : 1, 1); } /* the object lives as long as the process */
static void m_GethInstance(Guest* g) { wr32(ARG(1), EXE_HANDLE); RET(S_OK, 2); }
static void m_GetlpCmdLine(Guest* g) { wr32(ARG(1), g_cmdline); RET(S_OK, 2); }
static void m_GetCommonFunctionTable(Guest* g) { wr32(ARG(1), g_table); RET(S_OK, 2); }
static void m_PolViewerExec(Guest* g) { RET(S_OK, 2); }

/* vtable slots 0..35 (client-polcore-interface.md §2-3; 32-35 are ATL's IDispatch) */
static const char* const METHODS[36] = {
    "QueryInterface", "AddRef", "Release", "GethInstance", "GetlpCmdLine", "SetParamInit", "GetWindowsType",
    "GetCommonFunctionTable", "PolViewerExec", "GetWindowsVersion", "PressAnyKey",
    "PolconSetEnableWakeupFuncFlag", "CreateInput", "UpdateInputState", "GetPadRepeat", "GetPadOn",
    "FinalCleanup", "SetParamInitW", "GetlpCmdLineW", "PaintFriendList", "CreateFriendList",
    "DestroyFriendList", "SetMaskWindowHandle", "GetPlayOnlineRegKeyNameW", "GetPlayOnlineRegKeyNameA",
    "GetSquareEnixRegKeyNameW", "GetSquareEnixRegKeyNameA", "SetAreaCode", "GetAreaCode", "HideMaskWindow",
    "ShowMaskWindow", "IsVisibleMaskWindow", "GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke",
};

static const ShimDef OBJECT[] = {
    { "polcore.dll", "IPOLCoreCom::QueryInterface", m_QueryInterface },
    { "polcore.dll", "IPOLCoreCom::AddRef", m_AddRef },
    { "polcore.dll", "IPOLCoreCom::Release", m_Release },
    { "polcore.dll", "IPOLCoreCom::GethInstance", m_GethInstance },
    { "polcore.dll", "IPOLCoreCom::GetlpCmdLine", m_GetlpCmdLine },
    { "polcore.dll", "IPOLCoreCom::GetCommonFunctionTable", m_GetCommonFunctionTable },
    { "polcore.dll", "IPOLCoreCom::PolViewerExec", m_PolViewerExec },
    { NULL, NULL, NULL },
};

/* --- the common function table ----------------------------------------------------------------- */
static ShimDef g_slot_defs[POLCORE_SLOTS + 1];
static char g_slot_names[POLCORE_SLOTS][12];
static unsigned g_nslot_defs;
static int g_registered;

void polcore_register(const PolcoreSlot* slots)
{
    for (const PolcoreSlot* s = slots; s->fn; ++s)
    {
        if (s->disp / 4 >= POLCORE_SLOTS || g_nslot_defs >= POLCORE_SLOTS)
            continue;
        char* name = g_slot_names[g_nslot_defs];
        snprintf(name, sizeof g_slot_names[0], "+0x%x", s->disp);
        g_slot_defs[g_nslot_defs].dll = "polcore.dll";
        g_slot_defs[g_nslot_defs].name = name;
        g_slot_defs[g_nslot_defs].fn = s->fn;
        g_nslot_defs++;
    }
}

void polcore_init(void)
{
    if (!g_registered)
    {
        g_registered = 1;
        thunk_register(OBJECT);
        thunk_register(g_slot_defs); /* filled by polcore_register calls made before this */
    }
    int took = !gt_holds();
    if (took)
        gt_lock();
    g_cmdline = gheap_strdup("");
    /* the object: a vtable pointer and nothing else the guest reads */
    uint32_t vtbl = gheap_alloc(4 * 36, 1);
    for (unsigned i = 0; i < 36; ++i)
    {
        char name[64];
        snprintf(name, sizeof name, "IPOLCoreCom::%s", METHODS[i]);
        wr32(vtbl + 4 * i, thunk_for("polcore.dll", name));
    }
    g_object = gheap_alloc(16, 1);
    wr32(g_object, vtbl);
    /* the table: every slot a thunk, implemented or trapping */
    g_table = gheap_alloc(4 * POLCORE_SLOTS, 1);
    for (unsigned i = 0; i < POLCORE_SLOTS; ++i)
    {
        char name[16];
        snprintf(name, sizeof name, "+0x%x", 4 * i);
        wr32(g_table + 4 * i, thunk_for("polcore.dll", name));
    }
    if (took)
        gt_unlock();
    rt_log("[recomp] polcore: object %08x, function table %08x, %u slots implemented\n", g_object, g_table, g_nslot_defs);
}

uint32_t polcore_object(void) { return g_object; }
uint32_t polcore_table(void) { return g_table; }
