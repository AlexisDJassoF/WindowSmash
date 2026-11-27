/* Getting a room's files: see mod_fetch.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "mod_fetch.h"
#include "mod_sha256.h"
#include "mod_sync.h"
#include "mod_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define SILENT_MS 10000   /* nothing from the host for this long (not in a fight): it is gone (SSBU_MODS_SILENT_MS: tests) */
/* a block asked for and not come after three round trips (100 ms to 1 s) is asked again */
static uint64_t srtt = 100;   /* (the round trip, smoothed: from the blocks that come) */
static uint64_t ask_again_ms(void) { uint64_t t = srtt * 3 + 30; return t < 100 ? 100 : t > 1000 ? 1000 : t; }

void mod_blob_path(const char *dir, const uint8_t sha[32], char *out, size_t n) {
    char hex[65];
    sha256_hex(sha, hex);
    snprintf(out, n, "%s/%s", dir, hex);
}

int mod_blob_has(const char *dir, const uint8_t sha[32], uint32_t size) {
    char p[1100];
    mod_blob_path(dir, sha, p, sizeof p);
    struct stat st;
    return !stat(p, &st) && (uint64_t)st.st_size == size;
}

static int finish_part(const char *part, const uint8_t sha[32], uint32_t size, const char *blob) {
    uint8_t got[32];
    uint64_t n = 0;
    if (!sha256_file(part, got, &n) || n != size || memcmp(got, sha, 32)) { remove(part); return 0; }
    remove(blob);   /* (Windows: rename does not replace) */
    if (rename(part, blob)) { remove(part); return 0; }
    return 1;
}

int mod_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    if (!in || !out) { if (in) fclose(in); return 0; }
    char *buf = malloc(1 << 16);
    size_t got;
    int ok = buf != NULL;
    while (ok && (got = fread(buf, 1, 1 << 16, in)) > 0) ok &= fwrite(buf, 1, got, out) == got;
    free(buf);
    ok &= !ferror(in);
    fclose(in);
    ok &= fclose(out) == 0;
    if (!ok) remove(dst);
    return ok;
}

int mod_blob_copy(const char *dir, const uint8_t sha[32], uint32_t size, const char *src) {
    char blob[1100], part[1110];
    mod_blob_path(dir, sha, blob, sizeof blob);
    snprintf(part, sizeof part, "%s.part", blob);
    remove(part);
#ifndef _WIN32
    if (!link(src, part)) return finish_part(part, sha, size, blob);   /* (the player's own file: linked, not copied - checked all the same) */
#endif
    if (!mod_copy_file(src, part)) return 0;
    return finish_part(part, sha, size, blob);
}

/* ---- the queue ------------------------------------------------------------------------------------ */
static ModLock lock = MOD_LOCK_INIT;
static ModWake wake = MOD_WAKE_INIT;
static FetchStatus st;
static FetchFile *queue;
static FetchOrigin *origins;
static int nqueue, norigins, *results, cancelled, running;
static char (*whys)[96];
static uint32_t host;
static char dir[1024];
/* the file coming from the host now */
static uint64_t cur_sha;
static uint32_t cur_blocks, cur_size;
static uint8_t *cur_got;
static uint64_t *cur_asked;
static FILE *cur_file;
static int cur_not_here, cur_bad_write;
static uint64_t heard, paused_until;

static void fail(int i, const char *why) {   /* (lock held) */
    results[i] = FR_FAILED;
    snprintf(whys[i], sizeof whys[i], "%s", why);
    snprintf(st.why, sizeof st.why, "%s", why);
    st.failed++;
}

