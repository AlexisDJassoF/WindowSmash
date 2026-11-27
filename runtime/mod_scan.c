/* The mods folder: see mod_scan.h.
 *
 * mods/windowsmash.toml, which the Mods page writes, holds where each mod's colors go:
 *
 *   [mod."Frieren over Palutena"]
 *   palutena.c03 = "c06"
 *
 * A mod without an entry there, of one fighter and one color, whose folder's name ends in -c0Y goes
 * to c0Y. Entries of mods that are not in the folder any more are left out when it is written.
 * share = false in a mod's entry keeps it out of the rooms hosted here (netplay-mod-sync). */
#define _GNU_SOURCE
#include "mod_scan.h"
#include "mod_arc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern const char *rt_data_dir;

#define SLOTS_FILE "windowsmash.toml"

const char *mod_root(char *out, size_t n) {
    const char *root = getenv("SSBU_DATA_ROOT");
    if (root) snprintf(out, n, "%s/mods", root);
    else snprintf(out, n, "%s/../../mods", rt_data_dir ? rt_data_dir : ".");
    return out;
}

uint64_t (*mod_body_hook)(const char *fighter, int color);
void *(*mod_game_file_hook)(const char *path, uint64_t *size);

/* ---- param files of colors moved ------------------------------------------------------------ */
/* paracobn: "paracobn", u32 hash table size, u32 ref table size, the hashes (u64), the ref table, the
 * values: a type byte each (1-3: 1 byte, 4-5: 2, 6-10: 4 - 9 a hash's index, 10 a string's ref offset;
 * 11 list: count, offsets from it; 12 struct: count, ref table offset of (hash index, offset from it)
 * pairs). The mod's and the game's are matched by their names, not their bytes: a mod made on another
 * version has other indices (Splatsune Inku's effect.prc: one name fewer). */
typedef struct Prc { const unsigned char *b; size_t n, hashes, refs, root; } Prc;
static uint32_t prc32(const Prc *p, size_t o) { return o + 4 <= p->n ? (uint32_t)p->b[o] | (uint32_t)p->b[o + 1] << 8 | (uint32_t)p->b[o + 2] << 16 | (uint32_t)p->b[o + 3] << 24 : 0; }
static uint64_t prc_hash(const Prc *p, uint32_t index) {
    size_t o = 16 + 8ull * index;
    return o + 8 <= p->refs ? ((uint64_t)prc32(p, o) | (uint64_t)prc32(p, o + 4) << 32) & 0xFFFFFFFFFFull : 0;
}
static int prc_open(Prc *p, const unsigned char *b, size_t n) {
    p->b = b, p->n = n;
    if (n < 16 || memcmp(b, "paracobn", 8)) return 0;
    p->hashes = 16, p->refs = 16 + (size_t)prc32(p, 8), p->root = p->refs + (size_t)prc32(p, 12);
    return p->root < n && b[p->root] == 12;
}
static size_t prc_child(const Prc *p, size_t o, uint32_t i, uint64_t *hash) {   /* a list's or a struct's child i */
    if (p->b[o] == 11) { if (hash) *hash = 0; return o + prc32(p, o + 5 + 4ull * i); }
    size_t r = p->refs + prc32(p, o + 5) + 8ull * i;
    if (hash) *hash = prc_hash(p, prc32(p, r));
    return o + prc32(p, r + 4);
}
/* the end of the value at o (its last byte's next), 0 a bad one */
static size_t prc_end(const Prc *p, size_t o, int depth) {
    if (o >= p->n || depth > 32) return 0;
    unsigned t = p->b[o];
    if (t >= 1 && t <= 3) return o + 2;
    if (t == 4 || t == 5) return o + 3;
    if (t >= 6 && t <= 10) return o + 5;
    if (t != 11 && t != 12) return 0;
    uint32_t count = prc32(p, o + 1);
    if (count > p->n) return 0;
    size_t end = t == 11 ? o + 5 + 4ull * count : o + 9;
    for (uint32_t i = 0; i < count; i++) {
        size_t e = prc_end(p, prc_child(p, o, i, NULL), depth + 1);
        if (!e) return 0;
        if (e > end) end = e;
    }
    return end <= p->n ? end : 0;
}
static size_t prc_find(const Prc *p, size_t o, uint64_t hash) {   /* a struct's child by its name, 0 none */
    uint32_t count = prc32(p, o + 1);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t h;
        size_t at = prc_child(p, o, i, &h);
        if (h == hash) return at;
    }
    return 0;
}
/* mine's value at a the same as game's at b (the numbers, the names; strings by their bytes) */
static int prc_same(const Prc *m, size_t a, const Prc *g, size_t b, int depth) {
    unsigned t = m->b[a];
    if (t != g->b[b] || depth > 32) return 0;
    if (t >= 1 && t <= 8) { size_t l = t <= 3 ? 1 : t <= 5 ? 2 : 4; return !memcmp(m->b + a + 1, g->b + b + 1, l); }
    if (t == 9) return prc_hash(m, prc32(m, a + 1)) == prc_hash(g, prc32(g, b + 1));
    if (t == 10) {
        size_t x = m->refs + prc32(m, a + 1), y = g->refs + prc32(g, b + 1);
        if (x >= m->n || y >= g->n) return 0;
        return !strncmp((const char *)m->b + x, (const char *)g->b + y, m->n - x < g->n - y ? m->n - x : g->n - y);
    }
    uint32_t count = prc32(m, a + 1);
    if (count != prc32(g, b + 1)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t h;
        size_t x = prc_child(m, a, i, &h), y = t == 11 ? prc_child(g, b, i, NULL) : prc_find(g, b, h);
        if (!y || !prc_same(m, x, g, y, depth + 1)) return 0;
    }
    return 1;
}
/* game's numbers at b written into mine's at a, where both have them (the color left: the game's) */
static void prc_assign(unsigned char *mine, const Prc *m, size_t a, const Prc *g, size_t b, int depth) {
    unsigned t = m->b[a];
    if (t != g->b[b] || depth > 32) return;
    if (t >= 1 && t <= 8) { size_t l = t <= 3 ? 1 : t <= 5 ? 2 : 4; memcpy(mine + a + 1, g->b + b + 1, l); return; }
    if (t != 11 && t != 12) return;
    uint32_t count = prc32(m, a + 1), gcount = prc32(g, b + 1);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t h;
        size_t x = prc_child(m, a, i, &h), y = t == 11 ? (i < gcount ? prc_child(g, b, i, NULL) : 0) : prc_find(g, b, h);
        if (y) prc_assign(mine, m, x, g, y, depth + 1);
    }
}
/* pass 0: the colors the mod changed in any list of the colors (owned); pass 1: all those lists' entries of
 * the owned colors moved (the ones it left as the game's color had too) */
