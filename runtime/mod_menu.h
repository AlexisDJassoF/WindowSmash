/* The start-up menu's Mods page (mod_menu.c). First a grid of the fighters (mod_roster.h), in the
 * order of Smash's character select, then Other: each tile in colour with a green frame and how many
 * of its mods are enabled, dimmed with only disabled ones, grey with none, marked for a problem of
 * colors (two enabled mods in one, c08+) or a warning (more than looks, music, config.json, files
 * another mod replaces too). The left stick moves a cursor freely over it (mod_menu_cursor), the
 * D-pad and the arrows go tile by tile, the mouse selects the tile under it; A (Enter) or a click
 * opens the tile's page: its eight colors, its mods as cards and the selected one's detail beside
 * them, and B (Esc) goes back to the grid. There, A enables or disables the mod, X (Delete) asks
 * whether to delete it - Cancel chosen when the question opens -, the right trigger (M) moves it to
 * another color of its fighter (left/right, A places it there, B cancels), the left trigger (S)
 * shares it in the rooms hosted here or not (netplay-mod-sync); and Y (F) opens the folder on both.
 * Changes apply at the next Play. SSBU_MENU_MODS=list: the list of every mod instead of the grid.
 *
 * The model has no window (tests/test_mod_menu.py drives it); gpu_menu.c feeds it the menu's events
 * and the stick, and draws the rows it describes. */
#pragma once
#include "mod_cache.h"
#include "mod_recovery.h"
#include "mod_roster.h"
#include "mod_scan.h"

#define MENU_HOSTS 14   /* the hosts whose room mods are kept (mods/.netplay), listed under the mods: X deletes them */

enum { MOD_EV_NONE, MOD_EV_BACK, MOD_EV_OPEN_FOLDER,
       MOD_EV_NO_MODS };   /* what mod_menu_event says to do (the recovery's: start without mods, this once) */
enum { REC_OPT_DISABLE, REC_OPT_LAST_GOOD, REC_OPT_NO_MODS, REC_OPT_IGNORE };   /* the recovery's choices */

/* A tile's mods, counted when the folder is read: the marks from the enabled ones alone */
typedef struct RosterSum {
    int on, off;              /* its mods (and rooms' mods, in Other) enabled, disabled */
    char mark;                /* 'x' a problem of colors, '!' a warning, 0 none */
    char why[120];            /* the first one's reason */
} RosterSum;

/* The cursor: tiles a second with the stick all the way (its speed the tilt squared), slowed to
 * CURSOR_BRAKE times that within CURSOR_BRAKE_R of a tile's centre */
#define CURSOR_VEL 7.0f
#define CURSOR_BRAKE 0.55f
#define CURSOR_BRAKE_R 0.25f
#define CURSOR_MIN_DEADZONE 10   /* (percent: a stick at rest drifts, controllers.ini's 0 or not) */

typedef struct ModMenu {
    ModList *l;
    int list;                 /* SSBU_MENU_MODS=list: every mod in one page, no grid */
    int level;                /* 0 the grid, 1 a tile's page */
    int tile;                 /* the tile selected (the grid) or open (its page) */
    float cur_x, cur_y;       /* the cursor, in tiles (0, 0 the first one's corner) */
    int cols;                 /* the grid's columns, as the view drew it last */
    int cursor_shown;         /* 0 while the mouse is used */
    unsigned char tile_of[MOD_MAX];
    RosterSum sum[ROSTER_TILES];
    int page[MOD_MAX + MENU_HOSTS], npage, pmods;   /* the page's items (the mods first, then hosts: l->n + k), how many mods */
    int sel;                  /* the mod selected (-1: none); l->n and up: a host's room mods (hosts[sel - l->n]) */
    HostSummary hosts[MENU_HOSTS];
    int nhosts;
    uint64_t in_use;          /* the host whose room mods this start uses (SSBU_ROOM_MODS), 0 none */
    int in_use_mode;
    int on_button;            /* Open Mods Folder selected instead */
    int dialog, dialog_sel;   /* 1 the delete question open (0 Cancel, 1 Delete); 2 the recovery's (an option of rec_opts) */
    Recovery rec;             /* the recovery's: what happened last time (mod_recovery.h) */
    char rec_dir[1024];       /* (cache/mods: last_good.txt) */
    int rec_opts[4], nrec_opts;
    int moving, delta;        /* moving the selected mod: by how many colors so far */
    int preview;              /* which of the selected mod's colors its preview shows (left/right) */
    char note[200];           /* what the last action did, for the footer ('!' first: it failed) */
} ModMenu;

