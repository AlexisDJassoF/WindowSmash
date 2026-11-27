/* The start-up menu's Mods page: see mod_menu.h. */
#include "mod_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void read_hosts(ModMenu *m) {   /* the hosts whose room mods are kept */
    char cache[600];
    m->nhosts = 0;
    if (m->l->off || !m->l->root[0]) return;
    mod_cache_dir(m->l->root, cache, sizeof cache);
    m->nhosts = mod_cache_hosts(cache, m->hosts, MENU_HOSTS);
    if (!mod_room_chosen(&m->in_use, &m->in_use_mode)) m->in_use = 0;
}

static int items(const ModMenu *m) { return m->l->n + m->nhosts; }   /* (what can be selected: the mods, then the hosts) */
static int on_page(const ModMenu *m) { return m->list || m->level == 1; }   /* (a page of cards, not the grid) */
static const HostSummary *host_of(const ModMenu *m) {
    return on_page(m) && !m->on_button && m->sel >= m->l->n && m->sel < items(m) ? &m->hosts[m->sel - m->l->n] : NULL;
}
static int pos_of(const ModMenu *m, int item) {   /* its place in the page, -1 none */
    for (int k = 0; k < m->npage; k++) if (m->page[k] == item) return k;
    return -1;
}

static void say(ModMenu *m, int bad, const char *text);

/* ---- the grid's tiles ---------------------------------------------------------------------- */
static unsigned dest_colors(const ModInfo *x) {   /* its colors as loaded */
    unsigned out = 0;
    for (int c = 0; c < MOD_COLORS; c++)
        if (x->colors & 1u << c) { int to = mod_color_to(x, c); if (to >= 0 && to < MOD_COLORS) out |= 1u << to; }
    return out;
}

static void mark(RosterSum *s, char kind, const char *why) {   /* (the first reason of the worst kind) */
    if (s->mark == 'x' || (s->mark == '!' && kind == '!')) return;
    s->mark = kind;
    mod_ascii(why, s->why, sizeof s->why);
}

/* The tiles' mods counted, the page's items listed again: after the folder is read (any action) */
static void regroup(ModMenu *m) {
    const ModList *l = m->l;
    memset(m->sum, 0, sizeof m->sum);
    for (int i = 0; i < l->n; i++) m->tile_of[i] = (unsigned char)mod_roster_of(&l->mods[i]);
    for (int i = 0; i < l->n; i++) {
        const ModInfo *x = &l->mods[i];
        RosterSum *s = &m->sum[m->tile_of[i]];
        if (!x->enabled) { s->off++; continue; }
        s->on++;
        char why[240];
        if (x->high_colors) { snprintf(why, sizeof why, "%s has new colors (c08 and up): those files are left out", x->name); mark(s, 'x', why); }
        if (x->fighter[0] && x->fighters == 1 && x->colors)
            for (int k = 0; k < l->n; k++) {
                const ModInfo *y = &l->mods[k];
                if (k == i || !y->enabled || y->fighters != 1 || strcmp(y->fighter, x->fighter)) continue;
                unsigned both = dest_colors(x) & dest_colors(y);
                if (!both) continue;
                snprintf(why, sizeof why, "%s and %s are both in color %d", k < i ? y->name : x->name, k < i ? x->name : y->name, __builtin_ctz(both) + 1);
                mark(s, 'x', why);
                break;
            }
        if (x->gameplay) { snprintf(why, sizeof why, "%s changes more than looks", x->name); mark(s, '!', why); }
        if (x->has_stream) { snprintf(why, sizeof why, "%s has music (stream:), not supported yet", x->name); mark(s, '!', why); }
        if (x->has_config) { snprintf(why, sizeof why, "%s has a config.json", x->name); mark(s, '!', why); }
        if (x->conflicts) { snprintf(why, sizeof why, "%s has files %s also has", x->name, x->conflict_with); mark(s, '!', why); }
    }
    for (int k = 0; k < m->nhosts; k++) {   /* (the rooms' mods: in Other, enabled while this start uses them) */
        if (m->in_use == m->hosts[k].host_id) m->sum[ROSTER_OTHER].on++;
        else m->sum[ROSTER_OTHER].off++;
    }
    m->npage = m->pmods = 0;
    for (int i = 0; i < l->n; i++)
        if (m->list || m->tile_of[i] == m->tile) m->page[m->npage++] = i;
    m->pmods = m->npage;
    if (m->list || m->tile == ROSTER_OTHER)
        for (int k = 0; k < m->nhosts; k++) m->page[m->npage++] = l->n + k;
}

static int grid_rows(const ModMenu *m) { return (ROSTER_TILES + m->cols - 1) / m->cols; }
static void center(ModMenu *m) {   /* the cursor on the selected tile's centre */
    m->cur_x = (float)(m->tile % m->cols) + 0.5f;
    m->cur_y = (float)(m->tile / m->cols) + 0.5f;
}

void mod_menu_init(ModMenu *m, ModList *l) {
    memset(m, 0, sizeof *m);
    m->l = l;
    const char *mode = getenv("SSBU_MENU_MODS");
    m->list = mode && !strcmp(mode, "list");
    m->cols = 12;
    read_hosts(m);
    regroup(m);
    if (m->list) {
        m->level = 1;
        m->sel = items(m) ? 0 : -1;
        m->on_button = !items(m);
    } else {
        m->sel = -1;
        m->cursor_shown = 1;
        center(m);
    }
}

/* The selection after the folder was read: the mod called key, else the page's item at pos */
static void select_key(ModMenu *m, const char *key, int pos) {
    regroup(m);
    if (!on_page(m)) return;
    int i = key && *key ? mod_find(m->l, key) : -1;
    if (i >= 0 && pos_of(m, i) < 0) i = -1;
    if (i < 0 && m->pmods) i = m->page[pos < 0 ? 0 : pos < m->pmods ? pos : m->pmods - 1];   /* (a mod's card, as before: not a host's) */
    m->sel = i;
    m->on_button = i < 0;
}

