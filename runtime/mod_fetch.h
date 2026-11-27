/* Getting a room's files (openspec change netplay-mod-sync, room-mod-download): a queue of files known
 * by their SHA-256, each one taken from the first of
 *   the cache       mods/.netplay/blobs/<sha256> already there (another host's mod, an earlier visit)
 *   the player's    a file of the player's own mods with that hash: copied (the mod itself left as it is)
 *   the origin      the mod's .zip from the address its info.toml declares (visual mods only), when the
 *                   system has a way to fetch it (libcurl on Linux, WinHTTP on Windows) - see mod_zip.c
 *   the host        in blocks (mod_sync.h's MODGET / MODPART), MOD_WINDOW of them asked at a time and
 *                   the lost ones asked again
 * written as blobs/<sha256>.part and renamed only once its hash is the one asked for. A file that
 * fails is left out (its mod with it: the caller's to say); one already verified stays when the
 * queue is cancelled. One queue at a time, on a thread of its own. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct FetchFile {
    uint8_t sha[32];
    uint32_t size;
    char own[1024];        /* a file of the player's own mods with this hash, or "" */
    int origin;            /* its mod's .zip (FetchOrigin) to look in first, or -1 */
    int mod;               /* the caller's: which mod it is of */
} FetchFile;

typedef struct FetchOrigin {
    char url[256];
    uint8_t sha[32];       /* the .zip's */
} FetchOrigin;

enum { FS_IDLE, FS_RUNNING, FS_DONE, FS_CANCELLED };
enum { FR_WAITING, FR_OK, FR_FAILED };

typedef struct FetchStatus {
    int state;             /* FS_* */
    uint64_t done, total;  /* bytes: verified or arrived so far, of all the queue's */
    int files, files_done, failed;
    int paused;            /* the host plays a fight: it gives nothing meanwhile */
    int from_host;         /* the file now comes from the host (else copied, or from the origin) */
    char why[160];         /* the last failure, for the dialog */
} FetchStatus;

/* Start: the files of host (its address, for mod_sync.h's ModNet.get) into blob_dir. 1 started. */
int mod_fetch_start(uint32_t host, const char *blob_dir, const FetchFile *files, int n, const FetchOrigin *origins, int norigins);
void mod_fetch_status(FetchStatus *s);
int mod_fetch_result(int i);           /* FR_* of the queue's file i */
const char *mod_fetch_why(int i);      /* why it failed */
void mod_fetch_cancel(void);           /* stopped: the file being fetched dropped, the verified ones kept */
void mod_fetch_wait(void);             /* (tests) until it is not running */

/* blob_dir's file for a hash, and whether it is there with that size */
void mod_blob_path(const char *blob_dir, const uint8_t sha[32], char *out, size_t n);
int mod_blob_has(const char *blob_dir, const uint8_t sha[32], uint32_t size);
int mod_copy_file(const char *src, const char *dst);   /* 1 copied */
/* A file into blob_dir as the blob of sha: copied to .part, checked, renamed. 1 done. */
int mod_blob_copy(const char *blob_dir, const uint8_t sha[32], uint32_t size, const char *src);
/* (mod_zip.c) The origin's .zip of a mod, fetched and checked, its files whose hashes the queue
 * asks for put in blob_dir: how many. 0 when it could not (why). */
int mod_origin_fetch(const FetchOrigin *o, const char *blob_dir, const FetchFile *files, int n, int which, char *why, size_t wn);
