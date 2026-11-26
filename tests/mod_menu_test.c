/* runtime/mod_menu.c and mod_scan.c without a window: tests/test_mod_menu.py builds this with them.
 *   mod_menu_test <mods folder> <script>   one letter each: u d l r a b (up down left right accept back),
 *                                          x (delete: the west button), y (open the folder), z (move: the
 *                                          right trigger), R (the folder read again).
 * Prints, a line each: the selection, the question and the move, the note, every mod
 * (key:E|D:fighter:colors as loaded:conflicts:gameplay), how many times the folder was asked opened,
 * how many times the page asked to go back; then the rows (control characters as \NN, '\t' as " | "). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "mod_menu.h"

const char *rt_data_dir = ".";

static void show(const char *v) {
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p == '\t') fputs(" | ", stdout);
        else if (*p < 32) printf("\\%02o", *p);
        else putchar(*p);
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static ModList l;
    static ModMenu m;
    mod_scan(&l, argv[1]);
    mod_menu_init(&m, &l);
    int folder = 0, backs = 0;
    for (const char *p = argc > 2 ? argv[2] : ""; *p; p++) {
        int ev = -1, c = 0;
        switch (*p) {
        case 'u': ev = MENU_UP; break;
        case 'd': ev = MENU_DOWN; break;
        case 'l': ev = MENU_LEFT; break;
        case 'r': ev = MENU_RIGHT; break;
        case 'a': ev = MENU_ACCEPT; break;
        case 'b': ev = MENU_BACK; break;
        case 'x': ev = MENU_DELETE; break;
        case 'y': ev = MENU_FOLDER; break;
        case 'z': ev = MENU_MOVE; break;
        case 'R': mod_menu_reload(&m); continue;
        default: continue;
        }
        int r = mod_menu_event(&m, ev, c);
        folder += r == MOD_EV_OPEN_FOLDER;
        backs += r == MOD_EV_BACK;
    }
    printf("sel=%s\n", m.on_button ? "(button)" : m.sel >= 0 ? l.mods[m.sel].key : "(none)");
    printf("dialog=%d %d\nmoving=%d %d\n", m.dialog, m.dialog_sel, m.moving, m.delta);
    printf("note=%s\n", m.note);
    for (int i = 0; i < l.n; i++) {
        const ModInfo *x = &l.mods[i];
        char colors[64] = "";
        unsigned o = 0;
        for (int k = 0; k < MOD_COLORS; k++)
            if (x->colors & 1u << k) o += (unsigned)snprintf(colors + o, sizeof colors - o, "%sc%02d", o ? "," : "", mod_color_to(x, k));
        printf("mod=%s:%c:%s:%s:%d:%d:%s\n", x->key, x->enabled ? 'E' : 'D', x->fighter, colors, x->conflicts, x->gameplay, x->name);
    }
    printf("folder=%d\nbacks=%d\n", folder, backs);
    const char *keys[5] = {"A", "X", "RT", "Y", "B"};
    for (int r = 0; r < mod_menu_rows(&m); r++) {
        char label[64], value[2400];
        mod_menu_row(&m, r, keys, label, sizeof label, value, sizeof value);
        printf("row%d=%s: ", r, label);
        show(value);
    }
    if (m.dialog) {
        char q[400];
        mod_menu_dialog_text(&m, q, sizeof q);
        printf("question=");
        show(q);
    }
    mod_list_free(&l);
    return 0;
}
