/* Our own polcore (R3, decided 2026-09-24): what
 * PlayOnline's polcore.dll gives FFXi.dll and FFXiMain.dll, implemented natively.
 *
 * Two surfaces (specs/FFXI.polcore-surface.txt):
 *   - the IPOLCoreCom object FFXi.dll's GameStart receives. FFXi.dll calls six methods; the
 *     object answers only the US interface id, so FFXi.dll records region 1, as a retail
 *     US client does;
 *   - the 1,560-slot common function table, which FFXi.dll hands FFXiMain through the parameter
 *     block. It lives in guest memory; each slot is a thunk (thunk.h) named "polcore.dll!+0xDISP",
 *     so a slot with no implementation traps with its offset on first call.
 *
 * The slot implementations (polcore_slots.c) follow the per-slot specifications in
 * specs/polcore-slots.*.txt. */
#pragma once

#include <stdint.h>

#include "thunk.h"

#define POLCORE_SLOTS 1560u

/* Builds the table and the object in guest memory. Call after the guest window exists. */
void polcore_init(void);
/* The IPOLCoreCom object (a guest address), to pass to IFFXiEntry::GameStart. */
uint32_t polcore_object(void);
/* The common function table (a guest address). */
uint32_t polcore_table(void);

/* Slot implementations register here (by byte offset into the table, as the docs number them). */
typedef struct PolcoreSlot
{
    uint32_t disp;
    Shim fn;
} PolcoreSlot;
void polcore_register(const PolcoreSlot* slots); /* NULL-terminated (fn == NULL) */

/* The presence values slot 195 sets (-2 = keep), read back by slots 189/190/204/206/207. */
void polpro_presence_update(int32_t chan, int32_t sub, int32_t flag, int32_t mode, int32_t word);