void mod_menu_reload(ModMenu *m) {
    char key[128] = "", root[512];
    if (m->sel >= 0 && m->sel < m->l->n) snprintf(key, sizeof key, "%s", m->l->mods[m->sel].key);
    snprintf(root, sizeof root, "%s", m->l->root);
    int at = m->sel, pos = pos_of(m, m->sel), button = m->on_button;
    mod_scan(m->l, root);
    read_hosts(m);
    if (at >= m->l->n && at < items(m) && on_page(m)) { regroup(m); m->sel = at; return; }   /* (a host's card: where it was) */
    select_key(m, key, pos);
    if (button && on_page(m)) m->on_button = 1;
}

static ModInfo *selected(ModMenu *m) { return on_page(m) && !m->on_button && m->sel >= 0 && m->sel < m->l->n ? &m->l->mods[m->sel] : NULL; }


static void act_delete_host(ModMenu *m) {
    const HostSummary *h = host_of(m);
    if (!h) return;
    char cache[600], name[64], text[160];
    mod_ascii(h->name, name, sizeof name);
    mod_cache_dir(m->l->root, cache, sizeof cache);
    int ok = mod_cache_delete_host(cache, h->host_id), pos = pos_of(m, m->sel);
    read_hosts(m);
    regroup(m);
    m->sel = m->npage ? m->page[pos < m->npage ? pos : m->npage - 1] : -1;
    if (m->sel < 0) m->on_button = 1;
    snprintf(text, sizeof text, ok ? "%s's room mods deleted" : "Cannot delete %s's room mods", name);
    say(m, !ok, text);
}

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
    int on = !x->enabled, pos = pos_of(m, m->sel);
    if (mod_set_enabled(m->l, m->sel, on, err, sizeof err)) {
        select_key(m, key, pos);
        ModInfo *now = selected(m);
        snprintf(text, sizeof text, "%s %s - at the next Play", now ? now->name : key, on ? "enabled" : "disabled");
        say(m, 0, text);
    } else say(m, 1, err);
}

static void act_share(ModMenu *m) {
    ModInfo *x = selected(m);
    if (!x) return;
    char err[200], text[240];
    int on = x->no_share;
    if (mod_set_shared(m->l, m->sel, on, err, sizeof err)) {
        regroup(m);
        snprintf(text, sizeof text, on ? "%s shared in the rooms you host" : "%s not shared in the rooms you host", x->name);
        say(m, 0, text);
    } else say(m, 1, err);
}

static void act_delete(ModMenu *m) {
    ModInfo *x = selected(m);
    if (!x) return;
    char name[96], err[200], text[240];
    snprintf(name, sizeof name, "%s", x->name);
    int at = pos_of(m, m->sel);
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
        select_key(m, key, pos_of(m, m->sel));
        ModInfo *now = selected(m);
        int c = now && now->colors ? mod_color_to(now, __builtin_ctz(now->colors)) : 0;
        if (*other_name) snprintf(text, sizeof text, "Moved to color %d, %s to its place - at the next Play", c + 1, other_name);
        else snprintf(text, sizeof text, "Moved to color %d - at the next Play", c + 1);
        say(m, 0, text);
    } else say(m, 1, err);
}

/* The selected mod's colors as loaded, in order: how many, into list */
static int dest_list(const ModInfo *x, int list[MOD_COLORS]) {
    int k = 0;
    for (int c = 0; c < MOD_COLORS; c++)
        if (x->colors & 1u << c) list[k++] = mod_color_to(x, c);
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && list[j - 1] > list[j]; j--) { int t = list[j]; list[j] = list[j - 1]; list[j - 1] = t; }
    return k;
}

/* ---- the pictures' keys (gpu_menu_pics.h: "arc:<data.arc path>", "file:<path>") ---- */
/* The name a fighter's portraits and icons go by in ui/replace/chara/: Pyra's and Mythra's "eflame_first"
 * and "elight_first", the rest of a tile's (Pokemon Trainer's Pokemon, Popo and Nana) its own */
static const char *pic_name(const char *fighter) {
    if (!strcmp(fighter, "eflame")) return "eflame_first";
    if (!strcmp(fighter, "elight")) return "elight_first";
    const RosterTile *r = mod_roster(mod_roster_tile(fighter));
    return r ? r->portrait : fighter;
}

/* The mod's own ui/replace[_patch]/chara/chara_<kind>/ file for its color c as loaded (dest): its
 * key ("file:<path>") into out, 1 if it has one */
static int mod_picture(const ModMenu *m, const ModInfo *x, int kind, int dest, char *out, unsigned n) {
    char want[16];
    snprintf(want, sizeof want, "/chara_%d/", kind);
    for (int i = 0; i < x->npaths; i++) {
        const char *p = x->paths[i];
        char f[64];
        int c;
        if (!strstr(p, want) || !mod_path_color(p, f, sizeof f, &c) || strcmp(f, x->fighter) || c >= MOD_COLORS) continue;
        if (mod_color_to(x, c) != dest) continue;
        snprintf(out, n, "file:%s/%s/%s", m->l->root, x->dir, x->real ? x->real[i] : p);
        return 1;
    }
    return 0;
}

