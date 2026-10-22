/* Display options (display.ini): the window's mode and size, the picture's place in it, and the
 * pacing of frames (gpu_display.c). */
#pragma once
#include <windows.h>

typedef enum { PACING_CLOCK, PACING_VRR, PACING_DISPLAY, PACING_VSYNC } Pacing;   /* (vsync: the clock, presented at the next refresh) */
typedef enum { OP_DOCKED, OP_HANDHELD } OperationMode;   /* display.ini's mode: the console the game sees */
typedef enum { UPSCALE_LINEAR, UPSCALE_FSR } Upscale;      /* display.ini's upscale */

void display_dpi_aware(void);           /* first thing in main: window sizes in real pixels */
#ifdef _WIN32
/* The game's window, made by the renderer's window thread with its class and procedure. */
HWND display_create_window(HINSTANCE inst, const wchar_t *cls, const wchar_t *title);
/* The window procedure offers every message here first: nonzero = handled, the answer in *result. */
int display_message(HWND h, UINT msg, WPARAM w, LPARAM l, LRESULT *result);
#else
/* Linux: linux/window.c makes the window with SDL from display.ini's settings and reports to it. */
enum { DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN };
int display_window_mode(void);
void display_window_size(int *width, int *height);
int display_hide_cursor(void);
void display_note_size(unsigned w, unsigned h);   /* the window's inside changed (pixels) */
void display_note_refresh(int hz);                /* the window's monitor refreshes hz times a second */
#endif
/* The renderer, at a present: the window's client size, when it changed since the last call. */
int display_take_size(unsigned *w, unsigned *h);
/* The 16:9 picture centred in a w x h back buffer. */
void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh);
int display_latency(void);              /* frames the GPU may queue ahead: 1-3 */
OperationMode display_operation_mode(void);               /* docked (as it ships) or handheld */
void display_screen_size(unsigned *w, unsigned *h);       /* the screen the game is told of: 1920x1080 or 1280x720 */
Upscale display_upscale(void);
float display_sharpness(void);                            /* fsr's sharpening in stops: 0.0 (most) to 2.0 */
float display_scale(void);                                /* the render resolution scale: 0.5 to 2 (display.ini's scale %) */
unsigned display_sync_interval(void);                     /* Present's: 1 with pacing = vsync, else 0 */
int display_menu_enabled(void);                           /* display.ini's menu (SSBU_MENU overrides) */
int display_renderer(void);                               /* Linux: display.ini's renderer, RENDERER_* (SSBU_RENDERER overrides) */
enum { RENDERER_DXVK, RENDERER_VULKAN };

/* The start-up menu's settings (gpu_menu.c): window is the window mode (0 windowed, 1 borderless,
 * 2 fullscreen). LOCK_*: set by an environment variable, not the menu's to change. */
typedef struct DisplaySettings { int window; OperationMode operation; Upscale upscale; float sharpness; Pacing pacing; int scale; int renderer; } DisplaySettings;
enum { LOCK_WINDOW = 1, LOCK_MODE = 2, LOCK_UPSCALE = 4, LOCK_PACING = 8, LOCK_SCALE = 16, LOCK_RENDERER = 32 };
void display_get_settings(DisplaySettings *now, DisplaySettings *file, unsigned *fixed);
void display_apply(const DisplaySettings *s);             /* before the game starts */
int display_save(const DisplaySettings *s);               /* what changed from the file, into display.ini */
void display_toggle_operation_mode(void);                  /* F7: docked <-> handheld while playing, told to the game */
int display_notice(char *out, unsigned n);                /* for 2 s after F7: the mode, to show over the picture */
/* The mode bar at the top of a window (its button switches as F7 does): its height in the drawable's
 * pixels (0: none, in full screen), and its button's place, drawn by the renderer and clicked in the window */
void display_note_bar(unsigned px);
unsigned display_bar_height(void);
void display_note_bar_button(int x, int y, int w, int h);
int display_bar_hit(int x, int y);
int display_take_window_request(void);                    /* linux/window.c: a window mode display_apply asked for, or -1 */
Pacing display_pacing(void);
/* The renderer says whether the swap chain can tear (for VRR); the flags each Present gets. */
void display_set_tearing(int supported);
unsigned display_present_flags(void);
/* The vsync thread: returns when the next refresh the game sees is due. */
void display_wait_tick(void);