static int prc_walk(unsigned char *mine, const Prc *m, size_t a, const Prc *g, size_t b, const signed char to[8], int depth,
                    int pass, int owned[8]) {
    unsigned t = m->b[a];
    if ((t != 11 && t != 12) || t != g->b[b] || depth > 32) return 0;
    uint32_t count = prc32(m, a + 1);
    int moved = 0;
    if (t == 11 && count == 8 && prc32(g, b + 1) == 8) {   /* a color an entry? (all of the same size) */
        size_t at[8], len[8];
        int same = 1;
        for (int i = 0; i < 8 && same; i++) {
            at[i] = prc_child(m, a, (uint32_t)i, NULL);
            size_t e = prc_end(m, at[i], depth + 1);
            len[i] = e > at[i] ? e - at[i] : 0;
            same = len[i] && len[i] == len[0] && len[i] <= 256;
        }
        if (same && !pass) {
            for (int c = 0; c < 8; c++)
                if (to[c] >= 0 && to[c] < 8 && to[c] != c && !prc_same(m, at[c], g, prc_child(g, b, (uint32_t)c, NULL), depth + 1)) owned[c] = 1;
            return 0;
        }
        if (same) {
            unsigned char was[8][256];
            int dest[8] = {0};
            for (int c = 0; c < 8; c++)
                if (owned[c]) memcpy(was[c], mine + at[c], len[c]), dest[(int)to[c]] = 1;
            for (int c = 0; c < 8; c++)
                if (owned[c] && !dest[c]) prc_assign(mine, m, at[c], g, prc_child(g, b, (uint32_t)c, NULL), depth + 1);
            for (int c = 0; c < 8; c++)
                if (owned[c]) memcpy(mine + at[(int)to[c]], was[c], len[c]), moved++;
            return moved;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        uint64_t h;
        size_t x = prc_child(m, a, i, &h), y = t == 11 ? (i < prc32(g, b + 1) ? prc_child(g, b, i, NULL) : 0) : prc_find(g, b, h);
        if (y) moved += prc_walk(mine, m, x, g, y, to, depth + 1, pass, owned);
    }
    return moved;
}
/* ---- layouts of colors moved (SARC, BFLAN) ---------------------------------------------------- */
static uint32_t le32(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const unsigned char *b) { return (uint16_t)(b[0] | b[1] << 8); }
/* SARC: file i's name and bytes; 0 none */
static int sarc_file(const unsigned char *b, size_t n, int i, const char **name, size_t *at, size_t *len) {
    if (n < 0x20 || memcmp(b, "SARC", 4) || le16(b + 6) != 0xFEFF || memcmp(b + 0x14, "SFAT", 4)) return 0;
    size_t data = le32(b + 0xC), count = le16(b + 0x1A), nodes = 0x20, sfnt = nodes + 16 * count;
    if ((size_t)i >= count || sfnt + 8 > n || memcmp(b + sfnt, "SFNT", 4)) return 0;
    const unsigned char *node = b + nodes + 16ull * (size_t)i;
    uint32_t attr = le32(node + 4), s = le32(node + 8), e = le32(node + 12);
    size_t no = sfnt + 8 + 4ull * (attr & 0xFFFF);
    if (!(attr >> 24) || no >= n || !memchr(b + no, 0, n - no) || e < s || data + e > n) return 0;
    *name = (const char *)b + no, *at = data + s, *len = e - s;
    return 1;
}
/* A BFLAN's curves (float keys: frame, value, slope): visited with their identity (pane name, tag, index,
 * target) */
typedef struct Curve { char pane[29], tag[5]; unsigned idx, target; size_t keys; unsigned nkeys; } Curve;
static int flan_curves(const unsigned char *b, size_t n, Curve *out, int max) {
    if (n < 0x14 || memcmp(b, "FLAN", 4) || le16(b + 4) != 0xFEFF) return 0;
    size_t o = le16(b + 6);   /* (the header's size; its sections after it) */
    unsigned sections = le16(b + 0x10);
    int k = 0;
    for (unsigned s = 0; s < sections && o + 8 <= n; s++) {
        size_t size = le32(b + o + 4);
        if (size < 8 || o + size > n) return k;
        if (!memcmp(b + o, "pai1", 4) && size >= 0x14) {
            unsigned entries = le16(b + o + 0xE);
            size_t table = o + le32(b + o + 0x10);
            for (unsigned e = 0; e < entries && table + 4ull * e + 4 <= n; e++) {
                size_t eo = o + le32(b + table + 4ull * e);
                if (eo + 32 > n) continue;
                unsigned ntag = b[eo + 28];
                for (unsigned t = 0; t < ntag && eo + 32 + 4ull * t + 4 <= n; t++) {
                    size_t to = eo + le32(b + eo + 32 + 4ull * t);
                    if (to + 8 > n) continue;
                    unsigned nc = b[to + 4];
                    for (unsigned c = 0; c < nc && to + 8 + 4ull * c + 4 <= n; c++) {
                        size_t co = to + le32(b + to + 8 + 4ull * c);
                        if (co + 12 > n || b[co + 2] != 2) continue;   /* (float keys only) */
                        unsigned nk = le16(b + co + 4);
                        size_t ko = co + le32(b + co + 8);
                        if (ko + 12ull * nk > n || k >= max) continue;
                        Curve *x = &out[k++];
                        memcpy(x->pane, b + eo, 28), x->pane[28] = 0;
                        memcpy(x->tag, b + to, 4), x->tag[4] = 0;
                        x->idx = b[co], x->target = b[co + 1], x->keys = ko, x->nkeys = nk;
                    }
                }
            }
        }
        o += size;
    }
    return k;
}
static float keyf(const unsigned char *b) { uint32_t u = le32(b); float f; memcpy(&f, &u, 4); return f; }
/* key i's color: a step curve's keys - frame c at the start of color c, frame c + 1 its end (the next
 * color's start at the same frame after it); -1 none */
static int key_color(const unsigned char *b, const Curve *c, unsigned i) {
    float f = keyf(b + c->keys + 12ull * i);
    int fr = (int)f;
    if ((float)fr != f || fr < 0 || fr > 8) return -1;
    int next_same = i + 1 < c->nkeys && keyf(b + c->keys + 12ull * (i + 1)) == f;
    int prev_less = i > 0 && keyf(b + c->keys + 12ull * (i - 1)) < f;
    int col = next_same && prev_less ? fr - 1 : fr;   /* (the end of the color before, a start after it) */
    return col >= 0 && col < 8 ? col : -1;
}
/* the value of color col in curve c, and whether one was found */
static int color_value(const unsigned char *b, const Curve *c, int col, float *v) {
    for (unsigned i = 0; i < c->nkeys; i++)
        if (key_color(b, c, i) == col) { *v = keyf(b + c->keys + 12ull * i + 4); return 1; }
    return 0;
}
static void set_color(unsigned char *b, const Curve *c, int col, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    for (unsigned i = 0; i < c->nkeys; i++)
        if (key_color(b, c, i) == col) {
            unsigned char *p = b + c->keys + 12ull * i + 4;
            p[0] = (unsigned char)u, p[1] = (unsigned char)(u >> 8), p[2] = (unsigned char)(u >> 16), p[3] = (unsigned char)(u >> 24);
        }
}
/* A color is the mod's in an animation when it changed any of its values there; then all of them move,
 * those it left as the game's color had too (Splatsune Inku's gauge: blue 255 as the game's c06, c00's
 * 28 - green when ink is spent) */
static int flan_move(unsigned char *mine, size_t n, const unsigned char *game, size_t gn, const signed char to[8]) {
    static Curve mc[2048], gc[2048];   /* (one at a time: the game's start-up) */
    static const Curve *pair[2048];
    int nm = flan_curves(mine, n, mc, 2048), ng = flan_curves(game, gn, gc, 2048), moved = 0;
    int owned[8] = {0}, dest[8] = {0};
    for (int i = 0; i < nm; i++) {   /* (the curves of the colors, each with the game's; the colors the mod changed) */
        const Curve *m = &mc[i];
        pair[i] = NULL;
        for (int j = 0; j < ng && !pair[i]; j++)
            if (!strcmp(gc[j].pane, m->pane) && !strcmp(gc[j].tag, m->tag) && gc[j].idx == m->idx && gc[j].target == m->target) pair[i] = &gc[j];
        int colors = 0;   /* (a key for each color - the last one's may hold to the end, c07 too) */
        for (int c = 0; c < 8; c++) { float v; colors += color_value(mine, m, c, &v); }
        if (!pair[i] || m->nkeys < 8 || colors < 7) { pair[i] = NULL; continue; }
        for (int c = 0; c < 8; c++) {
            float a, b;
            if (to[c] >= 0 && to[c] < 8 && to[c] != c && color_value(mine, m, c, &a) && color_value(game, pair[i], c, &b) && a != b) owned[c] = 1;
        }
    }
    for (int c = 0; c < 8; c++) if (owned[c]) dest[(int)to[c]] = 1;
    for (int i = 0; i < nm; i++) {
        const Curve *m = &mc[i];
        if (!pair[i]) continue;
        float was[8], theirs[8];
        int go[8] = {0};
        for (int c = 0; c < 8; c++) {
            float there;
            go[c] = owned[c] && color_value(mine, m, c, &was[c]) && color_value(game, pair[i], c, &theirs[c]) &&
                    color_value(mine, m, to[c], &there);   /* (where it goes has its own key) */
        }
        for (int c = 0; c < 8; c++) if (go[c] && !dest[c]) set_color(mine, m, c, theirs[c]);
        for (int c = 0; c < 8; c++) if (go[c]) set_color(mine, m, to[c], was[c]), moved++;
    }
    return moved;
}
int mod_layout_move(unsigned char *mine, size_t n, const unsigned char *game, size_t gn, const signed char to[8]) {
    int moved = 0;
    const char *name, *gname;
    size_t at, len, gat, glen;
    for (int i = 0; sarc_file(mine, n, i, &name, &at, &len); i++) {
        size_t l = strlen(name);
        if (l < 6 || strcmp(name + l - 6, ".bflan")) continue;
        for (int j = 0; sarc_file(game, gn, j, &gname, &gat, &glen); j++)
            if (!strcmp(gname, name)) { moved += flan_move(mine + at, len, game + gat, glen, to); break; }
    }
    return moved;
}

int mod_prc_move(unsigned char *mine, size_t n, const unsigned char *game, size_t gn, const signed char to[8]) {
    Prc m, g;
    if (!prc_open(&m, mine, n) || !prc_open(&g, game, gn)) return 0;
    if (prc_end(&m, m.root, 0) != n || prc_end(&g, g.root, 0) != gn) return 0;   /* (all of each in range) */
    int owned[8] = {0}, any = 0;
    prc_walk(mine, &m, m.root, &g, g.root, to, 0, 0, owned);
    for (int c = 0; c < 8; c++) any |= owned[c];
    return any ? prc_walk(mine, &m, m.root, &g, g.root, to, 0, 1, owned) : 0;
}

/* ---- fighters ------------------------------------------------------------------------------- */
static const char *const FIGHTERS[][2] = {
    {"mario", "Mario"}, {"donkey", "Donkey Kong"}, {"link", "Link"}, {"samus", "Samus"}, {"samusd", "Dark Samus"},
    {"yoshi", "Yoshi"}, {"kirby", "Kirby"}, {"fox", "Fox"}, {"pikachu", "Pikachu"}, {"luigi", "Luigi"}, {"ness", "Ness"},
    {"captain", "Captain Falcon"}, {"purin", "Jigglypuff"}, {"peach", "Peach"}, {"daisy", "Daisy"}, {"koopa", "Bowser"},
    {"ice_climber", "Ice Climbers"}, {"popo", "Ice Climbers"}, {"nana", "Ice Climbers"}, {"sheik", "Sheik"}, {"zelda", "Zelda"},
    {"mariod", "Dr. Mario"}, {"pichu", "Pichu"}, {"falco", "Falco"}, {"marth", "Marth"}, {"lucina", "Lucina"},
    {"younglink", "Young Link"}, {"ganon", "Ganondorf"}, {"mewtwo", "Mewtwo"}, {"roy", "Roy"}, {"chrom", "Chrom"},
    {"gamewatch", "Mr. Game & Watch"}, {"metaknight", "Meta Knight"}, {"pit", "Pit"}, {"pitb", "Dark Pit"},
    {"szerosuit", "Zero Suit Samus"}, {"wario", "Wario"}, {"snake", "Snake"}, {"ike", "Ike"}, {"ptrainer", "Pokemon Trainer"},
    {"pzenigame", "Squirtle"}, {"pfushigisou", "Ivysaur"}, {"plizardon", "Charizard"}, {"diddy", "Diddy Kong"},
    {"lucas", "Lucas"}, {"sonic", "Sonic"}, {"dedede", "King Dedede"}, {"pikmin", "Olimar"}, {"lucario", "Lucario"},
    {"robot", "R.O.B."}, {"toonlink", "Toon Link"}, {"wolf", "Wolf"}, {"murabito", "Villager"}, {"rockman", "Mega Man"},
    {"wiifit", "Wii Fit Trainer"}, {"rosetta", "Rosalina & Luma"}, {"littlemac", "Little Mac"}, {"gekkouga", "Greninja"},
    {"palutena", "Palutena"}, {"pacman", "Pac-Man"}, {"reflet", "Robin"}, {"shulk", "Shulk"}, {"koopajr", "Bowser Jr."},
    {"duckhunt", "Duck Hunt"}, {"ryu", "Ryu"}, {"ken", "Ken"}, {"cloud", "Cloud"}, {"kamui", "Corrin"},
    {"bayonetta", "Bayonetta"}, {"inkling", "Inkling"}, {"ridley", "Ridley"}, {"simon", "Simon"}, {"richter", "Richter"},
    {"krool", "King K. Rool"}, {"shizue", "Isabelle"}, {"gaogaen", "Incineroar"}, {"packun", "Piranha Plant"},
    {"jack", "Joker"}, {"brave", "Hero"}, {"buddy", "Banjo & Kazooie"}, {"dolly", "Terry"}, {"master", "Byleth"},
    {"tantan", "Min Min"}, {"pickel", "Steve"}, {"edge", "Sephiroth"}, {"eflame", "Pyra"}, {"elight", "Mythra"},
    {"element", "Pyra & Mythra"}, {"demon", "Kazuya"}, {"trail", "Sora"}, {"miifighter", "Mii Brawler"},
    {"miiswordsman", "Mii Swordfighter"}, {"miigunner", "Mii Gunner"},
};

const char *mod_fighter_name(const char *internal) {
    for (size_t i = 0; i < sizeof FIGHTERS / sizeof FIGHTERS[0]; i++)
        if (!strcmp(FIGHTERS[i][0], internal)) return FIGHTERS[i][1];
    return internal;
}

static int fighter_known(const char *f) {
    for (size_t i = 0; i < sizeof FIGHTERS / sizeof FIGHTERS[0]; i++)
        if (!strcmp(FIGHTERS[i][0], f)) return 1;
    return 0;
}

int mod_fighter_varied(const char *f) {
    static const char *const V[] = {"pickel", "brave", "master", "ptrainer", "pzenigame", "pfushigisou", "plizardon", "pikmin",
                                    "koopajr", "murabito", "inkling", "reflet", "kamui", "wiifit", "eflame", "elight", "element"};
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++)
        if (!strcmp(V[i], f)) return 1;
    return 0;
}

