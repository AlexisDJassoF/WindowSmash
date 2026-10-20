/* The start-up settings menu (gpu_menu.c): before the shaders' screen and the game, the window
 * shows display.ini's main settings to change with the keyboard, the mouse or a controller. */
#pragma once
#include "display.h"

/* On the start-up thread, first: returns when Play, Esc or the 10 s without input started the game.
 * display.ini's menu = 0 or SSBU_MENU=0: returns at once. Linux only (Windows has no start-up phase). */
void menu_run(void);

/* The window's thread: while menu_active, its input, here instead of to the game. */
enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_ACCEPT, MENU_BACK, MENU_CLICK };
extern volatile int menu_active;
void menu_input(int event, int x, int y);   /* x, y: a click's pixel in the window's drawable */

/* ---- the model, without a window (tests/test_gpu_menu.py drives it) ---------------------- */
enum { ROW_MODE, ROW_UPSCALE, ROW_SHARPNESS, ROW_VSYNC, ROW_WINDOW, ROW_PLAY, ROWS };
enum { MENU_OPEN, MENU_PLAY, MENU_KEEP };   /* MENU_KEEP: started as display.ini was (Esc, or no input in time) */
typedef struct Menu {
    DisplaySettings s, initial;
    unsigned locked;
    int row, state, touched;
    double deadline;   /* seconds (now_s's clock) until it starts by itself, while untouched */
} Menu;
typedef struct MenuRect { float x, y, w, h; } MenuRect;   /* a row's place on screen, from the last frame */

void menu_init(Menu *m, const DisplaySettings *now, unsigned locked, double now_s);
int menu_visible(const Menu *m, int row);                     /* Sharpness only with FSR */
const char *menu_label(int row);
void menu_value(const Menu *m, int row, char *out, unsigned n);   /* "Handheld (720p)", "0.2", "(set by SSBU_MODE)" ... */
void menu_event(Menu *m, int event, double now_s);
/* A click at (x, y) against the rows drawn (rects[row], value_x: where values start): the events it is */
void menu_click(Menu *m, const MenuRect rects[ROWS], float value_x, float x, float y, double now_s);
void menu_tick(Menu *m, double now_s);                        /* the countdown */
