/* The start-up settings menu (gpu_menu.c): before the shaders' screen and the game, the window
 * shows display.ini's main settings to change with the keyboard, the mouse or a controller. */
#pragma once
#include "display.h"

/* On the start-up thread, first: returns when Play, Esc or the 10 s without input started the game.
 * display.ini's menu = 0 or SSBU_MENU=0: returns at once. Linux only (Windows has no start-up phase). */
void menu_run(void);

/* The window's thread: while menu_active, its input, here instead of to the game. */
enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_ACCEPT, MENU_BACK, MENU_CLICK,
       MENU_TAB,     /* the next page: Display, Network, Controls (Tab, a controller's right shoulder button) */
       MENU_CHAR,    /* a typed character (x), for the Network page's text rows */
       MENU_ERASE,   /* Backspace */
       MENU_TAB_PREV,   /* the page before (Shift+Tab, the left shoulder button) */
       MENU_PADKEY };   /* a pad's control pressed (x: pad_menu.h's PC_*) while the Controls page listens (menu_listening) */
extern volatile int menu_active;
extern volatile int menu_typing;            /* the selected row takes text: Space types, not plays */
void menu_input(int event, int x, int y);   /* x, y: a click's pixel in the window's drawable */

/* In the game (F8 twice): the program started again on the Network page, its menu shown. Linux;
 * returns only if it cannot. */
void menu_restart_network(void);

/* What the menus were used with last (the window's thread, and hle_pad.c in the game): the keyboard
 * or the mouse, or a pad of a kind - their hints name its buttons (the south one plays, the east one
 * goes back, the shoulders change pages). */
enum { MENU_KEYS, MENU_PAD_XBOX, MENU_PAD_PS, MENU_PAD_NINTENDO };
extern volatile int menu_device;

/* The Controls page's view of the pads (linux/window.c fills it while the menu is open): the pad used
 * last, its sticks (lx, ly, rx, ry), and whether the page waits for a control pressed - the pads then
 * press nothing in the menu, their next control going to it as MENU_PADKEY */
extern char menu_pad_name[96];
extern volatile int menu_axes[4];
extern volatile int menu_listening;
extern volatile int menu_sticks_still;
/* The stick test's snapback fix (pad_snap.h), as the game will have it: on for the left, right stick (the
 * page sets it), the sticks after it (linux/window.c), and how many bounces it has ignored on each */
extern volatile int menu_snapback[2], menu_axes_game[4], menu_snap_count[2];   /* the sticks move nothing in the menu, the D-pad alone does (the dead zones' page: the sticks are tested there) */
extern volatile int menu_pad_kind_seen;   /* the kind of the pad seen last (MENU_PAD_*), keyboard in use or not */

/* In the game, F9: the Network page in a window over the picture, to change the room - or host one,
 * or go offline - without leaving the game (net_switch; started again only for LAN mode). While it
 * is open the keyboard and the pads are its (menu_input), not the game's. Linux. */
extern volatile int menu_modal;
void menu_modal_toggle(void);                    /* F9 (the window's thread) */
int menu_modal_build(unsigned w, unsigned h);    /* the renderer, every frame: 1 when its quads are built (overlay_quads), w x h at 0, 0 */

/* ---- the model, without a window (tests/test_gpu_menu.py drives it) ---------------------- */
enum { ROW_MODE, ROW_SCALE, ROW_UPSCALE, ROW_SHARPNESS, ROW_VSYNC, ROW_WINDOW, ROW_RENDERER, ROW_PLAY, ROWS };
enum { MENU_OPEN, MENU_PLAY, MENU_KEEP };   /* MENU_KEEP: started as display.ini was (Esc, or no input in time) */
typedef struct Menu {
    DisplaySettings s, initial;
    unsigned locked;
    int row, state, touched;
    double deadline;   /* seconds (now_s's clock) until it starts by itself, while untouched */
} Menu;
typedef struct MenuRect { float x, y, w, h; } MenuRect;   /* a row's place on screen, from the last frame */
/* The Network page's list, as overlay_build_menu draws it: a room's Join button, from its row's right
 * end; the heading's Refresh button, the same */
#define MENU_JOIN_BUTTON 170
#define MENU_ICON_BUTTON 64

void menu_init(Menu *m, const DisplaySettings *now, unsigned locked, double now_s);
int menu_visible(const Menu *m, int row);                     /* Sharpness only with FSR */
const char *menu_label(int row);
void menu_value(const Menu *m, int row, char *out, unsigned n);   /* "Handheld (720p)", "75% (960 x 540)", "(set by SSBU_MODE)" ... */
int menu_recommends_fsr(const Menu *m);                      /* below 100% with linear upscaling */
void menu_event(Menu *m, int event, double now_s);
/* A click at (x, y) against the rows drawn (rects[row], value_x: where values start): the events it is */
void menu_click(Menu *m, const MenuRect rects[ROWS], float value_x, float x, float y, double now_s);
void menu_tick(Menu *m, double now_s);                        /* the countdown */