/* ---- paths ---------------------------------------------------------------------------------- */
static int is_color(const char *s, size_t len, int *c) {   /* "c03" (and c100...) */
    if (len < 3 || s[0] != 'c') return 0;
    int v = 0;
    for (size_t i = 1; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
        v = v * 10 + (s[i] - '0');
    }
    *c = v;
    return 1;
}

/* ui/replace/chara/chara_3/chara_3_palutena_03.bntx (or replace_patch): where the fighter's name and the
 * color are in the file's name; 1 if the path is one */
static int portrait(const char *path, const char **f, size_t *flen, const char **num) {
    const char *p;
    if (!strncmp(path, "ui/replace/chara/", 17)) p = path + 17;
    else if (!strncmp(path, "ui/replace_patch/chara/", 23)) p = path + 23;
    else return 0;
    const char *slash = strchr(p, '/');
    if (!slash || strncmp(p, "chara_", 6)) return 0;
    const char *file = slash + 1;
    if (strchr(file, '/') || strncmp(file, "chara_", 6)) return 0;
    const char *under = strchr(file + 6, '_'), *dot = strrchr(file, '.');
    if (!under || !dot || strcmp(dot, ".bntx")) return 0;
    const char *last = dot - 3;   /* "_03" */
    if (last <= under || last[0] != '_' || !isdigit((unsigned char)last[1]) || !isdigit((unsigned char)last[2])) return 0;
    *f = under + 1;
    *flen = (size_t)(last - *f);
    *num = last + 1;
    return 1;
}

/* The longest fighter's internal name s starts with, followed by '_', '/' or '.' ("ice_climber_x":
 * "ice_climber"); its length, 0 none */
static size_t fighter_prefix(const char *s) {
    size_t best = 0;
    for (size_t i = 0; i < sizeof FIGHTERS / sizeof FIGHTERS[0]; i++) {
        size_t l = strlen(FIGHTERS[i][0]);
        if (l > best && !strncmp(s, FIGHTERS[i][0], l) && (s[l] == '_' || s[l] == '/' || s[l] == '.')) best = l;
    }
    return best;
}

/* "cXX" (c00, c103...) at s, ended by '/', '_', '.' or the end: its length, 0 if not one */
static size_t color_token(const char *s, int *c) {
    size_t l = 1;
    if (s[0] != 'c') return 0;
    while (isdigit((unsigned char)s[l])) l++;
    if (l < 3 || (s[l] && s[l] != '/' && s[l] != '_' && s[l] != '.')) return 0;
    return is_color(s, l, c) ? l : 0;
}

/* The first directory of p (up to the file's name) that is a color: where it is, 0 none */
static const char *color_dir(const char *p, int *c) {
    for (const char *end; (end = strchr(p, '/')); p = end + 1)
        if (is_color(p, (size_t)(end - p), c)) return p;
    return NULL;
}

/* Where a path says whose color it is: the fighter, and the color's place in the path (the "cXX", or
 * a portrait's two digits: *digits 1). The forms (all as ARCropolis' reslotters move them):
 *   fighter/<f>/.../cXX/...                    the fighter's own (model, motion, camera...)
 *   fighter/kirby/model/copy_<f>_<part>/cXX/   Kirby's copy of <f>: <f>'s color
 *   camera/fighter/<f>/cXX/...                 victory cameras
 *   effect/fighter/<f>/trail_cXX/..., effect/fighter/<f>/ef_<f>_cXX.eff
 *   sound/bank/fighter_voice/vc_<f>[_cheer]_cXX.*, sound/bank/fighter/se_<f>_cXX.*
 *   ui/replace[_patch]/chara/chara_N/chara_N_<f>_XX.bntx */
static const char *path_color(const char *path, char *fighter, size_t n, int *color, int *digits) {
    const char *f = NULL, *at = NULL, *num;
    size_t fl = 0;
    *digits = 0;
    if (!strncmp(path, "fighter/kirby/model/copy_", 25) && (fl = fighter_prefix(path + 25))) {
        f = path + 25;
        at = color_dir(strchr(f, '/') ? strchr(f, '/') + 1 : f + fl, color);
    } else if (!strncmp(path, "fighter/", 8)) {
        const char *slash = strchr(path + 8, '/');
        if (!slash) return NULL;
        f = path + 8, fl = (size_t)(slash - f);
        at = color_dir(slash + 1, color);
    } else if (!strncmp(path, "camera/fighter/", 15)) {
        const char *slash = strchr(path + 15, '/');
        if (!slash) return NULL;
        f = path + 15, fl = (size_t)(slash - f);
        at = color_dir(slash + 1, color);
    } else if (!strncmp(path, "effect/fighter/", 15)) {
        const char *slash = strchr(path + 15, '/'), *rest;
        if (!slash) return NULL;
        f = path + 15, fl = (size_t)(slash - f), rest = slash + 1;
        if (!strncmp(rest, "trail_", 6) && color_token(rest + 6, color)) at = rest + 6;
        else if (!strncmp(rest, "ef_", 3) && !strncmp(rest + 3, f, fl) && rest[3 + fl] == '_' && color_token(rest + 4 + fl, color))
            at = rest + 4 + fl;
    } else if (!strncmp(path, "sound/bank/fighter_voice/vc_", 28) || !strncmp(path, "sound/bank/fighter/se_", 22)) {
        f = path + (path[18] == '_' ? 28 : 22);
        if ((fl = fighter_prefix(f))) {
            const char *t = f + fl;
            if (!strncmp(t, "_cheer", 6)) t += 6;
            if (*t == '_' && color_token(t + 1, color)) at = t + 1;
        }
    } else if (portrait(path, &f, &fl, &num)) {
        *color = (num[0] - '0') * 10 + (num[1] - '0');
        *digits = 1;
        at = num;
    }
    if (!at || !fl) return NULL;
    snprintf(fighter, n, "%.*s", (int)fl, f);
    return at;
}

