/* The start-up menu's Mods page: see mod_menu.h. */
#include "mod_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"

void mod_ascii(const char *s, char *out, unsigned n) {
    /* UTF-8's Latin-1 letters (U+00C0..U+00FF), their accents dropped */
    static const char L1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    unsigned o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 1 < n; p++) {
        if (*p >= 32 && *p < 127) out[o++] = (char)*p;
        else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) out[o++] = L1[p[1] - 0x80], p++;
        else if (*p == 0xC2 && p[1]) { if (p[1] == 0xA0) out[o++] = ' '; p++; }
        else if (*p == '\t' || *p == '\n' || *p == '\r') out[o++] = ' ';
        else if (*p >= 0xC0) {   /* another character: one '?' for all its bytes */
            out[o++] = '?';
            while ((p[1] & 0xC0) == 0x80) p++;
        }
    }
    out[o] = 0;
}

void mod_menu_init(ModMenu *m, ModList *l) {
    memset(m, 0, sizeof *m);
    m->l = l;
    m->sel = l->n ? 0 : -1;
    m->on_button = !l->n;
}

static void select_key(ModMenu *m, const char *key, int fallback) {
    int i = key && *key ? mod_find(m->l, key) : -1;
    if (i < 0) i = fallback < m->l->n ? fallback : m->l->n - 1;
    m->sel = i;
    if (m->sel < 0) m->on_button = 1;
}

void mod_menu_reload(ModMenu *m) {
    char key[128] = "", root[512];
    if (m->sel >= 0 && m->sel < m->l->n) snprintf(key, sizeof key, "%s", m->l->mods[m->sel].key);
    snprintf(root, sizeof root, "%s", m->l->root);
    int at = m->sel;
    mod_scan(m->l, root);
    select_key(m, key, at < 0 ? 0 : at);
}

static ModInfo *selected(ModMenu *m) { return !m->on_button && m->sel >= 0 && m->sel < m->l->n ? &m->l->mods[m->sel] : NULL; }

static void say(ModMenu *m, int bad, const char *text) {
    char clean[190];
    mod_ascii(text, clean, sizeof clean);
    snprintf(m->note, sizeof m->note, "%s%s", bad ? "!" : "", clean);
}

/* Why a mod cannot be moved, or NULL */
static const char *unmovable(const ModInfo *x) {
    if (!x->fighter[0] || !x->colors) return "It is not a skin of a fighter's colors";
    if (x->fighters != 1) return "It changes more than one fighter";
    return NULL;
}

static void act_toggle(ModMenu *m) {
    ModInfo *x = selected(m);
    if (!x) return;
    char key[128], err[200], text[240];
    snprintf(key, sizeof key, "%s", x->key);
    int on = !x->enabled;
    if (mod_set_enabled(m->l, m->sel, on, err, sizeof err)) {
        select_key(m, key, m->sel);
        ModInfo *now = selected(m);
        snprintf(text, sizeof text, "%s %s - at the next Play", now ? now->name : key, on ? "enabled" : "disabled");
        say(m, 0, text);
    } else say(m, 1, err);
}

static void act_delete(ModMenu *m) {
    ModInfo *x = selected(m);
    if (!x) return;
    char name[96], err[200], text[240];
    snprintf(name, sizeof name, "%s", x->name);
    int at = m->sel;
    if (mod_delete(m->l, m->sel, err, sizeof err)) {
        select_key(m, NULL, at);
        snprintf(text, sizeof text, "%s deleted", name);
        say(m, 0, text);
    } else { select_key(m, NULL, at); say(m, 1, err); }
}

static void act_move(ModMenu *m) {
    ModInfo *x = selected(m);
    if (!x) return;
    char key[128], err[200], text[240];
    snprintf(key, sizeof key, "%s", x->key);
    int other = mod_occupant(m->l, m->sel, m->delta);
    char other_name[96] = "";
    if (other >= 0) snprintf(other_name, sizeof other_name, "%s", m->l->mods[other].name);
    if (mod_move(m->l, m->sel, m->delta, err, sizeof err)) {
        select_key(m, key, m->sel);
        ModInfo *now = selected(m);
        int c = now && now->colors ? mod_color_to(now, __builtin_ctz(now->colors)) : 0;
        if (*other_name) snprintf(text, sizeof text, "Moved to color %d, %s to its place - at the next Play", c + 1, other_name);
        else snprintf(text, sizeof text, "Moved to color %d - at the next Play", c + 1);
        say(m, 0, text);
    } else say(m, 1, err);
}

