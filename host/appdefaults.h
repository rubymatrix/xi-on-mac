/* Defaults from the app bundle host64 runs in (macOS: "Final Fantasy XI.app", built by
 * tools/build_posix.py app): its Info.plist's FFXI* keys, so the app starts from Finder or the Dock
 * with no command line.
 *
 *   FFXIGameFolder       the FINAL FANTASY XI folder (--game)
 *   FFXISignInMethod     "pol" or "lsb": the sign-in screen's first-run method
 *   FFXIServer           the sign-in screen's first-run server
 *   FFXIWindowMode       0 full screen, 1 windowed, 2 borderless, 3 borderless full screen
 *   FFXIResolution       "3440x1440": the game's first-run window
 *   FFXIMenuResolution   "1720x720": its menus
 *   FFXIBackground       a picture in the bundle's Resources, behind the sign-in screen
 *
 * They are defaults: what the player saved (signin.cfg, settings.reg) and the command line win.
 * Elsewhere, and outside a bundle, there are none. */
#pragma once

#include <stddef.h>

/* key's string (or number, as text) from the bundle's Info.plist: 0 if there is none. */
int app_default(const char* key, char* out, size_t n);
/* The path of a file in the bundle's Resources: 0 if there is no bundle or no such file. */
int app_resource(const char* name, char* out, size_t n);
/* Running from an app bundle with FFXI* defaults. */
int app_bundled(void);