/* ---- the recovery ----------------------------------------------------------------------------- */
void mod_menu_recovery(ModMenu *m, const Recovery *r, const char *dir) {
    m->rec = *r;
    snprintf(m->rec_dir, sizeof m->rec_dir, "%s", dir);
    m->nrec_opts = 0;
    int i = r->suspect[0] ? mod_find(m->l, r->suspect) : -1;
    if (i >= 0 && m->l->mods[i].enabled) m->rec_opts[m->nrec_opts++] = REC_OPT_DISABLE;
    if (r->failed) m->rec_opts[m->nrec_opts++] = REC_OPT_LAST_GOOD;
    m->rec_opts[m->nrec_opts++] = REC_OPT_NO_MODS;
    m->rec_opts[m->nrec_opts++] = REC_OPT_IGNORE;
    m->dialog = 2;
    m->dialog_sel = 0;
    if (r->safe && !r->failed) m->dialog_sel = m->nrec_opts - 2;   /* (the safe mode: without mods) */
    if (i >= 0 && !m->list) {   /* (the grid on the suspect's tile) */
        m->tile = m->tile_of[i];
        center(m);
    }
}

static void act_last_good(ModMenu *m) {   /* the mods enabled since the last start that worked: disabled */
    static char good[256][128];
    int ngood = mod_recovery_last_good(m->rec_dir, good, 256), off = 0, bad = 0;
    char err[200], keys[MOD_MAX][128];
    int nk = 0;
    for (int i = 0; i < m->l->n; i++) {   /* (by name: each change reads the folder again) */
        int known = 0;
        for (int k = 0; k < ngood && !known; k++) known = !strcmp(good[k], m->l->mods[i].key);
        if (m->l->mods[i].enabled && !known) snprintf(keys[nk++], sizeof keys[0], "%s", m->l->mods[i].key);
    }
    for (int k = 0; k < nk; k++) {
        int i = mod_find(m->l, keys[k]);
        if (i >= 0 && mod_set_enabled(m->l, i, 0, err, sizeof err)) off++;
        else bad++;
    }
    regroup(m);
    char text[200];
    if (bad) snprintf(text, sizeof text, "%d mod%s disabled, %d could not be", off, off == 1 ? "" : "s", bad);
    else snprintf(text, sizeof text, off ? "%d mod%s added since the last start that worked disabled" : "No mod was added since the last start that worked",
                  off, off == 1 ? "" : "s");
    say(m, bad != 0, text);
}

static int recovery_event(ModMenu *m, int ev) {
    switch (ev) {
    case MENU_UP: case MENU_LEFT: m->dialog_sel = (m->dialog_sel + m->nrec_opts - 1) % m->nrec_opts; return MOD_EV_NONE;
    case MENU_DOWN: case MENU_RIGHT: m->dialog_sel = (m->dialog_sel + 1) % m->nrec_opts; return MOD_EV_NONE;
    case MENU_BACK: m->dialog = 0; mod_recovery_chosen(); return MOD_EV_NONE;
    case MENU_ACCEPT: break;
    default: return MOD_EV_NONE;
    }
    m->dialog = 0;
    mod_recovery_chosen();
    switch (m->rec_opts[m->dialog_sel]) {
    case REC_OPT_DISABLE: {
        int i = mod_find(m->l, m->rec.suspect);
        char err[200], text[240];
        if (i >= 0 && mod_set_enabled(m->l, i, 0, err, sizeof err)) {
            snprintf(text, sizeof text, "%s disabled - the others stay", m->rec.suspect);
            regroup(m);
            say(m, 0, text);
        } else say(m, 1, i < 0 ? "That mod is not in the folder any more" : err);
        return MOD_EV_NONE;
    }
    case REC_OPT_LAST_GOOD: act_last_good(m); return MOD_EV_NONE;
    case REC_OPT_NO_MODS: say(m, 0, "This start goes without mods - they stay as they are"); return MOD_EV_NO_MODS;
    }
    return MOD_EV_NONE;
}

/* ---- the grid ------------------------------------------------------------------------------ */
static void open_tile(ModMenu *m, int tile) {   /* its page, its first mod selected */
    m->tile = tile;
    m->level = 1;
    m->moving = 0, m->delta = 0, m->preview = 0;
    regroup(m);
    m->sel = m->npage ? m->page[0] : -1;
    m->on_button = !m->npage;
}

static void step(ModMenu *m, int tile) {   /* the D-pad's: the cursor shown, on its centre */
    m->tile = tile;
    m->on_button = 0;
    m->cursor_shown = 1;
    center(m);
}

static int grid_event(ModMenu *m, int ev) {
    int t = m->tile, cols = m->cols;
    switch (ev) {
    case MENU_LEFT: if (!m->on_button) step(m, t > 0 ? t - 1 : t); break;
    case MENU_RIGHT: if (!m->on_button) step(m, t + 1 < ROSTER_TILES ? t + 1 : t); break;
    case MENU_UP:
        if (m->on_button) step(m, t);   /* (back to the grid, where it was) */
        else if (t >= cols) step(m, t - cols);
        else step(m, t);
        break;
    case MENU_DOWN:
        if (m->on_button) break;
        if (t + cols < ROSTER_TILES) step(m, t + cols);
        else if (t / cols < grid_rows(m) - 1) step(m, ROSTER_TILES - 1);   /* (nothing under it: the last tile) */
        else m->on_button = 1;   /* (the last row: the button) */
        break;
    case MENU_ACCEPT:
        if (m->on_button) return MOD_EV_OPEN_FOLDER;
        open_tile(m, t);
        break;
    case MENU_FOLDER: return MOD_EV_OPEN_FOLDER;
    case MENU_BACK: return MOD_EV_BACK;
    }
    return MOD_EV_NONE;
}

int mod_menu_grid(const ModMenu *m) { return !on_page(m); }

void mod_menu_stick(int x, int y, int deadzone, float *ax, float *ay) {
    double max = 32767.0, r = sqrt((double)x * x + (double)y * y);
    double dz = (deadzone < CURSOR_MIN_DEADZONE ? CURSOR_MIN_DEADZONE : deadzone) / 100.0 * max;
    *ax = *ay = 0;
    if (r <= dz) return;
    double scale = (r - dz) / (max - dz) / r;   /* (as hle_pad.c's stick: the rest of the travel stretched to all of it) */
    if (r * scale > 1) scale = 1 / r;
    *ax = (float)(x * scale), *ay = (float)(y * scale);
}