int mod_menu_event(ModMenu *m, int ev, int c) {
    if (m->dialog) {
        switch (ev) {
        case MENU_LEFT: case MENU_RIGHT: case MENU_UP: case MENU_DOWN: m->dialog_sel = !m->dialog_sel; break;
        case MENU_ACCEPT: m->dialog = 0; if (m->dialog_sel) act_delete(m); break;
        case MENU_BACK: case MENU_DELETE: m->dialog = 0; break;   /* (nothing deleted) */
        }
        return MOD_EV_NONE;
    }
    if (m->moving) {
        switch (ev) {
        case MENU_LEFT: if (mod_can_move(m->l, m->sel, m->delta - 1)) m->delta--; break;
        case MENU_RIGHT: if (mod_can_move(m->l, m->sel, m->delta + 1)) m->delta++; break;
        case MENU_ACCEPT: m->moving = 0; if (m->delta) act_move(m); m->delta = 0; break;
        case MENU_BACK: case MENU_MOVE: m->moving = 0; m->delta = 0; break;
        case MENU_CHAR: if (c == 'm' || c == 'M') { m->moving = 0; m->delta = 0; } break;
        }
        return MOD_EV_NONE;
    }
    if (ev == MENU_CHAR) ev = c == 'f' || c == 'F' ? MENU_FOLDER : c == 'm' || c == 'M' ? MENU_MOVE : -1;
    ModInfo *x = selected(m);
    switch (ev) {
    case MENU_UP:
        if (m->on_button) { if (m->l->n) { m->on_button = 0; m->sel = m->l->n - 1; } }
        else if (m->sel > 0) m->sel--;
        else m->on_button = 1;   /* (around, to the button) */
        break;
    case MENU_DOWN:
        if (m->on_button) { if (m->l->n) { m->on_button = 0; m->sel = 0; } }
        else if (m->sel + 1 < m->l->n) m->sel++;
        else m->on_button = 1;
        break;
    case MENU_ACCEPT:
        if (m->on_button) return MOD_EV_OPEN_FOLDER;
        act_toggle(m);
        break;
    case MENU_DELETE:
        if (x) { m->dialog = 1; m->dialog_sel = 0; }
        break;
    case MENU_FOLDER: return MOD_EV_OPEN_FOLDER;
    case MENU_MOVE:
        if (!x) break;
        if (unmovable(x)) { say(m, 1, unmovable(x)); break; }
        m->moving = 1;
        m->delta = 0;
        m->note[0] = 0;
        break;
    case MENU_BACK: return MOD_EV_BACK;
    }
    return MOD_EV_NONE;
}

int mod_menu_click_row(ModMenu *m, int row, int on_tag) {
    if (m->dialog) return MOD_EV_NONE;
    if (row == mod_menu_button_row(m)) {
        m->moving = 0;
        m->on_button = 1;
        return MOD_EV_OPEN_FOLDER;
    }
    if (row < 0 || row >= m->l->n) return MOD_EV_NONE;
    if (m->moving && row != m->sel) { m->moving = 0; m->delta = 0; }
    m->on_button = 0;
    int again = m->sel == row;
    m->sel = row;
    if (on_tag && again && !m->moving) act_toggle(m);
    return MOD_EV_NONE;
}

int mod_menu_click_dialog(ModMenu *m, int which) {
    if (!m->dialog) return MOD_EV_NONE;
    m->dialog_sel = which;
    return mod_menu_event(m, MENU_ACCEPT, 0);
}

/* ---- the rows ------------------------------------------------------------------------------- */
int mod_menu_rows(const ModMenu *m) { return (m->l->n ? m->l->n : 1) + 2; }
int mod_menu_button_row(const ModMenu *m) { return mod_menu_rows(m) - 1; }
static int detail_row(const ModMenu *m) { return mod_menu_rows(m) - 2; }
int mod_menu_selected_row(const ModMenu *m) { return m->on_button || m->sel < 0 ? mod_menu_button_row(m) : m->sel; }

/* "color 4", "colors 1, 2 and 5" (as loaded: moved by delta) */
static void colors_text(const ModInfo *x, int delta, char *out, unsigned n) {
    int list[MOD_COLORS], k = 0;
    for (int c = 0; c < MOD_COLORS; c++)
        if (x->colors & 1u << c) list[k++] = mod_color_to(x, c) + delta;
    for (int i = 1; i < k; i++)   /* (in order) */
        for (int j = i; j > 0 && list[j - 1] > list[j]; j--) { int t = list[j]; list[j] = list[j - 1]; list[j - 1] = t; }
    if (!k) { snprintf(out, n, "no colors"); return; }
    if (k == MOD_COLORS) { snprintf(out, n, "all colors"); return; }
    unsigned o = (unsigned)snprintf(out, n, "color%s ", k > 1 ? "s" : "");
    for (int i = 0; i < k && o < n; i++)
        o += (unsigned)snprintf(out + o, n - o, "%s%d", i == 0 ? "" : i == k - 1 ? " and " : ", ", list[i] + 1);
}

