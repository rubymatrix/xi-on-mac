/* Direct3D 8 for 64-bit hosts (d3d8.c): the D3D8 front end the Metal renderer goes under. */
#pragma once

/* Registers the shims (before the images are mapped). */
void d3d8_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void d3d8_setup(void);
/* Called at every Present, on the game's thread with the guest lock held, before the frame goes
 * out: where the host adjusts per-frame game state (host64: the frame-rate divisor). */
void d3d8_set_present_hook(void (*fn)(void));
