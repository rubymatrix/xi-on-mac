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
/* The AFK logout timer's check, run from the per-frame pump (polcore_files.c). */
void polcore_idle_tick(void);

/* The session value V for this sign-in: 16 bytes, no NUL, byte-identical to the member's
 * pol_accounts.session_value (LSB's lobby checks the passwords built from it). */
void polcore_set_session(const uint8_t v[16]);
/* The lobby's IPv4 address (host byte order), which "ffxi00.pol.com" resolves to. Default 127.0.0.1;
 * the hosts set it to their PlayOnline server (host64: --pol-server). */
void polcore_set_lobby(uint32_t ipv4_host_order);
/* The command line polcore reports (GetlpCmdLine): empty, or xiloader's " /game eAZcFcB -net 3
 * -port <lobby view port>" for a LandSandBoat sign-in. Before polcore_init. */
void polcore_set_cmdline(const char* text);
/* The exit code and message the game left (slots 969/1026), after GameStart returns. */
int32_t polcore_exit_code(const char** message);
