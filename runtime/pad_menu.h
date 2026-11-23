/* The start-up menu's Controls page (pad_menu.c): controllers.ini's [device default] - the layout, the
 * sticks' dead zone, rumble, and which pad control presses each of the console's buttons - with a
 * picture of the pad in use, the selected button's control lit on it.
 *
 * No window here (tests/test_pad_menu.py drives it): gpu_menu.c draws it and feeds it input, and
 * linux/window.c tells it what the pads do (a control pressed while it listens, the sticks). Saved
 * before the game reads controllers.ini (hle_pad.c, at its first input), so it applies at once. */
#pragma once

/* A pad's controls, numbered as hle_pad.c's raw ones (SDL's gamepad buttons), then the triggers and none */
enum { PC_SOUTH, PC_EAST, PC_WEST, PC_NORTH, PC_BACK, PC_GUIDE, PC_START, PC_LSTICK, PC_RSTICK, PC_LSHOULDER, PC_RSHOULDER,
       PC_DPUP, PC_DPDOWN, PC_DPLEFT, PC_DPRIGHT, PC_BUTTONS = 26, PC_LTRIGGER = PC_BUTTONS, PC_RTRIGGER, PC_NONE, PC_COUNT };
#define PAD_DEFAULT (-1)   /* a console button as the layout has it */

/* The console's buttons, as nn::hid::NpadButton bits 0-15 (hle_pad.c's N_*) */
enum { NB_A, NB_B, NB_X, NB_Y, NB_LSTICK, NB_RSTICK, NB_L, NB_R, NB_ZL, NB_ZR, NB_PLUS, NB_MINUS, NB_LEFT, NB_UP, NB_RIGHT, NB_DOWN, NB_COUNT };

/* The kinds of pad the page draws and names controls for (gpu_menu.h's MENU_PAD_* less one) */
enum { PK_XBOX, PK_PS, PK_NINTENDO };

typedef struct PadSettings {
    int layout;          /* 0 positional (the bottom face button is B, as on a Switch), 1 labels (by the printed letter) */
    int deadzone_left, deadzone_right;   /* percent of each stick's travel ignored around its centre, 0-50 */
    int snapback_left, snapback_right;   /* the snapback fix on each stick (pad_snap.h): 1 on, 0 off (the default) */
    int rumble;          /* percent, 0-100 */
    int map[NB_COUNT];   /* PC_* for each console button, or PAD_DEFAULT */
} PadSettings;

/* The rows: the pad (information), its settings, a row per console button the page offers, Reset; the
 * dead zones' own page (CROW_DEADZONE opens it: each stick's, a test of the sticks, back); the picture
 * (drawn beside the rows), Play */
enum { CROW_PAD, CROW_LAYOUT, CROW_DEADZONE, CROW_RUMBLE, CROW_BUTTON, CROW_BUTTON_END = CROW_BUTTON + 11, CROW_RESET,
       CROW_DZ_LEFT, CROW_DZ_RIGHT, CROW_SNAP_LEFT, CROW_SNAP_RIGHT, CROW_DZ_TEST, CROW_DZ_BACK, CROW_PICTURE, CROW_PLAY, CROWS };
enum { PAD_EV_NONE, PAD_EV_PLAY };

typedef struct PadMenu {
    PadSettings s, initial;
    int row;
    int listening;            /* the selected button row waits for a control pressed (menu: Accept) */
    int dead_zones;           /* on the dead zones' own page (MENU_BACK: back to the rest) */
    int testing;              /* the sticks' test shown in the picture (the dead zones' page) */
    int no_play;              /* no Play row (the start-up menu plays from its Display page) */
    int kind;                 /* PK_*: the pad drawn and its controls' names */
    char pad[96];             /* the pad in use ("" none) */
    int axes[4];              /* the sticks now (lx, ly, rx, ry; -32768..32767, +y down), for the picture */
    int game_axes[4];         /* ...after the snapback fix, where it is on (linux/window.c: as the game will filter them) */
    int snap_flash[2];        /* a bounce was just ignored on the left, right stick (the test says so) */
} PadMenu;

void pad_menu_init(PadMenu *m, const PadSettings *s);
int pad_menu_visible(const PadMenu *m, int row);
int pad_menu_selectable(const PadMenu *m, int row);
const char *pad_menu_label(int row);
int pad_menu_row_button(int row);    /* a button row's console button (NB_*), -1 for another row */
/* The value shown (the same leading control characters as net_menu_value; '\16' the picture: see
 * pad_menu_value's comment in pad_menu.c) */
void pad_menu_value(const PadMenu *m, int row, char *out, unsigned n);
/* MENU_UP/DOWN/LEFT/RIGHT/ACCEPT (gpu_menu.h), or MENU_PADKEY with a control (PC_*) while listening,
 * MENU_BACK while it listens or is on the dead zones' page (it is the page's then: pad_menu_takes_back):
 * PAD_EV_PLAY when Play was chosen */
int pad_menu_takes_back(const PadMenu *m);
int pad_menu_event(PadMenu *m, int event, int c);
int pad_menu_effective(const PadSettings *s, int kind, int button);   /* the control that presses a console button (PC_*) */
const char *pad_control_name(int kind, int control);                  /* "A", "Cross", "LB", "L2", "ZR"... in the pad's own terms */

/* ---- controllers.ini ---- */
/* Its [device default] (SSBU_CONTROLLERS names another file), over the built-in defaults */
void pad_settings_load(PadSettings *s);
/* The settings into [device default], the rest of the file as it was (written with the defaults first
 * when there is none: pad_default_ini, hle_pad.c). 1 written. */
int pad_settings_save(const PadSettings *s);