int mod_path_color(const char *path, char *fighter, size_t n, int *color) {
    int digits;
    return path_color(path, fighter, n, color, &digits) != NULL;
}

int mod_color_to(const ModInfo *m, int c) { return c >= 0 && c < MOD_COLORS && m->to[c] >= 0 ? m->to[c] : c; }

void mod_dest_path(const ModInfo *m, const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char f[64];
    int c, digits;
    const char *at = path_color(path, f, sizeof f, &c, &digits);
    if (!at || strcmp(f, m->fighter) || c >= MOD_COLORS || mod_color_to(m, c) == c || (size_t)(at - path) + 3 > strlen(out)) return;
    int to = mod_color_to(m, c);
    char *o = out + (at - path) + (digits ? 0 : 1);   /* (the two digits: "_03" of a portrait, "c03") */
    if (!digits && !(isdigit((unsigned char)o[0]) && isdigit((unsigned char)o[1]) && !isdigit((unsigned char)o[2]))) return;
    o[0] = (char)('0' + to / 10), o[1] = (char)('0' + to % 10);
}

/* ---- info.toml ------------------------------------------------------------------------------ */
static void trim(char *s) {
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
    size_t i = 0;
    while (isspace((unsigned char)s[i])) i++;
    memmove(s, s + i, l - i + 1);
}

/* display_name, authors (or author), version, description: "...", '...' or """...""" */
static void read_info(ModInfo *m, const char *file) {
    FILE *fp = fopen(file, "rb");
    if (!fp) return;
    char line[1024], key[64];
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == '[') continue;
        *eq = 0;
        snprintf(key, sizeof key, "%s", line);
        trim(key);
        char *v = eq + 1;
        while (isspace((unsigned char)*v)) v++;
        char val[512] = "";
        if (!strncmp(v, "\"\"\"", 3) || !strncmp(v, "'''", 3)) {   /* across lines, to the closing quotes */
            char q[4];
            memcpy(q, v, 3);
            q[3] = 0;
            v += 3;
            size_t o = 0;
            for (;;) {
                char *end = strstr(v, q);
                size_t take = end ? (size_t)(end - v) : strlen(v);
                for (size_t i = 0; i < take && o + 1 < sizeof val; i++) val[o++] = v[i] == '\n' || v[i] == '\r' ? ' ' : v[i];
                if (end || !fgets(line, sizeof line, fp)) break;
                v = line;
            }
            val[o] = 0;
        } else if (*v == '"' || *v == '\'') {
            char q = *v++;
            size_t o = 0;
            for (; *v && *v != q && o + 1 < sizeof val; v++) {
                if (*v == '\\' && q == '"' && v[1]) { v++; val[o++] = *v == 'n' ? ' ' : *v; }
                else val[o++] = *v;
            }
            val[o] = 0;
        } else continue;
        trim(val);
        if (!strcmp(key, "display_name") && *val) snprintf(m->name, sizeof m->name, "%s", val);
        else if (!strcmp(key, "authors") || !strcmp(key, "author")) snprintf(m->authors, sizeof m->authors, "%s", val);
        else if (!strcmp(key, "version")) snprintf(m->version, sizeof m->version, "%s", val);
        else if (!strcmp(key, "description")) snprintf(m->description, sizeof m->description, "%s", val);
        else if (!strcmp(key, "url") && !strncmp(val, "https://", 8)) snprintf(m->url, sizeof m->url, "%s", val);
        else if (!strcmp(key, "url_sha256") && strlen(val) == 64) {
            int hex = 1;
            for (const char *c = val; *c; c++) hex &= isxdigit((unsigned char)*c) != 0;
            if (hex) for (int i = 0; i < 65; i++) m->url_sha256[i] = (char)tolower((unsigned char)val[i]);
        }
    }
    if (!m->url_sha256[0]) m->url[0] = 0;   /* (an address without its hash is not used: nothing to check the download by) */
    fclose(fp);
}

unsigned mod_path_kind(const char *p) {
    if (!strncmp(p, "fighter/", 8)) return strstr(p, "/model/") ? MK_MODEL : MK_OTHER;
    if (!strncmp(p, "ui/replace/chara/", 17) || !strncmp(p, "ui/replace_patch/chara/", 23)) return MK_PORTRAIT;
    if (!strncmp(p, "sound/bank/fighter_voice/", 25)) return MK_VOICE;
    if (!strncmp(p, "effect/fighter/", 15)) return MK_EFFECT;
    return MK_OTHER;
}

#ifndef _WIN32
/* ---- reading the folder --------------------------------------------------------------------- */
static void add_path(ModInfo *m, const char *rel) {
    if (m->npaths % 64 == 0) {
        char **p = realloc(m->paths, sizeof *p * (size_t)(m->npaths + 64));
        if (!p) return;
        m->paths = p;
        p = realloc(m->real, sizeof *p * (size_t)(m->npaths + 64));
        if (!p) return;
        m->real = p;
    }
    char *s = strdup(rel), *r = strdup(rel);
    if (!s || !r) { free(s); free(r); return; }
    for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)*c);
    m->real[m->npaths] = r;
    m->paths[m->npaths++] = s;
}

static void walk(ModInfo *m, const char *dir, const char *rel, int depth) {
    if (depth > 24) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;   /* (., .., and hidden files: .DS_Store, macOS's ._ copies - never the game's) */
        char full[4096], sub[2048];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(m, full, sub, depth + 1);
        else if (S_ISREG(st.st_mode)) {
            if (!*rel) {   /* at the top: the mod's own files (info.toml, preview.png, config.json...) */
                if (!strcasecmp(e->d_name, "config.json")) m->has_config = 1;
                if (!strcasecmp(e->d_name, "preview.png")) snprintf(m->preview, sizeof m->preview, "%s", e->d_name);
                continue;
            }
            add_path(m, sub);
        }
    }
    closedir(d);
}

int mod_path_fighter(const char *path, char *fighter, size_t n) {
    int c;
    if (mod_path_color(path, fighter, n, &c)) return 1;
    const char *slash;   /* (fighter/<f>/... without a color: that fighter still) */
    if (strncmp(path, "fighter/", 8) || !(slash = strchr(path + 8, '/'))) return 0;
    snprintf(fighter, n, "%.*s", (int)(slash - path - 8), path + 8);
    return 1;
}

/* What the files are: the fighters, colors and kinds */
static void classify(ModInfo *m) {
    int nf = 0, count[16] = {0};
    char seen[16][32];
    for (int i = 0; i < m->npaths; i++) {
        const char *p = m->paths[i];
        if (!strncmp(p, "stream;", 7) || !strncmp(p, "stream:", 7)) { m->has_stream = 1; continue; }
        m->files++;
        char f[64];
        unsigned kind = mod_path_kind(p);
        m->kinds |= kind;
        if (kind == MK_OTHER) m->gameplay = 1;
        if (!mod_path_fighter(p, f, sizeof f)) continue;
        /* (fighter/common/ - shared params, effects -, Giga Bowser's koopag...: no fighter's colors, so a
         * skin with one of those is still its fighter's alone, and moves) */
        if (!fighter_known(f)) continue;
        int known = -1;
        for (int k = 0; k < nf && known < 0; k++) if (!strcmp(seen[k], f)) known = k;
        if (known < 0 && nf < 16) snprintf(seen[known = nf++], sizeof seen[0], "%s", f);
        if (known >= 0) count[known]++;
    }
    /* the fighter: the one most of its files are of (the first in alphabetical order of those) */
    int best = -1;
    for (int k = 0; k < nf; k++)
        if (best < 0 || count[k] > count[best] || (count[k] == count[best] && strcmp(seen[k], seen[best]) < 0)) best = k;
    snprintf(m->fighter, sizeof m->fighter, "%s", best >= 0 ? seen[best] : "");
    m->fighters = nf;
    for (int i = 0; i < m->npaths; i++) {
        char f[64];
        int c;
        if (!mod_path_color(m->paths[i], f, sizeof f, &c) || strcmp(f, m->fighter)) continue;
        if (c < MOD_COLORS) m->colors |= 1u << c;
        else m->high_colors = 1;
    }
}

