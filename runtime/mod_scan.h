/* The mods folder (mod_scan.c): mods/ next to save/ and cache/, a mod per folder in the ARCropolis
 * layout (files at data.arc's paths: fighter/mario/model/body/c03/...), '.' first for a disabled
 * one. What each mod is - its info.toml, the fighter and colors its paths are in, what kind of
 * files - which ones replace the same file (the first in alphabetical order wins), and the color
 * each one is moved to (mods/windowsmash.toml, or a folder name ending in -c0Y). And what the Mods
 * page does to them: enable, disable, delete, move, open the folder.
 *
 * No window here (tests/test_mod_menu.py drives it through mod_menu.c). Linux; on Windows the
 * folder is never read (no start-up menu there yet). */
#pragma once
#include <stddef.h>

#define MOD_MAX 240               /* (the menu scrolls 256 rows at most: the cards, the detail, the button) */
#define MOD_COLORS 8              /* c00..c07: the colors every fighter has */

enum { MK_MODEL = 1, MK_PORTRAIT = 2, MK_VOICE = 4, MK_EFFECT = 8, MK_OTHER = 16 };   /* the kinds of its files */

typedef struct ModInfo {
    char dir[128];                /* its folder's name in mods/ ('.' first: disabled) */
    char key[128];                /* the name without the '.': what windowsmash.toml calls it */
    char name[96], authors[96], version[32], description[256];   /* info.toml's, the name else the folder's */
    int enabled;
    char fighter[24];             /* the fighter its files are of ("palutena"; the first one of several; "" none) */
    int fighters;                 /* how many fighters it touches */
    unsigned colors;              /* that fighter's colors its paths are in (bit i: c0i) */
    int high_colors;              /* paths in c08 or above (new colors: not supported) */
    int has_config, has_stream;   /* config.json; stream: files (music) */
    unsigned kinds;               /* MK_* */
    int gameplay;                 /* a file outside the visual paths: it can change how the game plays */
    int files;                    /* its data files (info.toml, preview.png, config.json... not counted) */
    signed char to[MOD_COLORS];   /* where each of its colors goes (-1: where it is) */
    int from_name;                /* the move comes from the folder's name (-c0Y), not windowsmash.toml */
    int conflicts;                /* its files that a mod before it replaces too (only enabled mods) */
    char conflict_with[96];       /* the first such mod's name */
    char **paths;                 /* its data files, lower case, '/' between */
    char **real;                  /* the same as they are on disk (case kept) */
    int npaths;
} ModInfo;

typedef struct ModList {
    char root[512];               /* the mods folder */
    int off;                      /* SSBU_MODS=0: the folder is not read */
    int n;
    ModInfo mods[MOD_MAX];        /* in alphabetical order (of key, ignoring case) */
} ModList;

/* data_root's mods/ (SSBU_DATA_ROOT, else next to save/) */
const char *mod_root(char *out, size_t n);
/* Reads root (created if missing): 1 read, 0 not (the list then empty). Frees what was there. */
int mod_scan(ModList *l, const char *root);
void mod_list_free(ModList *l);
int mod_find(const ModList *l, const char *key);   /* the index of the mod called key, or -1 */

/* The color c of the mod's fighter is loaded as (c itself when not moved) */
int mod_color_to(const ModInfo *m, int c);
/* A path of the mod as it is loaded: its color moved (fighter/<f>/.../c0X/..., chara_N_<f>_0X.bntx) */
void mod_dest_path(const ModInfo *m, const char *path, char *out, size_t n);
/* The fighter and color a path is of: 1 if it is one (color: its number, 8 and over too) */
int mod_path_color(const char *path, char *fighter, size_t n, int *color);

/* The actions; each 1 done (and the list read again), else 0 with why into err */
int mod_set_enabled(ModList *l, int i, int on, char *err, size_t n);   /* the folder renamed, '.' added or taken off */
int mod_delete(ModList *l, int i, char *err, size_t n);                /* the folder and all in it (only inside mods/) */
/* Its colors moved by delta (all of them together, inside c00..c07); a single-color mod of the same
 * fighter at the destination takes its place. Saved in windowsmash.toml. */
int mod_move(ModList *l, int i, int delta, char *err, size_t n);
int mod_can_move(const ModList *l, int i, int delta);   /* delta keeps all its colors in c00..c07 */
/* The other enabled mod of the same fighter at the mod's colors moved by delta, or -1 */
int mod_occupant(const ModList *l, int i, int delta);
int mod_open_folder(const char *path);   /* the system's file manager on it, not waited for */
int mod_slots_save(const ModList *l);    /* windowsmash.toml from the list: 1 written */

/* What the game loads (mod_arc.h): every enabled mod's data files at their paths as loaded, each
 * path once, the first mod in order winning (the others' logged); *out malloc'd, mod_load_free
 * frees it. The count, 0 with none. */
struct ModArcFile;
int mod_load_list(const ModList *l, struct ModArcFile **out);
void mod_load_free(struct ModArcFile *files, int n);

/* The fighter's name to show ("murabito": "Villager"); the internal one when not known */
const char *mod_fighter_name(const char *internal);
/* A fighter whose colors are different characters (Steve and Alex, the Koopalings...): moving a
 * skin between them can leave the voice or the effects of the other */
int mod_fighter_varied(const char *internal);
