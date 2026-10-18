/* Display options (display.ini): the window's mode and size, the picture's place in it, and the
 * pacing of frames (gpu_display.c). */
#pragma once
#include <windows.h>

typedef enum { PACING_CLOCK, PACING_VRR, PACING_DISPLAY } Pacing;

void display_dpi_aware(void);           /* first thing in main: window sizes in real pixels */
/* The game's window, made by the renderer's window thread with its class and procedure. */
HWND display_create_window(HINSTANCE inst, const wchar_t *cls, const wchar_t *title);
/* The window procedure offers every message here first: nonzero = handled, the answer in *result. */
int display_message(HWND h, UINT msg, WPARAM w, LPARAM l, LRESULT *result);
/* The renderer, at a present: the window's client size, when it changed since the last call. */
int display_take_size(unsigned *w, unsigned *h);
/* The 16:9 picture centred in a w x h back buffer. */
void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh);
int display_latency(void);              /* frames the GPU may queue ahead: 1-3 */
Pacing display_pacing(void);
/* The renderer says whether the swap chain can tear (for VRR); the flags each Present gets. */
void display_set_tearing(int supported);
unsigned display_present_flags(void);
/* The vsync thread: returns when the next refresh the game sees is due. */
void display_wait_tick(void);