/* windowsmash.toml's entries for the mods in the list */
static void read_slots(ModList *l) {
    char file[1024];
    snprintf(file, sizeof file, "%s/" SLOTS_FILE, l->root);
    FILE *fp = fopen(file, "rb");
    if (!fp) return;
    char line[1024];
    int cur = -1;
    while (fgets(line, sizeof line, fp)) {
        trim(line);
        if (!strncmp(line, "[mod.\"", 6)) {   /* [mod."name"] */
            char key[256];
            size_t o = 0;
            for (const char *p = line + 6; *p && *p != '"' && o + 1 < sizeof key; p++) {
                if (*p == '\\' && p[1]) p++;
                key[o++] = *p;
            }
            key[o] = 0;
            cur = mod_find(l, key);
            continue;
        }
        if (line[0] == '[') { cur = -1; continue; }
        if (cur < 0 || line[0] == '#') continue;
        if (!strncmp(line, "share", 5)) {   /* share = false */
            const char *v = line + 5;
            while (*v == ' ' || *v == '=') v++;
            l->mods[cur].no_share = !strncmp(v, "false", 5);
            continue;
        }
        /* palutena.c03 = "c06" */
        char f[64], from[16], to[16];
        if (sscanf(line, "%63[^.].%15[^ =] = \"%15[^\"]\"", f, from, to) != 3) continue;
        int a, b;
        ModInfo *m = &l->mods[cur];
        if (!is_color(from, strlen(from), &a) || !is_color(to, strlen(to), &b) || a >= MOD_COLORS) continue;
        if (b >= MOD_COLORS) { fprintf(stderr, "[mods] %s: %s.%s = \"%s\": only c00-c07; left where it is\n", m->key, f, from, to); continue; }
        if (strcmp(f, m->fighter)) continue;
        if (m->from_name) {   /* (the file wins over the name: its colors, not share = false alone) */
            memset(m->to, -1, sizeof m->to);
            m->from_name = 0;
        }
        m->to[a] = (signed char)b;
    }
    fclose(fp);
}

/* Which files of each enabled mod a mod before it already replaces */
static void find_conflicts(ModList *l) {
    size_t cap = 1;
    for (int i = 0; i < l->n; i++) cap += (size_t)l->mods[i].npaths;
    size_t size = 16;
    while (size < cap * 2) size <<= 1;
    struct Slot { const char *path; int mod; } *table = calloc(size, sizeof *table);
    char **owned = calloc(cap, sizeof *owned);
    if (!table || !owned) { free(table); free(owned); return; }
    size_t nowned = 0;
    for (int i = 0; i < l->n; i++) {
        ModInfo *m = &l->mods[i];
        m->conflicts = 0;
        m->conflict_with[0] = 0;
        if (!m->enabled) continue;
        for (int k = 0; k < m->npaths; k++) {
            char dest[2048];
            mod_dest_path(m, m->paths[k], dest, sizeof dest);
            unsigned long h = 5381;
            for (const char *c = dest; *c; c++) h = h * 33 + (unsigned char)*c;
            size_t at = h & (size - 1);
            while (table[at].path && strcmp(table[at].path, dest)) at = (at + 1) & (size - 1);
            if (table[at].path) {
                ModInfo *o = &l->mods[table[at].mod];
                if (!m->conflicts++) snprintf(m->conflict_with, sizeof m->conflict_with, "%s", o->name);
                if (!o->conflicts++) snprintf(o->conflict_with, sizeof o->conflict_with, "%s", m->name);
                continue;
            }
            char *copy = strdup(dest);
            if (!copy) continue;
            owned[nowned++] = copy;
            table[at].path = copy, table[at].mod = i;
        }
    }
    for (size_t i = 0; i < nowned; i++) free(owned[i]);
    free(owned);
    free(table);
}

static int by_key(const void *a, const void *b) { return strcasecmp(((const ModInfo *)a)->key, ((const ModInfo *)b)->key); }

int mod_scan(ModList *l, const char *root) {
    mod_list_free(l);
    snprintf(l->root, sizeof l->root, "%s", root);
    const char *v = getenv("SSBU_MODS");
    l->off = v && !strcmp(v, "0");
    if (l->off) return 0;
    mkdir(root, 0755);
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && l->n < MOD_MAX) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".netplay")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(full, &st) || !S_ISDIR(st.st_mode)) continue;
        ModInfo *m = &l->mods[l->n++];
        memset(m, 0, sizeof *m);
        memset(m->to, -1, sizeof m->to);
        snprintf(m->dir, sizeof m->dir, "%s", e->d_name);
        snprintf(m->key, sizeof m->key, "%s", e->d_name + (e->d_name[0] == '.'));
        snprintf(m->name, sizeof m->name, "%s", m->key);
        m->enabled = e->d_name[0] != '.';
        char info[1100];
        snprintf(info, sizeof info, "%s/info.toml", full);
        read_info(m, info);
        walk(m, full, "", 0);
        classify(m);
        /* a name ending in -c0Y: one fighter, one color, moved there */
        size_t kl = strlen(m->key);
        int c;
        if (kl > 4 && m->key[kl - 4] == '-' && is_color(m->key + kl - 3, 3, &c) && m->fighters == 1 && m->colors &&
            !(m->colors & (m->colors - 1))) {
            if (c < MOD_COLORS) {
                int from = __builtin_ctz(m->colors);
                if (from != c) { m->to[from] = (signed char)c; m->from_name = 1; }
            } else fprintf(stderr, "[mods] %s: only c00-c07; left where it is\n", m->key);
        }
    }
    closedir(d);
    qsort(l->mods, (size_t)l->n, sizeof l->mods[0], by_key);
    read_slots(l);
    find_conflicts(l);
    return 1;
}

void mod_list_free(ModList *l) {
    for (int i = 0; i < l->n; i++) {
        for (int k = 0; k < l->mods[i].npaths; k++) free(l->mods[i].paths[k]), free(l->mods[i].real[k]);
        free(l->mods[i].paths);
        free(l->mods[i].real);
        l->mods[i].paths = NULL;
        l->mods[i].real = NULL;
        l->mods[i].npaths = 0;
    }
    l->n = 0;
}

int mod_find(const ModList *l, const char *key) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->mods[i].key, key)) return i;
    return -1;
}

/* ---- config.json ---------------------------------------------------------------------------- */
/* ARCropolis' config.json: of it, "new-dir-files" - {"<data.arc directory>": ["<new file's path>", ...]}:
 * files that are not in data.arc, added to the game as children of that directory (mod-support 1.4c).
 * The other keys (new-shared-files, new-dir-infos...) are not read. A small reader of JSON: strings,
 * objects, arrays and the rest skipped. */
typedef struct { const char *p, *end; } Json;

static void json_ws(Json *j) { while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++; }

/* A string into out (lower case, '\\' as '/' for paths): 1, 0 not a string */
static int json_str(Json *j, char *out, size_t n) {
    json_ws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    size_t w = 0;
    for (j->p++; j->p < j->end && *j->p != '"'; j->p++) {
        char c = *j->p;
        if (c == '\\' && j->p + 1 < j->end) {
            c = *++j->p;
            if (c == 'u') { j->p += j->p + 4 < j->end ? 4 : 0; c = '?'; }   /* (not in paths) */
            else if (c == '\\') c = '/';
            else if (c == 'n' || c == 't' || c == 'r') c = ' ';
        }
        if (w + 1 < n) out[w++] = (char)tolower((unsigned char)c);
    }
    if (j->p < j->end) j->p++;
    out[w] = 0;
    return 1;
}

static void json_skip(Json *j, int depth) {   /* any value */
    char tmp[8];
    json_ws(j);
    if (j->p >= j->end || depth > 32) { j->p = j->end; return; }
    if (*j->p == '"') { json_str(j, tmp, sizeof tmp); return; }
    if (*j->p == '{' || *j->p == '[') {
        char close = *j->p == '{' ? '}' : ']';
        j->p++;
        for (;;) {
            json_ws(j);
            if (j->p >= j->end) return;
            if (*j->p == close) { j->p++; return; }
            if (*j->p == ',' || *j->p == ':') { j->p++; continue; }
            json_skip(j, depth + 1);
        }
    }
    while (j->p < j->end && !strchr(",}]", *j->p) && !isspace((unsigned char)*j->p)) j->p++;
}

typedef struct { char *path, *dir; } NewFile;

