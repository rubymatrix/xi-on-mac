/* What the host tells our own polcore (polcore_slots.c). */
#pragma once

#include <stdint.h>

/* Register the implemented slots; call before polcore_init. */
void polcore_slots_init(void); /* session and lobby (polcore_slots.c) */
void polcore_files_init(void); /* paths, files, registry, timers (polcore_files.c) */
void polcore_polpro_init(void); /* polpro client, records, mail, text (polcore_polpro.c) */

/* The PlayOnline Viewer folder as the guest sees it ("C:\...\PlayOnlineViewer"): the root of
 * every POL path (slot 126). */
void polcore_set_root(const char* guest_viewer_dir);
/* A patch.ver (0x120 bytes, encrypted as retail's with no registry key) carrying the client
 * version string, e.g. "30260903_0", for an install that ships none (private-server installs
 * launched without the PlayOnline Viewer). 0 if the string does not fit. */
int polcore_make_patch_ver(const char* version, uint8_t out[0x120]);
/* The AFK logout timer's check, run from the per-frame pump (polcore_files.c). */
void polcore_idle_tick(void);

/* The session value V for this sign-in: 16 bytes, no NUL, byte-identical to the member's
 * pol_accounts.session_value (LSB's lobby checks the passwords built from it). */
void polcore_set_session(const uint8_t v[16]);
/* The lobby's IPv4 address (host byte order), which "ffxi00.pol.com" resolves to. Default 127.0.0.1. */
void polcore_set_lobby(uint32_t ipv4_host_order);
/* The exit code and message the game left (slots 969/1026), after GameStart returns. */
int32_t polcore_exit_code(const char** message);
