/* The room's manifest (openspec change netplay-mod-sync, room-mod-manifest): the mods a host plays
 * with, as it tells those who join its room - for each mod its id, name, version, kind (visual, or
 * one that changes how the game plays), fighter, the color each of its colors is moved to, and,
 * when it gives them, its files: their data.arc paths, sizes and SHA-256. Never a local path, never
 * a file outside data.arc's folders, never code.
 *
 * What is announced (mani_build):
 *   visual mods            with their files - unless share_mods = off or the mod is marked No share:
 *                          then not at all
 *   gameplay mods          always listed (who joins must know); their files only in a private room,
 *                          with share_mods on and the mod not marked No share
 * up to MANI_MODS mods, MANI_FILES files and MANI_TOTAL bytes in all, MANI_FILE_MAX a file: a mod
 * past those is left out ("[mods]" in the log). Downloaded mods (mods/.netplay) are never in it.
 *
 * Encoding, every number big-endian:
 *   "SNMF" version u8, host_id u64, host name[32], flags u8 (1: a lobby's room), mods u8, files u16
 *   a mod:  id u64, flags u8 (MM_*), name, version, fighter (each a length u8 and its bytes),
 *           to[8] (i8: -1 where it is), files u16, then with MM_URL: url (length u16, bytes), sha256[32]
 *   a file: path (length u8, bytes), size u32, sha256[32]   (a mod's config.json too, as "config.json": the
 *           ARCropolis file that says where its new files go - data, never code)
 * the mods first, then all the files, a mod's after the one before's. mani_parse takes nothing it
 * has not checked: a manifest cut short or with a wrong count is refused whole; a mod with a path
 * outside the allowed ones, a size past the limits, an id that is not its files' is dropped (why:
 * ManiMod.why) and the rest kept. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mod_scan.h"

#define MANI_VERSION 2
#define MANI_MODS 128
#define MANI_FILES 20000
#define MANI_TOTAL (4ull << 30)
#define MANI_FILE_MAX (128u << 20)
#define MANI_PATH 256
#define MANI_NAME 32                    /* the host's name (net.h's NAME_LEN) */
#define MANI_CONFIG "config.json"       /* the one file of a mod's own given with it (where its new files go) */
#define MANI_CONFIG_MAX (1u << 20)

enum { MM_GAMEPLAY = 1, MM_FILES = 2, MM_URL = 4 };

typedef struct ManiFile {
    char path[MANI_PATH];   /* data.arc's path, as the mod has it (before its color is moved) */
    uint32_t size;
    uint8_t sha[32];
    char *src;              /* the host's: where it is on disk (never sent) */
} ManiFile;

typedef struct ManiMod {
    uint64_t id;            /* the first 8 bytes of the SHA-256 of its files (path and hash, by path) */
    uint8_t flags;          /* MM_* */
    char name[96], version[32], fighter[24];
    signed char to[MOD_COLORS];
    char url[256];          /* MM_URL: where the mod comes from (a .zip, https), and the .zip's SHA-256 */
    uint8_t url_sha[32];
    int first, nfiles;      /* its files in Manifest.files */
    uint64_t bytes;
    char why[96];           /* mani_parse: why it was dropped ("" kept) */
} ManiMod;

typedef struct Manifest {
    uint64_t host_id;
    char host[MANI_NAME + 1];
    int lobby;              /* made for (or read from) a room of the online lobby */
    int nmods, nfiles;
    ManiMod mods[MANI_MODS];
    ManiFile *files;
    uint64_t bytes;         /* its files' sizes in all */
} Manifest;

typedef struct ManiOptions {
    uint64_t host_id;
    const char *host;       /* the host's player name */
    int lobby;              /* the online lobby: gameplay mods without their files */
    int share;              /* share_mods */
    const char *hash_cache; /* the file the files' hashes are kept in between starts (NULL: none) */
} ManiOptions;

/* A path a room's mod may have: lower case letters, digits and _ . - + between '/', in one of
 * data.arc's folders (fighter/, ui/, sound/...), never .., an absolute path, a hidden file, stream:,
 * or code (.nro, .lua...); or its config.json */
int mani_path_ok(const char *path);
uint64_t mani_mod_id(const ManiFile *files, int n);

/* The host's: from the enabled mods of l. 1, or 0 (out of memory). */
int mani_build(Manifest *m, const ModList *l, const ManiOptions *o);
/* *out malloc'd: its bytes; 1, or 0 */
int mani_encode(const Manifest *m, uint8_t **out, size_t *n);
uint64_t mani_hash(const uint8_t *bytes, size_t n);   /* the manifest's hash: its encoding's first 8 SHA-256 bytes */
/* Who joins: 1 read (lobby: the room is the online lobby's - its gameplay mods' files are not taken),
 * 0 refused (why in err) */
int mani_parse(Manifest *m, const uint8_t *bytes, size_t n, int lobby, char *err, size_t en);
void mani_free(Manifest *m);

/* Every data file of every mod of l, enabled or not, with its hash and where it is (src): what
 * who joins already has, to copy instead of fetching. *out malloc'd (mani_free_files); the count. */
int mani_hash_all(const ModList *l, const char *hash_cache, ManiFile **out);
void mani_free_files(ManiFile *files, int n);

/* The file of the manifest with this hash (the short one, sha256_u64), or NULL */
const ManiFile *mani_find_file(const Manifest *m, uint64_t sha64, int *mod);