/* config.json's new-dir-files of mod m: *out malloc'd pairs (path, directory), their count */
static int read_new_files(const ModList *l, const ModInfo *m, NewFile **out) {
    *out = NULL;
    if (!m->has_config) return 0;
    char dirname[4096], file[4352];
    snprintf(dirname, sizeof dirname, "%s/%s", l->root, m->dir);
    file[0] = 0;
    DIR *d = opendir(dirname);
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strcasecmp(e->d_name, "config.json")) snprintf(file, sizeof file, "%s/%s", dirname, e->d_name);
    if (d) closedir(d);
    FILE *f = *file ? fopen(file, "rb") : NULL;
    if (!f) return 0;
    char *text = NULL;
    long len = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (len > 0 && len < (16 << 20) && !fseek(f, 0, SEEK_SET) && (text = malloc((size_t)len))) {
        if (fread(text, 1, (size_t)len, f) != (size_t)len) { free(text); text = NULL; }
    }
    fclose(f);
    if (!text) return 0;
    Json j = {text, text + len};
    int n = 0, cap = 0, bad = 0;
    NewFile *v = NULL;
    char key[1024], dir[1024], path[2048];
    json_ws(&j);
    if (j.p < j.end && *j.p == '{') {
        j.p++;
        while (!bad) {
            json_ws(&j);
            if (j.p >= j.end || *j.p == '}') break;
            if (*j.p == ',') { j.p++; continue; }
            if (!json_str(&j, key, sizeof key)) { bad = 1; break; }
            json_ws(&j);
            if (j.p < j.end && *j.p == ':') j.p++;
            json_ws(&j);
            if (strcmp(key, "new-dir-files") || j.p >= j.end || *j.p != '{') { json_skip(&j, 0); continue; }
            j.p++;
            for (;;) {   /* "dir": [paths] */
                json_ws(&j);
                if (j.p >= j.end || *j.p == '}') { if (j.p < j.end) j.p++; break; }
                if (*j.p == ',') { j.p++; continue; }
                if (!json_str(&j, dir, sizeof dir)) { bad = 1; break; }
                json_ws(&j);
                if (j.p < j.end && *j.p == ':') j.p++;
                json_ws(&j);
                if (j.p >= j.end || *j.p != '[') { json_skip(&j, 0); continue; }
                j.p++;
                for (;;) {
                    json_ws(&j);
                    if (j.p >= j.end || *j.p == ']') { if (j.p < j.end) j.p++; break; }
                    if (*j.p == ',') { j.p++; continue; }
                    if (!json_str(&j, path, sizeof path)) { json_skip(&j, 0); continue; }
                    size_t dl = strlen(dir);
                    while (dl && dir[dl - 1] == '/') dir[--dl] = 0;
                    if (n == cap) {
                        NewFile *q = realloc(v, sizeof *v * (size_t)(cap = cap ? cap * 2 : 32));
                        if (!q) { bad = 1; break; }
                        v = q;
                    }
                    v[n].path = strdup(path), v[n].dir = strdup(dir);
                    if (!v[n].path || !v[n].dir) { free(v[n].path); free(v[n].dir); bad = 1; break; }
                    n++;
                }
            }
        }
    }
    if (bad) fprintf(stderr, "[mods] %s: config.json could not be read whole; %d new files from it\n", m->key, n);
    free(text);
    *out = v;
    return n;
}

/* ---- what the game loads -------------------------------------------------------------------- */
static int by_dest_name(const void *a, const void *b) { return strcmp(((const ModArcFile *)a)->dest, ((const ModArcFile *)b)->dest); }

/* A fighter's file a color move took elsewhere ("fighter/inkling/model/body/c05/def_inkling_c06_001_col.nutexb"):
 * its color's data.arc directory ("fighter/inkling/c05"), into dir; else "" left. Some fighters' files are
 * named by their color (Inkling's textures), and the color it goes to has none of those names: there
 * they are new files of that directory - what a reslotter's config.json says (mod_arc only adds a
 * file of a directory when data.arc does not have it already) */
static void moved_dir(const char *dest, char *dir, size_t n) {
    if (strncmp(dest, "fighter/", 8)) return;
    const char *name = dest + 8, *slash = strchr(name, '/');
    if (!slash) return;
    for (const char *c = slash; (c = strstr(c, "/c")) != NULL; c++)
        if (isdigit((unsigned char)c[2]) && isdigit((unsigned char)c[3]) && c[4] == '/') {
            snprintf(dir, n, "fighter/%.*s/c%c%c", (int)(slash - name), name, c[2], c[3]);
            return;
        }
}

/* A move of mod m that puts a color where the game's fighter has another body (a girl's Inkling on a
 * boy's color: the fight's loading hangs): the first such color, else -1 */
static int other_body(const ModInfo *m) {
    if (!mod_body_hook || !m->fighter[0] || m->fighters != 1) return -1;
    for (int c = 0; c < MOD_COLORS; c++) {
        int to = mod_color_to(m, c);
        if (!(m->colors & 1u << c) || to == c) continue;
        uint64_t a = mod_body_hook(m->fighter, c), b = mod_body_hook(m->fighter, to);
        if (a && b && a != b) return c;
    }
    return -1;
}