void mod_menu_init(ModMenu *m, ModList *l);
void mod_menu_reload(ModMenu *m);   /* the folder read again, the same mod selected */
/* MENU_UP/DOWN/LEFT/RIGHT/ACCEPT/BACK/DELETE/FOLDER/MOVE/SHARE, or MENU_CHAR c (f, m, s): a MOD_EV_* */
int mod_menu_event(ModMenu *m, int event, int c);

/* The grid (level 0, not SSBU_MENU_MODS=list): the stick, as controllers.ini's dead zone (percent,
 * CURSOR_MIN_DEADZONE at least) leaves SDL's x, y (y down), into ax, ay in [-1, 1]; the cursor moved
 * by it for dt seconds, the tile under it selected; the mouse over a tile (it selects it, the cursor
 * hidden until the stick or the D-pad); a click on one (its page opened: a MOD_EV_*); the columns
 * the view drew */
int mod_menu_grid(const ModMenu *m);
void mod_menu_stick(int x, int y, int deadzone, float *ax, float *ay);
void mod_menu_cursor(ModMenu *m, float ax, float ay, float dt);
void mod_menu_hover(ModMenu *m, int tile);
int mod_menu_click_tile(ModMenu *m, int tile);
void mod_menu_set_cols(ModMenu *m, int cols);

/* The rows gfx_menu draws. The grid: '\23' (every tile, the cursor, the label), then the button. A
 * tile's page: its colors ('\24', not in Other's), a card per mod (or a note when there are none), the
 * detail (a picture beside them), then the button. keys: what the hints name - accept, delete, move, open the folder,
 * back, share (a pad's "^XS"... or a key) */
int mod_menu_rows(const ModMenu *m);
int mod_menu_button_row(const ModMenu *m);
int mod_menu_selected_row(const ModMenu *m);
void mod_menu_row(const ModMenu *m, int row, const char *const keys[6], char *label, unsigned ln, char *value, unsigned vn);
/* The recovery (mod_recovery.h): after a failed session (or the safe mode) its question opens over the
 * page, on the suspect's tile - disable the suspect (chosen at first, when there is one), back to the
 * last mods that worked (those enabled since are disabled), start without mods this once
 * (MOD_EV_NO_MODS; chosen at first in the safe mode), ignore (B, Esc). dir: cache/mods. */
void mod_menu_recovery(ModMenu *m, const Recovery *r, const char *dir);
/* The question open, for overlay_menu_dialog: its text ('\t' between the title and the question, then
 * '\t' and its buttons' labels with '|' between them: none, Cancel and Delete) */
void mod_menu_dialog_text(const ModMenu *m, char *out, unsigned n);
/* A click on row (its card selected; on its Enabled/Disabled tag, toggled) or on the question's
 * buttons (which: 0 Cancel, 1 Delete): a MOD_EV_* */
int mod_menu_click_row(ModMenu *m, int row, int on_tag);
int mod_menu_click_dialog(ModMenu *m, int which);
/* A click on the detail's k-th action (in the order its hints name them) or on a page's color c (the
 * selected mod moved there: A or "place it here" leaves it) */
int mod_menu_click_action(ModMenu *m, int k);
int mod_menu_click_color(ModMenu *m, int c);
/* text as the overlay's font draws it (printable ASCII: accented letters without their accents) */
void mod_ascii(const char *text, char *out, unsigned n);