void mod_menu_cursor(ModMenu *m, float ax, float ay, float dt) {
    if (on_page(m) || m->dialog || dt <= 0) return;
    float mag = sqrtf(ax * ax + ay * ay);
    if (mag <= 0) return;
    if (m->on_button) {   /* (up: back to the grid; the rest, nothing) */
        if (ay < -0.5f) step(m, m->tile);
        return;
    }
    if (!m->cursor_shown) { center(m); m->cursor_shown = 1; }   /* (after the mouse: from the tile selected) */
    if (mag > 1) ax /= mag, ay /= mag, mag = 1;
    float v = mag * mag * CURSOR_VEL;   /* (the curve: t squared) */
    float fx = m->cur_x - floorf(m->cur_x) - 0.5f, fy = m->cur_y - floorf(m->cur_y) - 0.5f;
    if (fx * fx + fy * fy < CURSOR_BRAKE_R * CURSOR_BRAKE_R) v *= CURSOR_BRAKE;
    m->cur_x += ax / mag * v * dt;
    m->cur_y += ay / mag * v * dt;
    /* inside the grid: its rectangle, less the last row's empty end (the row above instead) */
    int rows = grid_rows(m), last = ROSTER_TILES - (rows - 1) * m->cols;
    const float in = 0.001f;
    if (m->cur_x < 0) m->cur_x = 0;
    if (m->cur_x > m->cols - in) m->cur_x = m->cols - in;
    if (m->cur_y < 0) m->cur_y = 0;
    if (m->cur_y > rows - in) m->cur_y = rows - in;
    if ((int)m->cur_y == rows - 1 && m->cur_x >= last) {
        if (rows > 1) m->cur_y = rows - 1 - in;
        else m->cur_x = last - in;
    }
    m->tile = (int)m->cur_y * m->cols + (int)m->cur_x;
}

void mod_menu_hover(ModMenu *m, int tile) {
    if (on_page(m) || m->dialog || tile < 0 || tile >= ROSTER_TILES) return;
    m->tile = tile;
    m->on_button = 0;
    m->cursor_shown = 0;
}

int mod_menu_click_tile(ModMenu *m, int tile) {
    if (on_page(m) || m->dialog || tile < 0 || tile >= ROSTER_TILES) return MOD_EV_NONE;
    m->cursor_shown = 0;
    open_tile(m, tile);
    return MOD_EV_NONE;
}

void mod_menu_set_cols(ModMenu *m, int cols) {
    if (cols < 1 || cols == m->cols) return;
    m->cols = cols;
    center(m);   /* (the tile kept, where it is now) */
}

int mod_menu_event(ModMenu *m, int ev, int c) {
    if (m->dialog == 2) return recovery_event(m, ev);
    if (m->dialog) {
        switch (ev) {
        case MENU_LEFT: case MENU_RIGHT: case MENU_UP: case MENU_DOWN: m->dialog_sel = !m->dialog_sel; break;
        case MENU_ACCEPT: m->dialog = 0; if (m->dialog_sel) { if (host_of(m)) act_delete_host(m); else act_delete(m); } break;
        case MENU_BACK: case MENU_DELETE: m->dialog = 0; break;   /* (nothing deleted) */
        }
        return MOD_EV_NONE;
    }
    if (m->moving) {
        switch (ev) {
        case MENU_LEFT: case MENU_RIGHT: {   /* (the next color it can go to: one of another body is passed over) */
            int step = ev == MENU_LEFT ? -1 : 1;
            for (int d = m->delta + step; d > -MOD_COLORS && d < MOD_COLORS; d += step)
                if (mod_can_move(m->l, m->sel, d)) { m->delta = d; break; }
            break;
        }
        case MENU_ACCEPT: m->moving = 0; if (m->delta) act_move(m); m->delta = 0; break;
        case MENU_BACK: case MENU_MOVE: m->moving = 0; m->delta = 0; break;
        case MENU_CHAR: if (c == 'm' || c == 'M') { m->moving = 0; m->delta = 0; } break;
        }
        return MOD_EV_NONE;
    }
    if (ev == MENU_CHAR) ev = c == 'f' || c == 'F' ? MENU_FOLDER : c == 'm' || c == 'M' ? MENU_MOVE : c == 's' || c == 'S' ? MENU_SHARE : -1;
    if (!on_page(m)) return grid_event(m, ev);
    ModInfo *x = selected(m);
    int pos = pos_of(m, m->sel), was = m->sel;
    switch (ev) {
    case MENU_LEFT: case MENU_RIGHT:   /* (the preview: the mod's other colors) */
        if (x) {
            int list[MOD_COLORS], k = dest_list(x, list);
            m->preview += ev == MENU_RIGHT ? 1 : -1;
            if (m->preview >= k) m->preview = k - 1;
            if (m->preview < 0) m->preview = 0;
        }
        break;
    case MENU_UP:
        if (m->on_button) { if (m->npage) { m->on_button = 0; m->sel = m->page[m->npage - 1]; } }
        else if (pos > 0) m->sel = m->page[pos - 1];
        else m->on_button = 1;   /* (around, to the button) */
        break;
    case MENU_DOWN:
        if (m->on_button) { if (m->npage) { m->on_button = 0; m->sel = m->page[0]; } }
        else if (pos >= 0 && pos + 1 < m->npage) m->sel = m->page[pos + 1];
        else m->on_button = 1;
        break;
    case MENU_ACCEPT:
        if (m->on_button) return MOD_EV_OPEN_FOLDER;
        if (host_of(m)) { say(m, 0, "Used when you join that host's room again (X deletes them)"); break; }
        act_toggle(m);
        break;
    case MENU_DELETE:
        if (x || host_of(m)) { m->dialog = 1; m->dialog_sel = 0; }
        break;
    case MENU_FOLDER: return MOD_EV_OPEN_FOLDER;
    case MENU_SHARE: act_share(m); break;
    case MENU_MOVE:
        if (!x) break;
        if (unmovable(x)) { say(m, 1, unmovable(x)); break; }
        m->moving = 1;
        m->delta = 0;
        m->note[0] = 0;
        break;
    case MENU_BACK:
        if (m->list) return MOD_EV_BACK;
        m->preview = 0;
        m->level = 0;   /* (the grid, the cursor on the tile it came from) */
        m->on_button = 0;
        m->sel = -1;
        m->cursor_shown = 1;
        center(m);
        break;
    }
    if (m->sel != was) m->preview = 0;
    return MOD_EV_NONE;
}

