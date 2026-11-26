/* The start-up menu's Mods page (mod_menu.c): the mods in mods/ (mod_scan.h) as a list of cards,
 * the selected one's detail beside it, and Open Mods Folder under them. A (Enter) enables or
 * disables the mod, X (Delete) asks whether to delete it - Cancel chosen when the question opens -,
 * the right trigger (M) moves it to another color of its fighter (left/right, A places it there, B
 * cancels), Y (F) opens the folder. Changes apply at the next Play.
 *
 * The model has no window (tests/test_mod_menu.py drives it); gpu_menu.c feeds it the menu's events
 * and draws the rows it describes. */
#pragma once
#include "mod_scan.h"

enum { MOD_EV_NONE, MOD_EV_BACK, MOD_EV_OPEN_FOLDER };   /* what mod_menu_event says to do */

typedef struct ModMenu {
    ModList *l;
    int sel;                  /* the mod selected (-1: none) */
    int on_button;            /* Open Mods Folder selected instead */
    int dialog, dialog_sel;   /* the delete question open; 0 Cancel, 1 Delete */
    int moving, delta;        /* moving the selected mod: by how many colors so far */
    char note[200];           /* what the last action did, for the footer ('!' first: it failed) */
} ModMenu;

void mod_menu_init(ModMenu *m, ModList *l);
void mod_menu_reload(ModMenu *m);   /* the folder read again, the same mod selected */
/* MENU_UP/DOWN/LEFT/RIGHT/ACCEPT/BACK/DELETE/FOLDER/MOVE, or MENU_CHAR c (f, m): a MOD_EV_* */
int mod_menu_event(ModMenu *m, int event, int c);

/* The rows gfx_menu draws: a card per mod (or a note when there are none), the detail (a picture
 * beside them), then the button. keys: what the hints name - accept, delete, move, open the folder,
 * back (a pad's "^XS"... or a key) */
int mod_menu_rows(const ModMenu *m);
int mod_menu_button_row(const ModMenu *m);
int mod_menu_selected_row(const ModMenu *m);
void mod_menu_row(const ModMenu *m, int row, const char *const keys[5], char *label, unsigned ln, char *value, unsigned vn);
/* The delete question, for overlay_menu_dialog: its text ('\t' between the title and the question) */
void mod_menu_dialog_text(const ModMenu *m, char *out, unsigned n);
/* A click on row (its card selected; on its Enabled/Disabled tag, toggled) or on the question's
 * buttons (which: 0 Cancel, 1 Delete): a MOD_EV_* */
int mod_menu_click_row(ModMenu *m, int row, int on_tag);
int mod_menu_click_dialog(ModMenu *m, int which);
/* text as the overlay's font draws it (printable ASCII: accented letters without their accents) */
void mod_ascii(const char *text, char *out, unsigned n);
