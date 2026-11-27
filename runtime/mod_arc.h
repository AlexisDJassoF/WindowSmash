/* The mods' data.arc (mod_arc.c): the game's data.arc as the game sees it with the enabled mods'
 * files in it (openspec change mod-support, decisions 2, 3, 5 and 13).
 *
 * N being data.arc's size, the archive the game reads is:
 *   [0, 0x38)       the header, its file system table moved to the end
 *   [0x38, N)       data.arc's own bytes
 *   past N          the new table (a zstd frame of raw blocks), then every folder a mod touches,
 *                   rebuilt with its files in the same order and the mods' in their places, each a
 *                   zstd frame of raw blocks made on the fly from the mod's file (never copied)
 * as a list of segments. No file is read or written here except through ModArcIO, so that the
 * runtime (hle_fs.c) and the tests (tests/mod_arc_test.c) give it their own. */
#pragma once
#include <stdint.h>

typedef struct ModArcFile {
    char *dest;        /* the data.arc path it replaces (lower case, the mod's color move applied) */
    char *src;         /* the file */
    char *mod;         /* its mod's folder name, for the log */
    char *dir;         /* a new file (config.json's new-dir-files): the data.arc directory it is added to (the
                        * color move applied), NULL none */
    uint64_t size, mtime;
} ModArcFile;

typedef struct ModArcIO {
    void *ctx;
    int (*arc)(void *ctx, uint64_t at, void *buf, uint64_t size);                /* data.arc's bytes: 1 read */
    int (*mod)(void *ctx, int file, uint64_t at, void *buf, uint64_t size);      /* a ModArcFile's bytes */
} ModArcIO;

enum { SEG_ARC = 0, SEG_MEM = 1, SEG_MOD = 2 };
typedef struct ModArcSeg {
    uint64_t at, size;  /* where in the archive the game sees */
    uint64_t src;       /* SEG_ARC: offset in data.arc; SEG_MEM: in blob; SEG_MOD: the file's index */
    uint32_t kind, pad;
} ModArcSeg;

typedef struct ModArc {
    int on;                     /* there is something to serve */
    uint8_t header[0x38];
    uint64_t n, total;          /* data.arc's size, the archive's */
    ModArcSeg *seg;             /* in place order, all past n */
    uint32_t nseg;
    uint8_t *blob;              /* the new table and the copies of files given back to other colors */
    uint64_t blob_size;
    const ModArcFile *files;
    int nfiles;
    volatile uint8_t *read_once;   /* per file: the game read it already (logged the first time) */
    uint8_t **over;             /* per file: the bytes served in its place (NULL: the file's own), and their size: */
    uint64_t *over_size;        /*   a .nus3bank with the game's BINF (its name and ID: what the game finds it by) */
} ModArc;

/* The archive for files[0..n) over a data.arc of arc_size bytes: from cache_file when it was made
 * for the same data.arc and the same files (path, size, date), else built and saved there (NULL:
 * no cache). 1 when there is something to serve (a->on); 0 nothing (no file found in data.arc, or
 * data.arc unreadable: then the game sees it as it is). `files` must outlive `a`. */
int mod_arc_setup(ModArc *a, uint64_t arc_size, const ModArcIO *io, const ModArcFile *files, int n, const char *cache_file);
/* Called with each mod file the game is given (its mod and data.arc path, strings that last): NULL none */
extern void (*mod_arc_on_read)(const char *mod, const char *dest);
/* The archive's bytes [at, at + size): 1 read */
int mod_arc_read(const ModArc *a, uint64_t at, void *buf, uint64_t size, const ModArcIO *io);
void mod_arc_free(ModArc *a);

/* Whether the last setup took the table from the cache (1) or built it (0): for the log and the tests */
extern int mod_arc_from_cache;

/* One file of data.arc by its path ("ui/replace/chara/chara_7/chara_7_mario_00.bntx"), decompressed:
 * malloc'd, its size into *size; NULL when it is not there or cannot be read. The first call reads the
 * file system table, kept until mod_arc_read_done (before the game: mod_arc_setup reads its own). For
 * one thread at a time (the menu's pictures' loader). */
void *mod_arc_read_path(const ModArcIO *io, const char *path, uint64_t *size);
void mod_arc_read_done(void);

/* The same on the game's own data.arc, in its RomFS (hle_fs.c): for the start-up menu's pictures */
void *mod_arc_game_file(const char *path, uint64_t *size);
void mod_arc_game_done(void);
/* The game's body of fighter f's color c: a hash of fighter/<f>/model/body/c0<c>/model.nusktb, 0 none
 * (mod_scan.h's mod_body_hook; kept per fighter) */
uint64_t mod_arc_game_body(const char *fighter, int color);