static int mkdirs(char *path) {   /* path's folders (its last part a file) */
    for (char *s = strchr(path + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        int ok = !mkdir(path, 0755) || errno == EEXIST;
        *s = '/';
        if (!ok) return 0;
    }
    return 1;
}

/* A param file of a color-less path (fighter/common/param/effect.prc) of moved mod m: its entries of the
 * colors moved taken along (mod_prc_move), written to cache/mods/moved/<mod>/<path> - src then that
 * one. 0 nothing to do (src left). */
/* bytes written to file (its folders made; the same bytes there already: left, its date too) - 1 done */
static int write_same(const char *file, const unsigned char *b, size_t n) {
    FILE *had = fopen(file, "rb");
    if (had) {
        unsigned char *old = malloc(n + 1);
        int same = old && fread(old, 1, n + 1, had) == n && !memcmp(old, b, n);
        free(old);
        fclose(had);
        if (same) return 1;
    }
    char path[4200], tmp[4300];
    snprintf(path, sizeof path, "%s", file);
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *w = mkdirs(path) ? fopen(tmp, "wb") : NULL;
    int ok = w && fwrite(b, 1, n, w) == n;
    if (w) ok &= fclose(w) == 0;
    if (ok && !rename(tmp, file)) return 1;
    unlink(tmp);
    return 0;
}

/* cache/mods/moved/<mod>/<path>: into out */
static void moved_file(const ModList *l, const ModInfo *m, const char *path, char *out, size_t n) {
    size_t rl = strlen(l->root);
    snprintf(out, n, "%.*s/cache/mods/moved/%s/%s", (int)(rl >= 5 && !strcmp(l->root + rl - 5, "/mods") ? rl - 5 : rl), l->root, m->key, path);
}

static int moved_prc(const ModList *l, const ModInfo *m, const char *path, char *src, size_t n) {
    size_t pl = strlen(path);
    int layout = pl >= 11 && !strcmp(path + pl - 11, "/layout.arc");
    if (!mod_game_file_hook || ((pl < 5 || strcmp(path + pl - 4, ".prc")) && !layout)) return 0;
    char f[64];
    int c;
    if (mod_path_color(path, f, sizeof f, &c)) return 0;   /* (a color's own: its path moves instead) */
    signed char to[8];
    int any = 0;
    for (int k = 0; k < 8; k++) to[k] = (signed char)(m->colors & 1u << k ? mod_color_to(m, k) : -1), any |= to[k] >= 0 && to[k] != k;
    if (!any) return 0;
    FILE *fp = fopen(src, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    unsigned char *mine = size > 0 && size < (64 << 20) ? malloc((size_t)size) : NULL;
    int ok = mine && fread(mine, 1, (size_t)size, fp) == (size_t)size;
    fclose(fp);
    uint64_t gsize = 0;
    unsigned char *game = ok ? mod_game_file_hook(path, &gsize) : NULL;
    int moved = !game ? 0 : layout ? mod_layout_move(mine, (size_t)size, game, (size_t)gsize, to) : mod_prc_move(mine, (size_t)size, game, (size_t)gsize, to);
    free(game);
    if (moved) {
        char out[4096];
        moved_file(l, m, path, out, sizeof out);   /* (the same as last time: left as it is, its date too - the table's cache keeps) */
        if (write_same(out, mine, (size_t)size)) {
            fprintf(stderr, "[mods] %s: %s: %d of its colors' entries moved with them\n", m->key, path, moved);
            snprintf(src, n, "%s", out);
        } else {
            moved = 0;
            fprintf(stderr, "[mods] %s: %s: cannot write %s; its colors' entries stay where they were\n", m->key, path, out);
        }
    }
    free(mine);
    return moved;
}

/* The game's textures a moved mod's material names and the mod does not bring: a reslotter copies them.
 * Its material (a .numatb of a color it moves) names textures of its color, the mod's own or the game's
 * there ("alp_inkling_c06_002_col": Splatsune Inku's hair, the game's c06 one); the color it goes to has
 * none of those names, and without them the hair is black. Each: the game's file of the color it was
 * made for, written to cache/mods/moved/<mod>/, a new file of the color it goes to. Appended to *v. */
static void moved_textures(const ModList *l, const ModInfo *m, ModArcFile **v, int *n, int *cap) {
    if (!mod_game_file_hook) return;
    for (int k = 0; k < m->npaths; k++) {
        const char *p = m->paths[k];
        size_t pl = strlen(p);
        char f[64], mdest[2048], dir[1100] = "";
        int c;
        if (pl < 7 || strcmp(p + pl - 7, ".numatb") || !mod_path_color(p, f, sizeof f, &c) || strcmp(f, m->fighter)) continue;
        mod_dest_path(m, p, mdest, sizeof mdest);
        if (!strcmp(mdest, p)) continue;   /* (not moved) */
        moved_dir(mdest, dir, sizeof dir);
        const char *slash = strrchr(p, '/');
        if (!*dir || !slash) continue;
        size_t dl = (size_t)(slash - p) + 1;
        char src[4096];
        snprintf(src, sizeof src, "%s/%s/%s", l->root, m->dir, m->real[k]);
        FILE *fp = fopen(src, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        rewind(fp);
        unsigned char *mat = size > 0 && size < (16 << 20) ? malloc((size_t)size + 1) : NULL;
        int ok = mat && fread(mat, 1, (size_t)size, fp) == (size_t)size;
        fclose(fp);
        if (!ok) { free(mat); continue; }
        mat[size] = 0;
        int copied = 0;
        for (long i = 0; i < size; i++) {   /* (its names: a string after a byte that is not a letter, ended by a 0) */
            if (!(isalnum(mat[i]) || mat[i] == '_') || (i && (isalnum(mat[i - 1]) || mat[i - 1] == '_'))) continue;
            long e = i;
            while (e < size && (isalnum(mat[e]) || mat[e] == '_')) e++;
            if (e >= size || mat[e] || e - i < 4 || e - i > 120) { i = e; continue; }
            char name[128], path[2048], dest[2048];
            for (long j = i; j < e; j++) name[j - i] = (char)tolower(mat[j]);
            name[e - i] = 0;
            i = e;
            snprintf(path, sizeof path, "%.*s%s.nutexb", (int)dl, p, name);
            int has = 0;
            for (int q = 0; q < m->npaths && !has; q++) has = !strcmp(m->paths[q], path);
            if (has) continue;   /* (the mod's own) */
            mod_dest_path(m, path, dest, sizeof dest);
            for (int q = 0; q < *n && !has; q++) has = !strcmp((*v)[q].dest, dest);
            if (has) continue;   /* (another material named it first) */
            uint64_t gsize = 0;
            void *there = mod_game_file_hook(dest, &gsize);
            if (there) { free(there); continue; }   /* (the color it goes to has it: that one) */
            unsigned char *game = mod_game_file_hook(path, &gsize);
            if (!game) continue;   /* (not a texture of the game's there either) */
            char file[4096];
            moved_file(l, m, dest, file, sizeof file);
            int wrote = write_same(file, game, (size_t)gsize);
            free(game);
            struct stat st;
            if (!wrote || stat(file, &st)) continue;
            if (*n == *cap) {
                ModArcFile *q = realloc(*v, sizeof **v * (size_t)(*cap = *cap ? *cap * 2 : 256));
                if (!q) break;
                *v = q;
            }
            ModArcFile *x = &(*v)[*n];
            x->dest = strdup(dest), x->src = strdup(file), x->mod = strdup(m->key), x->dir = strdup(dir);
            if (!x->dest || !x->src || !x->mod || !x->dir) { free(x->dest); free(x->src); free(x->mod); free(x->dir); break; }
            x->size = (uint64_t)st.st_size;
            x->mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
            (*n)++, copied++;
        }
        free(mat);
        if (copied) fprintf(stderr, "[mods] %s: %s: %d of the game's textures it names copied with it\n", m->key, mdest, copied);
    }
}

int mod_load_list(const ModList *l, ModArcFile **out) {
    *out = NULL;
    int n = 0, cap = 0;
    ModArcFile *v = NULL;
    for (int i = 0; i < l->n; i++) {
        if (!l->mods[i].enabled) continue;
        ModInfo own = l->mods[i];   /* (a copy: its move may be undone for this load) */
        const ModInfo *m = &own;
        int bad = other_body(m);
        if (bad >= 0) {
            fprintf(stderr, "[mods] %s: moved to color %d, where %s has another body (skeleton); loaded at its own colors\n",
                    m->key, mod_color_to(m, bad) + 1, mod_fighter_name(m->fighter));
            for (int c = 0; c < MOD_COLORS; c++) own.to[c] = -1;
        }
        NewFile *nf;
        int nnf = read_new_files(l, m, &nf);
        for (int k = 0; k < m->npaths; k++) {
            const char *p = m->paths[k];
            if (!strncmp(p, "stream;", 7) || !strncmp(p, "stream:", 7)) continue;
            char dest[2048], src[4096], dir[1100] = "";
            mod_dest_path(m, p, dest, sizeof dest);
            for (int x = 0; x < nnf && !*dir; x++)
                if (!strcmp(nf[x].path, p)) {   /* (the directory moves with the color: "fighter/lucina/c03" read as a path) */
                    char probe[1100];
                    snprintf(probe, sizeof probe, "%s/x", nf[x].dir);
                    mod_dest_path(m, probe, dir, sizeof dir);
                    dir[strlen(dir) > 2 ? strlen(dir) - 2 : 0] = 0;
                }
            if (!*dir && strcmp(dest, p)) moved_dir(dest, dir, sizeof dir);
            snprintf(src, sizeof src, "%s/%s/%s", l->root, m->dir, m->real[k]);
            moved_prc(l, m, p, src, sizeof src);
            struct stat st;
            if (stat(src, &st) || !S_ISREG(st.st_mode)) continue;
            if (!st.st_size) {   /* (an unfinished download: an empty file would replace the game's) */
                fprintf(stderr, "[mods] %s: %s is empty; not loaded\n", m->key, p);
                continue;
            }
            if (n == cap) {
                ModArcFile *q = realloc(v, sizeof *v * (size_t)(cap = cap ? cap * 2 : 256));
                if (!q) break;
                v = q;
            }
            ModArcFile *f = &v[n];
            f->dest = strdup(dest), f->src = strdup(src), f->mod = strdup(m->key), f->dir = *dir ? strdup(dir) : NULL;
            if (!f->dest || !f->src || !f->mod || (*dir && !f->dir)) { free(f->dest); free(f->src); free(f->mod); free(f->dir); break; }
            f->size = (uint64_t)st.st_size;
            f->mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
            n++;
        }
        moved_textures(l, m, &v, &n, &cap);
        for (int x = 0; x < nnf; x++) free(nf[x].path), free(nf[x].dir);
        free(nf);
    }
    /* each path once: the first mod in order (a stable pass: the list is in mod order) */
    int w = 0;
    for (int i = 0; i < n; i++) {
        int dup = -1;
        for (int j = 0; j < w && dup < 0; j++) if (!strcmp(v[j].dest, v[i].dest)) dup = j;
        if (dup >= 0) {
            fprintf(stderr, "[mods] %s: %s is %s's (that mod comes first)\n", v[i].mod, v[i].dest, v[dup].mod);
            free(v[i].dest); free(v[i].src); free(v[i].mod); free(v[i].dir);
            continue;
        }
        v[w++] = v[i];
    }
    qsort(v, (size_t)w, sizeof *v, by_dest_name);
    *out = v;
    return w;
}

void mod_load_free(ModArcFile *files, int n) {
    for (int i = 0; i < n; i++) free(files[i].dest), free(files[i].src), free(files[i].mod), free(files[i].dir);
    free(files);
}

/* ---- the actions ---------------------------------------------------------------------------- */
static void rescan_keep(ModList *l) {
    char root[512];
    snprintf(root, sizeof root, "%s", l->root);
    mod_scan(l, root);
}

int mod_set_enabled(ModList *l, int i, int on, char *err, size_t n) {
    if (i < 0 || i >= l->n) { snprintf(err, n, "No mod selected"); return 0; }
    ModInfo *m = &l->mods[i];
    if (m->enabled == !!on) return 1;
    char from[1024], to[1024];
    snprintf(from, sizeof from, "%s/%s", l->root, m->dir);
    snprintf(to, sizeof to, "%s/%s%s", l->root, on ? "" : ".", m->key);
    struct stat st;
    if (!lstat(to, &st)) { snprintf(err, n, "Cannot rename: %s%s already exists", on ? "" : ".", m->key); return 0; }
    if (rename(from, to)) { snprintf(err, n, "Cannot rename %s: %s", m->dir, strerror(errno)); return 0; }
    fprintf(stderr, "[mods] %s %s\n", m->key, on ? "enabled" : "disabled");
    rescan_keep(l);
    return 1;
}

/* Everything under path, links themselves (never what they point to) */
static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int bad = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (remove_tree(sub)) bad = 1;
    }
    closedir(d);
    return bad ? -1 : rmdir(path);
}

int mod_delete(ModList *l, int i, char *err, size_t n) {
    if (i < 0 || i >= l->n) { snprintf(err, n, "No mod selected"); return 0; }
    ModInfo *m = &l->mods[i];
    char path[1024], real_root[PATH_MAX], real[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", l->root, m->dir);
    /* only a folder right inside mods/ (its own place, not where a link leads) */
    if (!realpath(l->root, real_root) || !m->dir[0] || strchr(m->dir, '/') || !strcmp(m->dir, ".") || !strcmp(m->dir, "..")) {
        snprintf(err, n, "Cannot delete %s: not inside the mods folder", m->dir);
        return 0;
    }
    struct stat st;
    if (lstat(path, &st)) { snprintf(err, n, "Cannot delete %s: %s", m->dir, strerror(errno)); return 0; }
    if (S_ISLNK(st.st_mode)) {   /* a link to a mod elsewhere: the link goes, not the files */
        if (unlink(path)) { snprintf(err, n, "Cannot delete %s: %s", m->dir, strerror(errno)); return 0; }
    } else {
        char parent[PATH_MAX];
        if (!realpath(path, real)) { snprintf(err, n, "Cannot delete %s", m->dir); return 0; }
        snprintf(parent, sizeof parent, "%s", real);
        char *slash = strrchr(parent, '/');
        if (!slash || (*slash = 0, strcmp(parent, real_root))) { snprintf(err, n, "Cannot delete %s: not inside the mods folder", m->dir); return 0; }
        if (remove_tree(path)) { snprintf(err, n, "Could not delete all of %s", m->dir); rescan_keep(l); return 0; }
    }
    fprintf(stderr, "[mods] %s deleted\n", m->key);
    rescan_keep(l);
    mod_slots_save(l);   /* (its entry left out) */
    return 1;
}

int mod_can_move(const ModList *l, int i, int delta) {
    if (i < 0 || i >= l->n) return 0;
    const ModInfo *m = &l->mods[i];
    if (!m->colors || !m->fighter[0] || m->fighters != 1) return 0;
    for (int c = 0; c < MOD_COLORS; c++)
        if (m->colors & 1u << c) {
            int to = mod_color_to(m, c) + delta;
            if (to < 0 || to >= MOD_COLORS) return 0;
            if (mod_body_hook && to != c) {   /* (another body there: not that color) */
                uint64_t a = mod_body_hook(m->fighter, c), b = mod_body_hook(m->fighter, to);
                if (a && b && a != b) return 0;
            }
        }
    return 1;
}

static unsigned dest_colors(const ModInfo *m, int delta) {
    unsigned out = 0;
    for (int c = 0; c < MOD_COLORS; c++)
        if (m->colors & 1u << c) {
            int to = mod_color_to(m, c) + delta;
            if (to >= 0 && to < MOD_COLORS) out |= 1u << to;
        }
    return out;
}

int mod_occupant(const ModList *l, int i, int delta) {
    const ModInfo *m = &l->mods[i];
    unsigned want = dest_colors(m, delta);
    for (int k = 0; k < l->n; k++) {
        const ModInfo *o = &l->mods[k];
        if (k == i || !o->enabled || o->fighters != 1 || strcmp(o->fighter, m->fighter)) continue;
        if (dest_colors(o, 0) & want) return k;
    }
    return -1;
}

int mod_move(ModList *l, int i, int delta, char *err, size_t n) {
    if (!mod_can_move(l, i, delta)) { snprintf(err, n, "It cannot go there: only colors 1 to 8 of the same body"); return 0; }
    if (!delta) return 1;
    ModInfo *m = &l->mods[i];
    int other = mod_occupant(l, i, delta);
    unsigned mine = dest_colors(m, 0);
    if (other >= 0) {   /* a single-color mod there: it takes this one's place */
        ModInfo *o = &l->mods[other];
        unsigned theirs = dest_colors(o, 0);
        if (!(theirs & (theirs - 1)) && !(mine & (mine - 1))) {
            int from = __builtin_ctz(o->colors), at = __builtin_ctz(mine);
            o->to[from] = (signed char)(at == from ? -1 : at);
            o->from_name = 0;
        }
    }
    for (int c = 0; c < MOD_COLORS; c++)
        if (m->colors & 1u << c) {
            int to = mod_color_to(m, c) + delta;
            m->to[c] = (signed char)(to == c ? -1 : to);
        }
    m->from_name = 0;
    if (!mod_slots_save(l)) { snprintf(err, n, "Cannot write %s", SLOTS_FILE); return 0; }
    fprintf(stderr, "[mods] %s moved %+d\n", m->key, delta);
    rescan_keep(l);
    return 1;
}

int mod_slots_save(const ModList *l) {
    char file[1024], tmp[1100];
    snprintf(file, sizeof file, "%s/" SLOTS_FILE, l->root);
    snprintf(tmp, sizeof tmp, "%s.new", file);
    int any = 0;
    for (int i = 0; i < l->n; i++) {
        any |= l->mods[i].no_share;
        for (int c = 0; c < MOD_COLORS; c++) any |= l->mods[i].to[c] >= 0 && !l->mods[i].from_name;
    }
    if (!any) { unlink(file); return 1; }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return 0;
    fprintf(fp, "# WindowSmash: the color each mod is loaded into (the start-up menu's Mods page writes this).\n"
                "# [mod.\"<its folder, without the dot>\"]  <fighter>.c0X = \"c0Y\"\n"
                "# share = false: not given to those who join a room you host.\n");
    for (int i = 0; i < l->n; i++) {
        const ModInfo *m = &l->mods[i];
        int has = 0;
        for (int c = 0; c < MOD_COLORS; c++) has |= m->to[c] >= 0 && !m->from_name;
        if (!has && !m->no_share) continue;
        fprintf(fp, "\n[mod.\"");
        for (const char *p = m->key; *p; p++) { if (*p == '"' || *p == '\\') fputc('\\', fp); fputc(*p, fp); }
        fprintf(fp, "\"]\n");
        if (m->no_share) fprintf(fp, "share = false\n");
        for (int c = 0; c < MOD_COLORS && !m->from_name; c++)
            if (m->to[c] >= 0) fprintf(fp, "%s.c%02d = \"c%02d\"\n", m->fighter, c, m->to[c]);
    }
    int ok = !ferror(fp);
    ok &= fclose(fp) == 0;
    if (!ok || rename(tmp, file)) { unlink(tmp); return 0; }
    return 1;
}

int mod_set_shared(ModList *l, int i, int on, char *err, size_t n) {
    if (i < 0 || i >= l->n) { snprintf(err, n, "No such mod"); return 0; }
    ModInfo *m = &l->mods[i];
    int was = m->no_share;
    m->no_share = !on;
    if (!mod_slots_save(l)) { m->no_share = was; snprintf(err, n, "Cannot write %s", SLOTS_FILE); return 0; }
    fprintf(stderr, "[mods] %s: %s in rooms hosted here\n", m->key, on ? "shared" : "not shared");
    return 1;
}

int mod_open_folder(const char *path) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {   /* a grandchild runs it, so nothing waits for it and it leaves no zombie */
        setsid();
        if (fork() == 0) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
            execlp("xdg-open", "xdg-open", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return 1;
}
#else
int mod_scan(ModList *l, const char *root) { mod_list_free(l); snprintf(l->root, sizeof l->root, "%s", root); return 0; }
void mod_list_free(ModList *l) { l->n = 0; }
int mod_find(const ModList *l, const char *key) { (void)l; (void)key; return -1; }
int mod_set_enabled(ModList *l, int i, int on, char *err, size_t n) { (void)l; (void)i; (void)on; snprintf(err, n, "Not on Windows"); return 0; }
int mod_delete(ModList *l, int i, char *err, size_t n) { (void)l; (void)i; snprintf(err, n, "Not on Windows"); return 0; }
int mod_move(ModList *l, int i, int delta, char *err, size_t n) { (void)l; (void)i; (void)delta; snprintf(err, n, "Not on Windows"); return 0; }
int mod_can_move(const ModList *l, int i, int delta) { (void)l; (void)i; (void)delta; return 0; }
int mod_occupant(const ModList *l, int i, int delta) { (void)l; (void)i; (void)delta; return -1; }
int mod_open_folder(const char *path) { (void)path; return 0; }
int mod_set_shared(ModList *l, int i, int on, char *err, size_t n) { (void)l; (void)i; (void)on; snprintf(err, n, "Not on Windows"); return 0; }
int mod_slots_save(const ModList *l) { (void)l; return 0; }
int mod_load_list(const ModList *l, ModArcFile **out) { (void)l; *out = NULL; return 0; }
void mod_load_free(ModArcFile *files, int n) { (void)n; free(files); }
#endif

int mod_gameplay_warning(const ModList *l, char *out, size_t n) {
    int count = 0, first = -1;
    if (n) out[0] = 0;
    for (int i = 0; i < l->n; i++)
        if (l->mods[i].enabled && l->mods[i].gameplay) { if (first < 0) first = i; count++; }
    if (!count || !n) return 0;
    if (count == 1) snprintf(out, n, "Online play can desync: %s changes more than looks", l->mods[first].name);
    else snprintf(out, n, "Online play can desync: %d mods change more than looks, like %s", count, l->mods[first].name);
    return 1;
}
