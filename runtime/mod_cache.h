/* The rooms' mods kept here (openspec change netplay-mod-sync, room-mod-download): mods_join/, beside mods/
 *
 *   <host>/<mod>/...            to look at: each host's mods as folders (mod_cache_views)
 *   .blobs/<sha256>             each file once, whichever hosts use it (mod_fetch.h writes them)
 *   .hosts/<host_id>/host.toml  a host's: its name, when it was last joined, its manifest's hash, and
 *                              each of its mods - id, name, kind, the colors it is moved to, whether
 *                              the host still has it, and its files (data.arc path -> SHA-256)
 *
 * A host's mods are a "room set" the game loads in place of (mirror) or under (overlay) the player's
 * own (mod_room_load_list) - never copied into mods/, whose folders are never touched. When the host
 * is joined again its manifest is compared with what is kept (mod_reconcile): the same mod is not
 * fetched again, a moved one only changes its colors, a changed one fetches only the files that are
 * not here, and one the host dropped stays here, unused, until the host is forgotten - 30 days
 * without joining it, or deleted on the Mods page. Then the blobs no host uses go too (a day after
 * they came: a download cancelled keeps what it had verified for the next try). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mod_fetch.h"
#include "mod_manifest.h"
#include "mod_scan.h"

#define HOST_MODS 256
#define HOST_DAYS 30

typedef struct HostFile { char path[MANI_PATH]; uint8_t sha[32]; uint32_t size; } HostFile;
typedef struct HostMod {
    uint64_t id;
    char name[96], version[32], fighter[24];
    int gameplay, active;
    signed char to[MOD_COLORS];
    int first, nfiles;
} HostMod;
typedef struct HostIndex {
    uint64_t host_id;
    char name[MANI_NAME + 1];
    long long last;            /* when it was last joined (seconds since 1970) */
    uint64_t manifest;         /* that time's manifest's hash */
    int nmods, nfiles;
    HostMod mods[HOST_MODS];
    HostFile *files;
} HostIndex;

/* mods_join/ beside the mods folder (an earlier mods/.netplay moved there), and its .blobs/ (both made) */
const char *mod_cache_dir(const char *mods_root, char *out, size_t n);
const char *mod_cache_blobs(const char *cache, char *out, size_t n);

int host_index_read(const char *cache, uint64_t host_id, HostIndex *h);   /* 1 read; 0 none (h empty, its id set) */
int host_index_write(const char *cache, const HostIndex *h);               /* written whole, then renamed: 1 */
void host_index_free(HostIndex *h);
int mod_cache_delete_host(const char *cache, uint64_t host_id);           /* its index and its folder (its blobs go at the next start) */
/* mods_join/<host>/<mod>/...: the host's active mods as normal mod folders (their files, config.json,
 * an info.toml), hard links to the blobs (copies where links cannot be), and mods_join/<host>/
 * windowsmash.toml with the colors the host plays them in - the room's configuration. Made again
 * whole; the game loads the room from there (mod_room_load_list). The folder to dir_out; how many files. */
int mod_cache_views(const char *cache, const HostIndex *h, const char *blobs, char *dir_out, size_t dn);

typedef struct HostSummary { uint64_t host_id; char name[MANI_NAME + 1]; long long last; int mods, active; uint64_t bytes; } HostSummary;
int mod_cache_hosts(const char *cache, HostSummary *out, int most);        /* the hosts kept, the last joined first */

/* At start: the .part files of a download cut short gone; then the hosts not joined for `days`, and
 * the blobs no host uses (a day old at least). now: seconds since 1970. */
void mod_cache_tidy(const char *cache, long long now, int days);

/* ---- a manifest against what is kept ---- */
enum { RS_SAME, RS_MOVED, RS_CHANGED, RS_NEW, RS_REMOVED, RS_UNAVAILABLE, RS_DROPPED };
typedef struct RecMod {
    int state;                 /* RS_* */
    int mani, old;             /* its index in the manifest, in the host's index (-1: none) */
    char name[96], fighter[24];
    int gameplay;
    int from_color, to_color;  /* (moved: the first of its colors, where it was and where it is now) */
    int files;                 /* to fetch (not in the cache), and their bytes; */
    uint64_t bytes;
    int own;                   /* of them, ones the player's own mods have (copied, not fetched) */
    uint64_t own_bytes;
} RecMod;
typedef struct Reconcile {
    int n;
    RecMod mods[MANI_MODS + HOST_MODS];
    int files, own;            /* all of them */
    uint64_t bytes, own_bytes;
    int news, changed, moved, same, removed, dropped;
    int gameplay_missing;      /* the host's mods that change how the game plays and that cannot be had */
    int origin;                /* some visual mod's files come from where it comes from (its address sees the player's) */
    int dialog;                /* something to fetch, to agree to, or to warn of: the dialog is shown */
} Reconcile;

/* A file of the player's own mods, by its hash (mani_hash_all) */
typedef struct OwnFiles { ManiFile *files; int n; } OwnFiles;
/* old: the host's index (nmods 0: none); own: the player's files (NULL: none) */
void mod_reconcile(const Manifest *m, const HostIndex *old, const char *blobs, const OwnFiles *own, Reconcile *r);
/* The queue for mod_fetch_start: every file the accepted mods lack. *files and *origins malloc'd. */
int mod_fetch_list(const Manifest *m, const Reconcile *r, const char *blobs, const OwnFiles *own, FetchFile **files, int *n,
                   FetchOrigin **origins, int *norigins);
/* The index after the fetch: the manifest's mods whose files are all here, active; the rest as they were, inactive. */
void host_index_update(HostIndex *h, const Manifest *m, const char *blobs, long long now, uint64_t manifest_hash);

/* ---- what the game loads ---- */
enum { ROOM_MIRROR, ROOM_OVERLAY };
/* What the game loads this start (SSBU_ROOM_MODS="<host id>:mirror|overlay"): set when the player
 * agreed (mod_room.c), read by hle_fs.c. 1 and the host and mode when there is one. */
int mod_room_chosen(uint64_t *host_id, int *mode);
/* The room set of h over the player's own mods own: its folders (mod_cache_views) read as the mods folder
 * is; mirror, the host's active mods alone; overlay, the player's own visual mods first (they win where
 * they clash), then the host's. The player's own gameplay mods never. As mod_load_list's: *out
 * malloc'd, the count. */
int mod_room_load_list(const ModList *own, const HostIndex *h, const char *cache, int mode, struct ModArcFile **out);