static int moved(const ModInfo *x) {
    for (int c = 0; c < MOD_COLORS; c++)
        if (x->colors & 1u << c && mod_color_to(x, c) != c) return 1;
    return 0;
}

static void what_text(const ModInfo *x, char *out, unsigned n) {
    static const char *const KIND[] = {"model", "portraits", "voice", "effects", "other files"};
    unsigned o = 0;
    out[0] = 0;
    for (int k = 0; k < 5 && o < n; k++)
        if (x->kinds & 1u << k) o += (unsigned)snprintf(out + o, n - o, "%s%s", o ? ", " : "", KIND[k]);
}

static void card(const ModMenu *m, int i, char *v, unsigned n) {
    const ModInfo *x = &m->l->mods[i];
    char name[96], sub[200], colors[64];
    mod_ascii(x->name, name, sizeof name);
    int warn = x->conflicts || x->high_colors || x->has_stream || x->gameplay;
    if (x->fighter[0] && x->colors) {
        colors_text(x, m->moving && i == m->sel ? m->delta : 0, colors, sizeof colors);
        snprintf(sub, sizeof sub, "%s%s - %s", mod_fighter_name(x->fighter), x->fighters > 1 ? " and others" : "", colors);
    } else if (x->description[0]) mod_ascii(x->description, sub, sizeof sub);
    else snprintf(sub, sizeof sub, "%d file%s", x->files, x->files == 1 ? "" : "s");
    char initials[4] = "?";
    if (x->fighter[0]) {
        const char *f = mod_fighter_name(x->fighter);
        initials[0] = f[0] >= 'a' && f[0] <= 'z' ? (char)(f[0] - 32) : f[0];
        initials[1] = 0;
    }
    /* '\17', then: enabled (E/D), a warning (!), being moved (>), the fighter's initial; '\t', the
     * name, '\t', the line under it, '\t', the tag */
    snprintf(v, n, "\17%c%c%c%s\t%s\t%s\t%s", x->enabled ? 'E' : 'D', warn ? '!' : ' ', m->moving && i == m->sel ? '>' : ' ', initials, name, sub,
             x->enabled ? "Enabled" : "Disabled");
}

/* The detail: '\22', then fields with '\t' between them - the name; E or D; the description; author
 * and version; what it changes; visual or not; notes ('|' between them, '!' first a warning); the
 * colors' row (the fighter's name '|' eight letters: M the mod, o another mod, W both, . none '|' 1
 * while moving); the actions' hints */
