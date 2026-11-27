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
#include <stdint.h>
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
    char preview[16];             /* its preview.png's name as on disk (its case kept), "" none */
    unsigned kinds;               /* MK_* */
    int gameplay;                 /* a file outside the visual paths: it can change how the game plays */
    int files;                    /* its data files (info.toml, preview.png, config.json... not counted) */
    signed char to[MOD_COLORS];   /* where each of its colors goes (-1: where it is) */
    int from_name;                /* the move comes from the folder's name (-c0Y), not windowsmash.toml */
    int conflicts;                /* its files that a mod before it replaces too (only enabled mods) */
    char conflict_with[96];       /* the first such mod's name */
    int no_share;                 /* windowsmash.toml's share = false: not given to those who join a room hosted here */
    char url[256], url_sha256[65];   /* info.toml's url and url_sha256: where the mod comes from (a .zip) and its SHA-256 */
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
/* The fighter a path is of (its color's, or fighter/<f>/... without one): 1 if it is one's */
int mod_path_fighter(const char *path, char *fighter, size_t n);

/* The actions; each 1 done (and the list read again), else 0 with why into err */
int mod_set_enabled(ModList *l, int i, int on, char *err, size_t n);   /* the folder renamed, '.' added or taken off */
int mod_delete(ModList *l, int i, char *err, size_t n);                /* the folder and all in it (only inside mods/) */
/* Its colors moved by delta (all of them together, inside c00..c07); a single-color mod of the same
 * fighter at the destination takes its place. Saved in windowsmash.toml. */
int mod_move(ModList *l, int i, int delta, char *err, size_t n);
/* delta keeps all its colors in c00..c07, each where the game's fighter has the body (skeleton) of the
 * color the mod was made for - Inkling's even colors are the girl and odd ones the boy, and a girl's model
 * on the boy's animations hangs the fight's loading */
int mod_can_move(const ModList *l, int i, int delta);
/* The game's body of fighter f's color c (a hash of its model.nusktb), 0 not known: the runtime's (the
 * game's data.arc); NULL, every color has the same */
extern uint64_t (*mod_body_hook)(const char *fighter, int color);
/* The game's own file of data.arc by its path (malloc'd, NULL none): the runtime's, for mod_prc_move */
extern void *(*mod_game_file_hook)(const char *path, uint64_t *size);
/* A param file (.prc) of a mod whose colors move, in a path with no color (fighter/common/param/effect.prc:
 * Inkling's ink, a color an entry of its lists): each list of 8 entries (a color each) that the mod changed
 * at a color it moves, that entry taken to where the color goes, and the color left given back the game's.
 * mine and game: the mod's file and the game's, the same layout (else nothing done). to: the colors' moves
 * (-1 none). The entries moved (mine changed in place), 0 none. */
int mod_prc_move(unsigned char *mine, size_t n, const unsigned char *game, size_t gn, const signed char to[8]);
/* The same for a layout (layout.arc, a SARC of BFLYT/BFLAN: the fight's HUD, Inkling's ink behind the
 * portrait): an animation whose frames 0-7 are the eight colors (a curve's value a color), the frames the
 * mod changed at a color it moves taken to where the color goes. The curves matched by their pane,
 * target and kind. The keys moved, 0 none. */
int mod_layout_move(unsigned char *mine, size_t n, const unsigned char *game, size_t gn, const signed char to[8]);
/* The other enabled mod of the same fighter at the mod's colors moved by delta, or -1 */
int mod_occupant(const ModList *l, int i, int delta);
int mod_open_folder(const char *path);   /* the system's file manager on it, not waited for */
/* Shared in a room hosted here (on) or not: windowsmash.toml's share = false */
int mod_set_shared(ModList *l, int i, int on, char *err, size_t n);
int mod_slots_save(const ModList *l);    /* windowsmash.toml from the list: 1 written */

/* What the game loads (mod_arc.h): every enabled mod's data files at their paths as loaded, each
 * path once, the first mod in order winning (the others' logged); *out malloc'd, mod_load_free
 * frees it. The count, 0 with none. */
struct ModArcFile;
int mod_load_list(const ModList *l, struct ModArcFile **out);
void mod_load_free(struct ModArcFile *files, int n);

/* The netplay warning (mod-loading's "Aviso de netplay"): the enabled mods that change more than looks,
 * named, into out - 1 if there is one, else 0 and out "" */
int mod_gameplay_warning(const ModList *l, char *out, size_t n);

/* What kind of file a mod's path is (MK_*): MK_OTHER is one that can change how the game plays */
unsigned mod_path_kind(const char *path);

/* The fighter's name to show ("murabito": "Villager"); the internal one when not known */
const char *mod_fighter_name(const char *internal);
/* A fighter whose colors are different characters (Steve and Alex, the Koopalings...): moving a
 * skin between them can leave the voice or the effects of the other */
int mod_fighter_varied(const char *internal);
