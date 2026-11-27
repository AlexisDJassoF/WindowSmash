/* runtime/mod_menu.c and mod_scan.c without a window: tests/test_mod_menu.py builds this with them.
 *   mod_menu_test <mods folder> <script>   one letter each: u d l r a b (up down left right accept back),
 *                                          x (delete: the west button), y (open the folder), z (move: the
 *                                          right trigger), h (share or not: the left trigger), R (the folder read again);
 *                                          and for the grid, each ended by ';': C<cols> (the view's columns),
 *                                          S<ax>,<ay>,<frames> (the stick, frames of 1/60 s), W<x>,<y>,<deadzone>,<frames>
 *                                          (SDL's stick through the dead zone), H<tile> (the mouse over it),
 *                                          K<tile> (a click on it), k<row>,<tag> (a click on a row), T<fighter> (its tile opened),
 *                                          A<k> (a click on the detail's k-th action), P<c> (a click on a page's color c).
 *   mod_menu_test --roster                 the tiles: "tile<N>=<name>:<internal names>:<portrait>", a line each.
 * Prints, a line each: the level, tile, cursor, each tile's sum (name:on:off:mark:why), the selection, the question and the move, the note, every mod
 * (key:E|D:fighter:colors as loaded:conflicts:gameplay), how many times the folder was asked opened,
 * how many times the page asked to go back; then the rows (control characters as \NN, '\t' as " | "). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "mod_menu.h"
#include "mod_roster.h"

const char *rt_data_dir = ".";

static void show(const char *v) {
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p == '\t') fputs(" | ", stdout);
        else if (*p < 32) printf("\\%02o", *p);
        else putchar(*p);
    }
    putchar('\n');
}

static const char *num(const char *p, double *v) {   /* a number at p, and where it ends */
    char *end;
    *v = strtod(p, &end);
    return end;
}

static uint64_t parity_body(const char *fighter, int color) { return !strcmp(fighter, "inkling") ? 1 + (uint64_t)(color & 1) : 0; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "--roster")) {
        for (int t = 0; t < ROSTER_TILES; t++) {
            const RosterTile *r = mod_roster(t);
            printf("tile%d=%s:", t, mod_roster_name(t));
            for (int k = 0; r && k < ROSTER_NAMES && r->names[k]; k++) printf("%s%s", k ? "," : "", r->names[k]);
            printf(":%s\n", r ? r->portrait : "");
        }
        return 0;
    }
    static ModList l;
    static ModMenu m;
    mod_scan(&l, argv[1]);
    mod_menu_init(&m, &l);
    int folder = 0, backs = 0, no_mods = 0;
    if (getenv("TEST_BODY_PARITY")) mod_body_hook = parity_body;   /* (Inkling's: the even colors one body, the odd ones another) */
    const char *rec_dir = getenv("TEST_RECOVERY_DIR");   /* (the recovery's question: its last session there) */
    if (rec_dir) {
        Recovery r;
        mod_recovery_read(rec_dir, &r);
        r.safe = getenv("TEST_RECOVERY_SAFE") != NULL;
        if (r.failed || r.safe) mod_menu_recovery(&m, &r, rec_dir);
    }
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
        case 'h': ev = MENU_SHARE; break;
        case 'R': mod_menu_reload(&m); continue;
        case 'C': case 'S': case 'W': case 'H': case 'K': case 'k': case 'T': case 'A': case 'P': {
            char cmd = *p, arg[64];
            const char *end = strchr(p + 1, ';');
            if (!end) end = p + strlen(p);
            snprintf(arg, sizeof arg, "%.*s", (int)(end - p - 1), p + 1);
            double v[4] = {0};
            const char *q = arg;
            for (int k = 0; k < 4 && *q; k++) { q = num(q, &v[k]); if (*q == ',') q++; else break; }
            if (cmd == 'C') mod_menu_set_cols(&m, (int)v[0]);
            else if (cmd == 'S') for (int f = 0; f < (int)v[2]; f++) mod_menu_cursor(&m, (float)v[0], (float)v[1], 1.0f / 60);
            else if (cmd == 'W') {
                float ax, ay;
                mod_menu_stick((int)v[0], (int)v[1], (int)v[2], &ax, &ay);
                for (int f = 0; f < (int)v[3]; f++) mod_menu_cursor(&m, ax, ay, 1.0f / 60);
            } else if (cmd == 'H') mod_menu_hover(&m, (int)v[0]);
            else if (cmd == 'K') mod_menu_click_tile(&m, (int)v[0]);
            else if (cmd == 'k') folder += mod_menu_click_row(&m, (int)v[0], (int)v[1]) == MOD_EV_OPEN_FOLDER;
            else if (cmd == 'A') folder += mod_menu_click_action(&m, (int)v[0]) == MOD_EV_OPEN_FOLDER;
            else if (cmd == 'P') mod_menu_click_color(&m, (int)v[0]);
            else mod_menu_click_tile(&m, mod_roster_tile(arg));
            p = *end ? end : end - 1;
            continue;
        }
        default: continue;
        }
        int r = mod_menu_event(&m, ev, c);
        no_mods += r == MOD_EV_NO_MODS;
        folder += r == MOD_EV_OPEN_FOLDER;
        backs += r == MOD_EV_BACK;
    }
    printf("level=%d\ntile=%d:%s\ncursor=%.3f,%.3f,%d\n", m.level, m.tile, mod_roster_name(m.tile), m.cur_x, m.cur_y, m.cursor_shown);
    for (int t = 0; t < ROSTER_TILES; t++)
        if (m.sum[t].on || m.sum[t].off)
            printf("sum=%s:%d:%d:%c:%s\n", mod_roster_name(t), m.sum[t].on, m.sum[t].off, m.sum[t].mark ? m.sum[t].mark : '-', m.sum[t].why);
    const char *sel = m.on_button ? "(button)" : mod_menu_grid(&m) ? "(grid)" : m.sel >= l.n ? "(host)" : m.sel >= 0 ? l.mods[m.sel].key : "(none)";
    printf("sel=%s\n", sel);
    printf("dialog=%d %d\nmoving=%d %d\n", m.dialog, m.dialog_sel, m.moving, m.delta);
    printf("note=%s\n", m.note);
    for (int i = 0; i < l.n; i++) {
        const ModInfo *x = &l.mods[i];
        char colors[64] = "";
        unsigned o = 0;
        for (int k = 0; k < MOD_COLORS; k++)
            if (x->colors & 1u << k) o += (unsigned)snprintf(colors + o, sizeof colors - o, "%sc%02d", o ? "," : "", mod_color_to(x, k));
        printf("mod=%s:%c:%s:%s:%d:%d:%s%s\n", x->key, x->enabled ? 'E' : 'D', x->fighter, colors, x->conflicts, x->gameplay, x->name,
               x->no_share ? ":noshare" : "");
    }
    printf("folder=%d\nbacks=%d\nno_mods=%d\n", folder, backs, no_mods);
    char warn[200];
    mod_gameplay_warning(&l, warn, sizeof warn);
    printf("gameplay=%s\n", warn);
    const char *keys[6] = {"A", "X", "RT", "Y", "B", "LT"};
    for (int r = 0; r < mod_menu_rows(&m); r++) {
        char label[64], value[4096];
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