static void detail(const ModMenu *m, const char *const keys[5], char *v, unsigned n) {
    const ModInfo *x = m->sel >= 0 && m->sel < m->l->n ? &m->l->mods[m->sel] : NULL;
    if (!x) {
        snprintf(v, n, "\22%s\tD\t%s\t\t\t\t\t\t%s=Open the mods folder", m->l->off ? "Mods are off" : "No mods yet",
                 m->l->off ? "SSBU_MODS=0: the mods folder is not read." :
                             "Put each mod in a folder of its own inside the mods folder, as for ARCropolis "
                             "(fighter/..., ui/...). A folder whose name starts with a dot is disabled.",
                 keys[3]);
        return;
    }
    char name[96], desc[256], meta[200], what[96], change[240], notes[600], slots[96], acts[200], colors[64];
    mod_ascii(x->name, name, sizeof name);
    mod_ascii(x->description, desc, sizeof desc);
    char a[96], ver[32];
    mod_ascii(x->authors, a, sizeof a);
    mod_ascii(x->version, ver, sizeof ver);
    snprintf(meta, sizeof meta, "%s%s%s%s%s", *a ? "Author: " : "", a, *a && *ver ? "     " : "", *ver ? "Version: " : "", ver);
    what_text(x, what, sizeof what);
    if (x->fighter[0] && x->colors) {
        colors_text(x, m->moving ? m->delta : 0, colors, sizeof colors);
        snprintf(change, sizeof change, "%s%s - %s - %s", mod_fighter_name(x->fighter), x->fighters > 1 ? " and others" : "", colors, what);
    } else snprintf(change, sizeof change, "%s (%d file%s)", *what ? what : "nothing", x->files, x->files == 1 ? "" : "s");
    unsigned o = 0;
    notes[0] = 0;
#define NOTE(...) do { if (o < sizeof notes) o += (unsigned)snprintf(notes + o, sizeof notes - o, "%s", o ? "|" : ""); \
                       if (o < sizeof notes) o += (unsigned)snprintf(notes + o, sizeof notes - o, __VA_ARGS__); } while (0)
    char other[96];
    if (x->conflicts) {
        mod_ascii(x->conflict_with, other, sizeof other);
        NOTE("!%d file%s also in %s: the first in alphabetical order is used", x->conflicts, x->conflicts == 1 ? "" : "s", other);
    }
    if (x->high_colors) NOTE("!New colors (c08 and up) are not supported yet: those files are left out");
    if (x->has_stream) NOTE("!Music (stream:) is not supported yet");
    if (x->has_config) NOTE("-It has a config.json: its extra files may not load yet");
    if (moved(x) && !m->moving) {
        char was[64];
        int from = __builtin_ctz(x->colors);
        snprintf(was, sizeof was, "%d", from + 1);
        NOTE("-Its own color %s, moved here%s", was, x->from_name ? " by its folder's name" : "");
    }
    if (x->fighter[0] && x->fighters == 1 && mod_fighter_varied(x->fighter))
        NOTE("-%s's colors are different characters: moved, its voice or effects may not match", mod_fighter_name(x->fighter));
    if (m->moving) {
        int occ = mod_occupant(m->l, m->sel, m->delta);
        if (occ >= 0) { mod_ascii(m->l->mods[occ].name, other, sizeof other); NOTE("-%s is there: it takes this one's place", other); }
    }
#undef NOTE
    slots[0] = 0;
    if (x->fighter[0] && x->colors && x->fighters == 1) {
        char row[MOD_COLORS + 1];
        unsigned mine = 0, others = 0;
        for (int c = 0; c < MOD_COLORS; c++)
            if (x->colors & 1u << c) {
                int to = mod_color_to(x, c) + (m->moving ? m->delta : 0);
                if (to >= 0 && to < MOD_COLORS) mine |= 1u << to;
            }
        for (int k = 0; k < m->l->n; k++) {
            const ModInfo *y = &m->l->mods[k];
            if (k == m->sel || !y->enabled || y->fighters != 1 || strcmp(y->fighter, x->fighter)) continue;
            for (int c = 0; c < MOD_COLORS; c++)
                if (y->colors & 1u << c) others |= 1u << mod_color_to(y, c);
        }
        for (int c = 0; c < MOD_COLORS; c++)
            row[c] = mine & others & 1u << c ? 'W' : mine & 1u << c ? 'M' : others & 1u << c ? 'o' : '.';
        row[MOD_COLORS] = 0;
        snprintf(slots, sizeof slots, "%s|%s|%d", mod_fighter_name(x->fighter), row, m->moving);
    }
    if (m->moving) snprintf(acts, sizeof acts, "%s=place it here\t%s=cancel", keys[0], keys[4]);
    else snprintf(acts, sizeof acts, "%s=%s\t%s=Move\t%s=Delete", keys[0], x->enabled ? "Disable" : "Enable", keys[2], keys[1]);
    snprintf(v, n, "\22%s\t%c\t%s\t%s\t%s\t%s\t%s\t%s\t%s", name, x->enabled ? 'E' : 'D', desc, meta, change,
             x->gameplay ? "!It changes more than looks: it can desync online play" : "Visual only", notes, slots, acts);
}

void mod_menu_row(const ModMenu *m, int row, const char *const keys[5], char *label, unsigned ln, char *value, unsigned vn) {
    label[0] = value[0] = 0;
    if (row == mod_menu_button_row(m)) { snprintf(label, ln, "Open Mods Folder"); return; }
    if (row == detail_row(m)) { detail(m, keys, value, vn); return; }
    if (!m->l->n) {
        snprintf(value, vn, m->l->off ? "\3Mods are off (SSBU_MODS=0)" : "\5No mods yet - Open Mods Folder, then copy them there");
        return;
    }
    if (row >= 0 && row < m->l->n) card(m, row, value, vn);
}

void mod_menu_dialog_text(const ModMenu *m, char *out, unsigned n) {
    char name[96];
    mod_ascii(m->sel >= 0 && m->sel < m->l->n ? m->l->mods[m->sel].name : "", name, sizeof name);
    snprintf(out, n, "Delete mod\tDelete \"%s\" and all its files? This cannot be undone.", name);
}