/* The page's card at row (past the colors' row), as an item; -1 none (the note) */
static int colors_rows(const ModMenu *m) { return m->list || m->tile == ROSTER_OTHER ? 0 : 1; }
static int card_item(const ModMenu *m, int row) {
    row -= colors_rows(m);
    int base = m->pmods ? m->pmods : 1;   /* (the hosts' cards after the mods', or after the note when there are none) */
    if (row >= 0 && row < m->pmods) return m->page[row];
    if (row >= base && row < base + m->npage - m->pmods) return m->page[m->pmods + row - base];
    return -1;
}

int mod_menu_click_row(ModMenu *m, int row, int on_tag) {
    if (m->dialog) return MOD_EV_NONE;
    if (row == mod_menu_button_row(m)) {
        m->moving = 0;
        m->on_button = 1;
        return MOD_EV_OPEN_FOLDER;
    }
    if (!on_page(m)) return MOD_EV_NONE;   /* (the grid's tiles: mod_menu_click_tile) */
    int item = card_item(m, row);
    if (item < 0) return MOD_EV_NONE;
    if (item >= m->l->n) {
        m->moving = 0, m->delta = 0, m->on_button = 0;
        m->sel = item;
        return MOD_EV_NONE;
    }
    if (m->moving && item != m->sel) { m->moving = 0; m->delta = 0; }
    m->on_button = 0;
    int again = m->sel == item;
    if (!again) m->preview = 0;
    m->sel = item;
    if (on_tag && again && !m->moving) act_toggle(m);
    return MOD_EV_NONE;
}

int mod_menu_click_action(ModMenu *m, int k) {
    if (m->dialog || !on_page(m) || k < 0) return MOD_EV_NONE;
    int ev = -1;
    if (m->moving) ev = k == 0 ? MENU_ACCEPT : k == 1 ? MENU_BACK : -1;           /* place it here, cancel */
    else if (host_of(m)) ev = k == 0 ? MENU_DELETE : -1;                           /* Delete */
    else if (!selected(m)) ev = k == 0 ? MENU_FOLDER : -1;                         /* Open the mods folder */
    else {
        static const int ACTS[] = {MENU_ACCEPT, MENU_MOVE, MENU_SHARE, MENU_DELETE};   /* (detail's order: Enable/Disable, Move, Share, Delete) */
        ev = k < 4 ? ACTS[k] : -1;
    }
    if (ev < 0) return MOD_EV_NONE;
    if (ev == MENU_ACCEPT && !m->moving) m->on_button = 0;
    return mod_menu_event(m, ev, 0);
}

int mod_menu_click_color(ModMenu *m, int c) {
    ModInfo *x = selected(m);
    if (m->dialog || !x || c < 0 || c >= MOD_COLORS) return MOD_EV_NONE;
    int list[MOD_COLORS];
    if (!dest_list(x, list)) return MOD_EV_NONE;
    int want = c - list[0];   /* (all its colors together, the first one to c) */
    if (!m->moving) {
        if (unmovable(x)) { say(m, 1, unmovable(x)); return MOD_EV_NONE; }
        m->moving = 1, m->delta = 0, m->note[0] = 0;
    }
    if (mod_can_move(m->l, m->sel, want)) m->delta = want;
    return MOD_EV_NONE;
}

int mod_menu_click_dialog(ModMenu *m, int which) {
    if (!m->dialog) return MOD_EV_NONE;
    m->dialog_sel = which;
    return mod_menu_event(m, MENU_ACCEPT, 0);
}

/* ---- the rows ------------------------------------------------------------------------------- */
int mod_menu_rows(const ModMenu *m) {
    if (!on_page(m)) return 2;   /* (the grid, the button) */
    return colors_rows(m) + (m->pmods ? m->pmods : 1) + m->npage - m->pmods + 2;
}
int mod_menu_button_row(const ModMenu *m) { return mod_menu_rows(m) - 1; }
static int detail_row(const ModMenu *m) { return on_page(m) ? mod_menu_rows(m) - 2 : -1; }
int mod_menu_selected_row(const ModMenu *m) {
    if (m->on_button) return mod_menu_button_row(m);
    if (!on_page(m)) return 0;
    int pos = pos_of(m, m->sel);
    if (pos < 0) return mod_menu_button_row(m);
    return colors_rows(m) + (pos < m->pmods ? pos : (m->pmods ? m->pmods : 1) + pos - m->pmods);
}

static void ago(long long last, char *out, size_t n) {
    long long d = (long long)time(NULL) - last;
    if (d < 3600) snprintf(out, n, "last joined just now");
    else if (d < 86400) snprintf(out, n, "last joined %lld hour%s ago", d / 3600, d / 3600 == 1 ? "" : "s");
    else snprintf(out, n, "last joined %lld day%s ago", d / 86400, d / 86400 == 1 ? "" : "s");
}

