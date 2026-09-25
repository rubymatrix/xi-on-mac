/* USER32 and WINMM on SDL3 (user32.c). */
#pragma once

#include <stdint.h>

void user32_init(void);
/* The SDL_Window behind a guest HWND (for the graphics layer), or NULL. */
void* user32_sdl_window(uint32_t hwnd);
/* A window's client size (unchanged if hwnd is not ours); the desktop's mode. */
void user32_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz);
/* --ui-aspect: the game's screen-space draws (its interface) keep width / height = aspect, centered
 * in a wider window, and the mouse is mapped to match. 0 (the default) is off. */
void user32_set_ui_aspect(float aspect);
/* The fraction of hwnd's width the interface keeps: 1 when off, or the window is not wider. */
float user32_ui_squeeze(uint32_t hwnd);