void mod_fetch_part(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 12) return;
    uint64_t sha = (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 | (uint64_t)p[2] << 40 | (uint64_t)p[3] << 32 |
                   (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 | (uint64_t)p[6] << 8 | p[7];
    uint32_t block = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
    mod_lock(&lock);
    if (!running || src != host || !cur_got || sha != cur_sha) { mod_unlock(&lock); return; }
    uint64_t t = mod_now_ms();
    heard = t;
    if (block == MOD_PAUSED) { paused_until = t + 1000; st.paused = 1; }
    else if (block == MOD_NOT_HERE) cur_not_here = 1;
    else if (block < cur_blocks && !cur_got[block]) {
        size_t want = block + 1 < cur_blocks ? MOD_BLOCK : cur_size - (size_t)block * MOD_BLOCK;
        if (n - 12 == want) {
            if (fseek(cur_file, (long)block * MOD_BLOCK, SEEK_SET) || fwrite(p + 12, 1, want, cur_file) != want) cur_bad_write = 1;
            if (cur_asked[block] && t >= cur_asked[block]) srtt = (srtt * 7 + (t - cur_asked[block])) / 8;
            cur_got[block] = 1;
            st.done += want;
            st.paused = 0;
        }
    }
    mod_wake(&wake);
    mod_unlock(&lock);
}

/* From the host, in blocks: 1, 0 failed (why), -1 cancelled (lock held throughout but while waiting) */
static int from_host(int i, char *why, size_t wn) {
    FetchFile *f = &queue[i];
    char blob[1100], part[1110];
    mod_blob_path(dir, f->sha, blob, sizeof blob);
    snprintf(part, sizeof part, "%s.part", blob);
    FILE *out = fopen(part, "wb");
    if (!out) { snprintf(why, wn, "cannot write in %s", dir); return 0; }
    cur_blocks = (f->size + MOD_BLOCK - 1) / MOD_BLOCK;
    cur_size = f->size;
    cur_got = calloc(cur_blocks ? cur_blocks : 1, 1);
    cur_asked = calloc(cur_blocks ? cur_blocks : 1, sizeof *cur_asked);
    if (!cur_got || !cur_asked) { free(cur_got); free(cur_asked); cur_got = NULL; fclose(out); remove(part); snprintf(why, wn, "out of memory"); return 0; }
    cur_file = out;
    cur_sha = sha256_u64(f->sha);
    cur_not_here = cur_bad_write = 0;
    heard = mod_now_ms();
    paused_until = 0;
    st.from_host = 1;
    int result = 0;
    const char *v = getenv("SSBU_MODS_SILENT_MS");
    uint64_t silent_ms = v && atoi(v) > 0 ? (uint64_t)atoi(v) : SILENT_MS;
    uint32_t next = 0;   /* (the first block not come: every block before it has) */
    for (;;) {
        if (cancelled) { result = -1; break; }
        while (next < cur_blocks && cur_got[next]) next++;
        if (next == cur_blocks) { result = 1; break; }
        uint64_t t = mod_now_ms();
        if (cur_not_here) { snprintf(why, wn, "the host does not give this file now"); break; }
        if (cur_bad_write) { snprintf(why, wn, "cannot write in %s", dir); break; }
        if (t < paused_until) { heard = t; mod_wait(&wake, &lock, 200); continue; }   /* (a fight: no lost time counted) */
        st.paused = 0;
        if (t - heard > silent_ms) { snprintf(why, wn, "the host stopped answering"); break; }
        /* the blocks to ask for: not come, and not asked lately; MOD_WINDOW at most in flight */
        int flight = 0;
        uint64_t again = ask_again_ms();
        for (uint32_t b = next; b < cur_blocks && flight < MOD_WINDOW; b++)
            if (!cur_got[b] && cur_asked[b] && t - cur_asked[b] < again) flight++;
        uint8_t req[14];
        int no_way = 0;
        for (uint32_t b = next; b < cur_blocks && flight < MOD_WINDOW;) {
            if (cur_got[b] || (cur_asked[b] && t - cur_asked[b] < again)) { b++; continue; }
            uint32_t first = b;
            unsigned count = 0;
            while (b < cur_blocks && count < (unsigned)(MOD_WINDOW - flight) && !cur_got[b] && !(cur_asked[b] && t - cur_asked[b] < again)) {
                cur_asked[b] = t;
                b++, count++;
            }
            flight += (int)count;
            for (int k = 0; k < 8; k++) req[k] = (uint8_t)(cur_sha >> (56 - 8 * k));
            req[8] = (uint8_t)(first >> 24), req[9] = (uint8_t)(first >> 16), req[10] = (uint8_t)(first >> 8), req[11] = (uint8_t)first;
            req[12] = (uint8_t)(count >> 8), req[13] = (uint8_t)count;
            uint32_t to = host;
            mod_unlock(&lock);
            if (!mod_sync_send_get(to, req, sizeof req)) no_way = 1;
            mod_lock(&lock);
            if (no_way || cancelled) break;
        }
        if (no_way) { snprintf(why, wn, "no direct connection to the host"); break; }
        mod_wait(&wake, &lock, 50);
    }
    free(cur_got);
    cur_got = NULL;   /* (no part taken from now) */
    cur_file = NULL;
    mod_unlock(&lock);
    int ok = fclose(out) == 0;
    mod_lock(&lock);
    free(cur_asked);
    cur_asked = NULL;
    st.from_host = 0;
    if (result != 1) { remove(part); return result; }
    mod_unlock(&lock);
    ok = ok && finish_part(part, f->sha, f->size, blob);
    mod_lock(&lock);
    if (!ok) { snprintf(why, wn, "its SHA-256 is not the one in the manifest"); return 0; }
    return 1;
}

static void fetch_thread(void *unused) {
    (void)unused;
    mod_lock(&lock);
    for (int o = 0; o < norigins && !cancelled; o++) {   /* the origins first: each .zip once, for all its files */
        int wanted = 0;
        for (int i = 0; i < nqueue; i++) wanted |= queue[i].origin == o && !mod_blob_has(dir, queue[i].sha, queue[i].size) && !queue[i].own[0];
        if (!wanted) continue;
        FetchOrigin copy = origins[o];
        mod_unlock(&lock);
        char why[160] = "";
        int got = mod_origin_fetch(&copy, dir, queue, nqueue, o, why, sizeof why);
        mod_lock(&lock);
        if (!got) fprintf(stderr, "[mods] from %s: %s; from the host instead\n", copy.url, why[0] ? why : "nothing");
    }
    for (int i = 0; i < nqueue && !cancelled; i++) {
        FetchFile *f = &queue[i];
        if (results[i] == FR_FAILED) continue;   /* (given up already: the host is gone) */
        char why[160] = "";
        int r = 0;
        mod_unlock(&lock);
        if (mod_blob_has(dir, f->sha, f->size)) r = 1;
        else if (f->own[0] && mod_blob_copy(dir, f->sha, f->size, f->own)) r = 1;
        mod_lock(&lock);
        if (!r) r = from_host(i, why, sizeof why);
        if (r < 0) break;
        if (r) { results[i] = FR_OK; st.files_done++; }
        else fail(i, why);
        st.done = 0;   /* (counted again: the verified ones whole) */
        for (int k = 0; k < nqueue; k++) if (results[k] == FR_OK) st.done += queue[k].size;
        if (!r && (!strcmp(why, "the host stopped answering") || !strcmp(why, "no direct connection to the host")))
            for (int k = i + 1; k < nqueue; k++)   /* (the rest can only come the same way: given up too, unless they are here) */
                if (!mod_blob_has(dir, queue[k].sha, queue[k].size) && !queue[k].own[0]) fail(k, why);
    }
    st.state = cancelled ? FS_CANCELLED : FS_DONE;
    st.paused = 0;
    running = 0;
    mod_wake(&wake);
    mod_unlock(&lock);
}

int mod_fetch_start(uint32_t h, const char *blob_dir, const FetchFile *files, int n, const FetchOrigin *orig, int norig) {
    mod_lock(&lock);
    if (running) { mod_unlock(&lock); return 0; }
    free(queue); free(results); free(whys); free(origins);
    queue = malloc(sizeof *queue * (size_t)(n ? n : 1));
    results = calloc((size_t)(n ? n : 1), sizeof *results);
    whys = calloc((size_t)(n ? n : 1), sizeof *whys);
    origins = malloc(sizeof *origins * (size_t)(norig ? norig : 1));
    if (!queue || !results || !whys || !origins) { mod_unlock(&lock); return 0; }
    memcpy(queue, files, sizeof *queue * (size_t)n);
    if (norig) memcpy(origins, orig, sizeof *origins * (size_t)norig);
    nqueue = n, norigins = norig;
    host = h;
    snprintf(dir, sizeof dir, "%s", blob_dir);
    memset(&st, 0, sizeof st);
    st.state = FS_RUNNING;
    st.files = n;
    for (int i = 0; i < n; i++) st.total += files[i].size;
    cancelled = 0;
    running = 1;
    if (!mod_thread(fetch_thread, NULL)) { running = 0; st.state = FS_IDLE; mod_unlock(&lock); return 0; }
    mod_unlock(&lock);
    return 1;
}

void mod_fetch_status(FetchStatus *s) {
    mod_lock(&lock);
    *s = st;
    mod_unlock(&lock);
}

int mod_fetch_result(int i) {
    mod_lock(&lock);
    int r = i >= 0 && i < nqueue && results ? results[i] : FR_FAILED;
    mod_unlock(&lock);
    return r;
}

const char *mod_fetch_why(int i) {
    static char out[96];
    mod_lock(&lock);
    snprintf(out, sizeof out, "%s", i >= 0 && i < nqueue && whys ? whys[i] : "");
    mod_unlock(&lock);
    return out;
}

void mod_fetch_cancel(void) {
    mod_lock(&lock);
    if (running) cancelled = 1;
    mod_wake(&wake);
    mod_unlock(&lock);
}

void mod_fetch_wait(void) {
    mod_lock(&lock);
    while (running) mod_wait(&wake, &lock, 100);
    mod_unlock(&lock);
}