static void host_size(uint64_t b, char *out, size_t n) {
    if (b >= 1 << 20) snprintf(out, n, "%llu MB", (unsigned long long)((b + (1 << 19)) >> 20));
    else snprintf(out, n, "%llu KB", (unsigned long long)((b + 1023) >> 10));
}

/* A host's card: as a mod's, its tag Room (or In use) */
static void host_card(const ModMenu *m, int k, char *v, unsigned n) {
    const HostSummary *h = &m->hosts[k];
    char name[64], when[64], size[32];
    mod_ascii(h->name, name, sizeof name);
    ago(h->last, when, sizeof when);
    host_size(h->bytes, size, sizeof size);
    int used = m->in_use == h->host_id;
    snprintf(v, n, "\17%c  R\t%s's room mods\t%d mod%s, %s - %s\t%s", h->active ? 'E' : 'D', name, h->active, h->active == 1 ? "" : "s", size, when,
             used ? "In use" : "Room");
}

static void host_detail(const ModMenu *m, const char *const keys[6], char *v, unsigned n) {
    const HostSummary *h = host_of(m);
    char name[64], when[64], size[32], change[160], use[200];
    mod_ascii(h->name, name, sizeof name);
    ago(h->last, when, sizeof when);
    when[0] = 'L';   /* ("Last joined ...") */
    host_size(h->bytes, size, sizeof size);
    snprintf(change, sizeof change, "%d mod%s the host plays with, %d kept from before - %s", h->active, h->active == 1 ? "" : "s", h->mods - h->active, size);
    if (m->in_use == h->host_id) snprintf(use, sizeof use, "In use this start (%s)", m->in_use_mode == ROOM_OVERLAY ? "overlay: your own skins win" : "mirror");
    else snprintf(use, sizeof use, "Used when you join %s's room, in place of yours (or under your skins)", name);
    snprintf(v, n, "\22%s's room mods\t%c\tKept from the rooms of %s you joined: used again there, and only what changes is downloaded. "
                   "Your own mods are left as they are.\t%s\t%s\t%s\t-Forgotten %d days after you last join that room\t\t\t%s=Delete",
             name, h->active ? 'E' : 'D', name, when, change, use, HOST_DAYS, keys[1]);
}

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
    /* its icon: the mod's own stock icon of its first color as loaded, else the game's */
    char icon[600] = "";
    if (x->fighter[0] && x->colors) {
        int dest = mod_color_to(x, __builtin_ctz(x->colors)) + (m->moving && i == m->sel ? m->delta : 0);
        if (dest >= 0 && dest < MOD_COLORS && !mod_picture(m, x, 2, dest, icon, sizeof icon))
            snprintf(icon, sizeof icon, "arc:ui/replace/chara/chara_2/chara_2_%s_%02d.bntx", pic_name(x->fighter), dest);
    }
    /* '\17', then: enabled (E/D), a warning (!), being moved (>), the fighter's initial; '\t', the
     * name, '\t', the line under it, '\t', the tag, '\t', its icon's key (none: the initial) */
    snprintf(v, n, "\17%c%c%c%s\t%s\t%s\t%s\t%s", x->enabled ? 'E' : 'D', warn ? '!' : ' ', m->moving && i == m->sel ? '>' : ' ', initials, name, sub,
             x->enabled ? "Enabled" : "Disabled", icon);
}

/* The preview (mod-preview): the game's portrait of the color shown and the mod's (its chara_3 of that
 * color, else its preview.png) - "<game's key>|<the mod's key, none: the game's again>|<a note under
 * the mod's side>|<the caption>"; nothing for a mod of no fighter's colors */
static void preview_field(const ModMenu *m, const ModInfo *x, char *out, unsigned n) {
    int list[MOD_COLORS], k = x->fighter[0] && x->fighters == 1 ? dest_list(x, list) : 0;
    out[0] = 0;
    if (!k) return;
    int at = m->preview < 0 ? 0 : m->preview >= k ? k - 1 : m->preview;
    int dest = list[at] + (m->moving ? m->delta : 0);
    if (dest < 0 || dest >= MOD_COLORS) return;
    char mine[600] = "", caption[96];
    int has = mod_picture(m, x, 3, list[at], mine, sizeof mine);
    if (!has && x->preview[0]) snprintf(mine, sizeof mine, "file:%s/%s/%s", m->l->root, x->dir, x->preview), has = 1;   /* (its preview.png instead) */
    if (k > 1) snprintf(caption, sizeof caption, "%s - color %d (%d/%d)", mod_fighter_name(x->fighter), dest + 1, at + 1, k);
    else snprintf(caption, sizeof caption, "%s - color %d", mod_fighter_name(x->fighter), dest + 1);
    snprintf(out, n, "arc:ui/replace/chara/chara_3/chara_3_%s_%02d.bntx|%s|%s|%s", pic_name(x->fighter), dest, mine, has ? "" : "No portrait", caption);
}

/* The detail: '\22', then fields with '\t' between them - the name; E or D; the description; author
 * and version; what it changes; visual or not; notes ('|' between them, '!' first a warning); the
 * colors' row (the fighter's name '|' eight letters: M the mod, o another mod, W both, . none '|' 1
 * while moving; SSBU_MENU_MODS=list alone); the preview (preview_field); the actions' hints */
