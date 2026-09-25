/* Guest paths (R3): the guest always sees Windows paths ("C:\Program Files (x86)\PlayOnline\...",
 * relative paths, "FINAL FANTASY XI//FTABLE.DAT" with a doubled separator, "/" or "\"). vfs
 * keeps the guest's current directory itself - the host's is never consulted - normalises every
 * path to an absolute Windows form, and maps it to a host path through a mount table:
 *   Windows x64: no mounts needed; the normalised path is the host path.
 *   macOS: the player's copied install mounted where the retail installer put it, e.g.
 *          "C:\Program Files (x86)\PlayOnline" -> "/Users/x/Library/Application Support/FFXI/PlayOnline".
 * Guest strings are code page 1252 (k32.c); host paths are UTF-8. */
#pragma once

#include <stddef.h>

void vfs_init(const char* guest_cwd);
/* Adds a mapping: guest paths under guest_prefix live under host_prefix. Longest prefix wins. */
void vfs_mount(const char* guest_prefix, const char* host_prefix);

/* The absolute, normalised guest form of a guest path (GetFullPathNameA). 0 if it does not fit. */
int vfs_full_path(const char* guest, char* out, size_t n);
/* The host path for a guest path. 0 if it maps nowhere. */
int vfs_host_path(const char* guest, char* host, size_t n);

const char* vfs_cwd(void);
/* Changes the guest's current directory (it must exist). */
int vfs_set_cwd(const char* guest);