static void detail(const ModMenu *m, const char *const keys[6], char *v, unsigned n) {
    const ModInfo *x = m->sel >= 0 && m->sel < m->l->n ? &m->l->mods[m->sel] : NULL;
    if (!x) {
        char none[80] = "No mods yet";
        if (!m->list && m->tile != ROSTER_OTHER) snprintf(none, sizeof none, "No %s mods yet", mod_roster_name(m->tile));
        snprintf(v, n, "\22%s\tD\t%s\t\t\t\t\t\t\t%s=Open the mods folder", m->l->off ? "Mods are off" : none,
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
    if (x->no_share) NOTE(x->gameplay ? "-Not shared: who joins your room is told its name, not given its files"
                                      : "-Not shared: who joins your room does not get it");
    if (x->fighter[0] && x->fighters == 1 && mod_fighter_varied(x->fighter))
        NOTE("-%s's colors are different characters: moved, its voice or effects may not match", mod_fighter_name(x->fighter));
    if (m->moving) {
        int occ = mod_occupant(m->l, m->sel, m->delta);
        if (occ >= 0) { mod_ascii(m->l->mods[occ].name, other, sizeof other); NOTE("-%s is there: it takes this one's place", other); }
    }
#undef NOTE
    slots[0] = 0;   /* (a tile's page: its colors' row instead, above the cards) */
    if (m->list && x->fighter[0] && x->colors && x->fighters == 1) {
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
    else snprintf(acts, sizeof acts, "%s=%s\t%s=Move\t%s=%s\t%s=Delete", keys[0], x->enabled ? "Disable" : "Enable", keys[2], keys[5],
                  x->no_share ? "Share" : "Don't share", keys[1]);
    char preview[1400];
    preview_field(m, x, preview, sizeof preview);
    snprintf(v, n, "\22%s\t%c\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s", name, x->enabled ? 'E' : 'D', desc, meta, change,
             x->gameplay ? "!It changes more than looks: it can desync online play" : "Visual only", notes, slots, preview, acts);
}

/* The grid ('\23'), fields with '\t' between them: the tile selected; 1 if the button is; the
 * cursor's x and y (in tiles); 1 if it is shown; the label - the tile's name, its mods, its mark's
 * reason ('x' or '!' first); then a field per tile: its state (A enabled mods, D only disabled ones,
 * N none), its mark ('x', '!' or ' '), how many enabled, ',', its portrait's name (chara_7_<it>_00:
 * none for Other), ',', its name */
static void grid_row(const ModMenu *m, char *v, unsigned n) {
    const RosterSum *s = &m->sum[m->tile];
    char sub[96], why[130] = "";
    int all = s->on + s->off;
    if (m->l->off) snprintf(sub, sizeof sub, "Mods are off (SSBU_MODS=0)");
    else if (!all) snprintf(sub, sizeof sub, "No mods");
    else snprintf(sub, sizeof sub, "%d mod%s, %d enabled", all, all == 1 ? "" : "s", s->on);
    if (s->mark) snprintf(why, sizeof why, "%c%s", s->mark, s->why);
    unsigned o = (unsigned)snprintf(v, n, "\23%d\t%d\t%.3f\t%.3f\t%d\t%s\t%s\t%s", m->tile, m->on_button, m->cur_x, m->cur_y, m->cursor_shown,
                                    mod_roster_name(m->tile), sub, why);
    for (int t = 0; t < ROSTER_TILES && o < n; t++) {
        const RosterSum *k = &m->sum[t];
        char name[40];
        mod_ascii(mod_roster_name(t), name, sizeof name);
        const RosterTile *r = mod_roster(t);
        o += (unsigned)snprintf(v + o, n - o, "\t%c%c%d,%s,%s", k->on ? 'A' : k->off ? 'D' : 'N', k->mark ? k->mark : ' ', k->on, r ? r->portrait : "", name);
    }
}

/* The fighter the page's colors are of: the selected mod's (one of the tile's), else the tile's */
static const char *page_fighter(const ModMenu *m) {
    const ModInfo *x = m->sel >= 0 && m->sel < m->l->n ? &m->l->mods[m->sel] : NULL;
    if (x && x->fighters == 1 && x->fighter[0] && mod_roster_tile(x->fighter) == m->tile) return x->fighter;
    const RosterTile *r = mod_roster(m->tile);
    return r ? r->names[0] : "";
}

/* The page's colors ('\24'): the fighter's name '|' eight letters - M the selected mod's, o another
 * enabled mod's, s two or more others', W the selected one's and another's, . none - '|' 1 while
 * moving '|' the name its icons go by (chara_2_<it>_0X) '|' then eight: each color's icon brought by a
 * mod there ("file:<path>"), or nothing (the game's) */
static void colors_row(const ModMenu *m, char *v, unsigned n) {
    const char *f = page_fighter(m);
    const ModInfo *x = !m->on_button && m->sel >= 0 && m->sel < m->l->n ? &m->l->mods[m->sel] : NULL;
    unsigned mine = 0;
    int others[MOD_COLORS] = {0};
    if (x && x->fighters == 1 && !strcmp(x->fighter, f))
        for (int c = 0; c < MOD_COLORS; c++)
            if (x->colors & 1u << c) {
                int to = mod_color_to(x, c) + (m->moving ? m->delta : 0);
                if (to >= 0 && to < MOD_COLORS) mine |= 1u << to;
            }
    for (int k = 0; k < m->l->n; k++) {
        const ModInfo *y = &m->l->mods[k];
        if (y == x || !y->enabled || y->fighters != 1 || strcmp(y->fighter, f)) continue;
        for (int c = 0; c < MOD_COLORS; c++)
            if (y->colors & 1u << c) { int to = mod_color_to(y, c); if (to >= 0 && to < MOD_COLORS) others[to]++; }
    }
    char row[MOD_COLORS + 1];
    for (int c = 0; c < MOD_COLORS; c++)
        row[c] = mine & 1u << c ? (others[c] ? 'W' : 'M') : others[c] > 1 ? 's' : others[c] ? 'o' : '.';
    row[MOD_COLORS] = 0;
    unsigned o = (unsigned)snprintf(v, n, "\24%s|%s|%d|%s", mod_fighter_name(f), row, m->moving, pic_name(f));
    for (int c = 0; c < MOD_COLORS && o < n; c++) {   /* each color's icon: an enabled mod's own there (the selected one's first) */
        char key[600] = "";
        if (x && x->fighters == 1 && !strcmp(x->fighter, f) && !m->moving) mod_picture(m, x, 2, c, key, sizeof key);
        for (int k = 0; k < m->l->n && !key[0]; k++) {
            const ModInfo *y = &m->l->mods[k];
            if (y->enabled && y->fighters == 1 && !strcmp(y->fighter, f) && (y != x || !m->moving)) mod_picture(m, y, 2, c, key, sizeof key);
        }
        o += (unsigned)snprintf(v + o, n - o, "|%s", key);
    }
}

void mod_menu_row(const ModMenu *m, int row, const char *const keys[6], char *label, unsigned ln, char *value, unsigned vn) {
    label[0] = value[0] = 0;
    if (row == mod_menu_button_row(m)) { snprintf(label, ln, "Open Mods Folder"); return; }
    if (!on_page(m)) { if (row == 0) grid_row(m, value, vn); return; }
    if (row == detail_row(m)) { if (host_of(m)) host_detail(m, keys, value, vn); else detail(m, keys, value, vn); return; }
    if (row < colors_rows(m)) { colors_row(m, value, vn); return; }
    int item = card_item(m, row);
    if (item >= m->l->n) { host_card(m, item - m->l->n, value, vn); return; }
    if (item >= 0) { card(m, item, value, vn); return; }
    if (row != colors_rows(m)) return;
    if (m->l->off) snprintf(value, vn, "\3Mods are off (SSBU_MODS=0)");
    else if (m->list) snprintf(value, vn, "\5No mods yet - Open Mods Folder, then copy them there");
    else if (m->tile == ROSTER_OTHER) snprintf(value, vn, "\5No mods of several fighters, stages, music or menus yet");
    else snprintf(value, vn, "\5No %s mods yet - Open Mods Folder, then copy them there", mod_roster_name(m->tile));
}

/* "Link color 6" for the suspect's last file read, "" when it names no fighter's color */
static void suspect_where(const Recovery *r, char *out, unsigned n) {
    char f[64];
    int c;
    out[0] = 0;
    if (r->suspect_path[0] && mod_path_color(r->suspect_path, f, sizeof f, &c)) snprintf(out, n, "%s color %d", mod_fighter_name(f), c + 1);
}

static void recovery_text(const ModMenu *m, char *out, unsigned n) {
    const Recovery *r = &m->rec;
    static const char *const TITLE[] = {"Safe mode", "The game did not start", "The game crashed", "The game stopped", "The game froze"};
    char name[64], where[64], text[700], buttons[300];
    mod_ascii(r->suspect, name, sizeof name);
    if (strlen(name) > 28) strcpy(name + 25, "...");
    suspect_where(r, where, sizeof where);
    unsigned o = 0;
    if (!r->failed) o = (unsigned)snprintf(text, sizeof text, "Starting without mods this time is chosen. Your mods stay as they are.");
    else {
        o = (unsigned)snprintf(text, sizeof text, "%s%s with mods. ", r->failed == REC_UNCLEAN ? (r->was_running ? "Last time the game closed unexpectedly" : "Last time the game did not finish starting")
                                                                  : r->failed == REC_FROZEN ? "Last time the game froze" : "Last time the game crashed",
                                 r->was_running && r->failed != REC_UNCLEAN ? " while playing" : "");
        if (r->suspect[0] && !r->suspect_new) o += (unsigned)snprintf(text + o, sizeof text - o, "It was loading %s%s%s%s: the likely cause.", name, where[0] ? " (" : "", where, where[0] ? ")" : "");
        else if (r->suspect_new && r->nnews == 1) o += (unsigned)snprintf(text + o, sizeof text - o, "No mod file was being loaded; %s is new since the last start that worked.", name);
        else if (r->suspect_new) o += (unsigned)snprintf(text + o, sizeof text - o, "No mod file was being loaded; %d mods are new since the last start that worked.", r->nnews);
        else o += (unsigned)snprintf(text + o, sizeof text - o, "No mod file was being loaded and no mod is new: probably not the mods.");
        if (r->no_mods && o < sizeof text)
            snprintf(text + o, sizeof text - o, " It failed twice in a row with these mods: unless you choose, this start goes without them.");
    }
    unsigned b = 0;
    buttons[0] = 0;
    for (int k = 0; k < m->nrec_opts && b < sizeof buttons; k++) {
        const char *sep = k ? "|" : "";
        switch (m->rec_opts[k]) {
        case REC_OPT_DISABLE: b += (unsigned)snprintf(buttons + b, sizeof buttons - b, "%sDisable %s", sep, name); break;
        case REC_OPT_LAST_GOOD: b += (unsigned)snprintf(buttons + b, sizeof buttons - b, "%sBack to the mods that last worked", sep); break;
        case REC_OPT_NO_MODS: b += (unsigned)snprintf(buttons + b, sizeof buttons - b, "%sStart without mods this time", sep); break;
        default: b += (unsigned)snprintf(buttons + b, sizeof buttons - b, "%sIgnore", sep); break;
        }
    }
    snprintf(out, n, "%s	%s	%s", TITLE[r->failed], text, buttons);
}

void mod_menu_dialog_text(const ModMenu *m, char *out, unsigned n) {
    char name[96];
    if (m->dialog == 2) { recovery_text(m, out, n); return; }
    if (host_of(m)) {
        mod_ascii(host_of(m)->name, name, sizeof name);
        snprintf(out, n, "Delete room mods\tDelete the mods kept from %s's room? They are downloaded again if you join it and agree.\tCancel|Delete", name);
        return;
    }
    mod_ascii(m->sel >= 0 && m->sel < m->l->n ? m->l->mods[m->sel].name : "", name, sizeof name);
    snprintf(out, n, "Delete mod\tDelete \"%s\" and all its files? This cannot be undone.\tCancel|Delete", name);
}
