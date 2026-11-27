/* runtime/mod_sync.c and mod_fetch.c without a game: tests/test_mod_sync.py builds this with them.
 * One process is the host (its mods folder) and who joins (its cache), over a network made here:
 * datagrams queued and delivered by a thread of their own, some of them lost (LOSS=0.1), and the
 * host's game in a fight for the first FIGHT seconds.
 *   mod_sync_test <mods> <blobs> <scenario> [options]
 *     scenarios: full     the manifest, then every file it gives
 *                old      the host is an older game: it never answers
 *                lobby    the host is in the online lobby (gameplay mods without files); then a MODGET of a
 *                         gameplay file (named by GAMEPLAY_SHA) is not answered with it
 *                corrupt  one byte of the first file's blocks changed on the way
 *                cancel   cancelled once a quarter has come
 *                gone     the host's answers stop once a quarter has come
 *     environment: LOSS, FIGHT (seconds), RATE (bytes a second), OWN (a file of the player's own: its
 *                  hash's file copied, not fetched), SSBU_MODS_ORIGIN_DIR (origin .zips)
 * Prints: "manifest <state> <mods> <files> <bytes>", then for the queue "file <sha> <result> <why>", and
 * "done <state> <failed> <ms> <given> <paused>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_fetch.h"
#include "mod_sha256.h"
#include "mod_sync.h"
#include "mod_thread.h"

const char *rt_data_dir = ".";

#define HOST_IP 0x0A0D0002u
#define ME_IP 0x0A0D0003u
#define BCAST 0x0A0DFFFFu

typedef struct Packet { int kind; uint32_t src, dst; size_t n; uint8_t p[1200]; struct Packet *next; } Packet;   /* kind: 0 data, 1 get, 2 part */
static ModLock qlock = MOD_LOCK_INIT;
static ModWake qwake = MOD_WAKE_INIT;
static Packet *head, *tail;
static double loss;
static uint64_t started, fight_until;
static volatile int corrupt, gone, parts_seen;
static uint64_t corrupt_sha;

static int lost(void) { return loss > 0 && (double)rand() / RAND_MAX < loss; }

static void push(int kind, uint32_t src, uint32_t dst, const void *p, size_t n) {
    if (n > sizeof head->p || lost()) return;
    Packet *k = malloc(sizeof *k);
    if (!k) return;
    k->kind = kind, k->src = src, k->dst = dst, k->n = n, k->next = NULL;
    memcpy(k->p, p, n);
    mod_lock(&qlock);
    if (tail) tail->next = k; else head = k;
    tail = k;
    mod_wake(&qwake);
    mod_unlock(&qlock);
}

static void deliver(void *unused) {
    (void)unused;
    for (;;) {
        mod_lock(&qlock);
        while (!head) mod_wait(&qwake, &qlock, 100);
        Packet *k = head;
        head = k->next;
        if (!head) tail = NULL;
        mod_unlock(&qlock);
        if (k->kind == 0) mod_sync_data(k->src, k->p, k->n);
        else if (k->kind == 1) { ModRoute r = {1, k->src, 1234, 0}; mod_sync_get(&r, k->p, k->n); }
        else mod_sync_part(k->src, k->p, k->n);
        free(k);
    }
}

/* the host is HOST_IP, who joins ME_IP; a broadcast reaches the host */
static int old_host;
static void net_data(uint32_t dst, const void *p, size_t n) {
    const uint8_t *b = p;
    int from_host = n >= 2 && b[1] == 'L';
    if (!from_host && old_host) return;   /* (an older game: port 65512 means nothing to it) */
    if (gone && from_host) return;
    push(0, from_host ? HOST_IP : ME_IP, dst == BCAST ? HOST_IP : dst, p, n);
}
static int net_get(uint32_t host, const void *p, size_t n) { if (gone) return 1; push(1, ME_IP, host, p, n); return 1; }
static void net_part(const ModRoute *to, const void *p, size_t n) {
    if (gone) return;
    uint8_t copy[1200];
    memcpy(copy, p, n);
    uint64_t sha = 0;
    for (int i = 0; i < 8; i++) sha = sha << 8 | copy[i];
    uint32_t block = (uint32_t)copy[8] << 24 | (uint32_t)copy[9] << 16 | (uint32_t)copy[10] << 8 | copy[11];
    if (corrupt && n > 12 && sha == corrupt_sha && block == 0) copy[12] ^= 0x5A;
    if (n > 12) parts_seen++;
    push(2, HOST_IP, to->ip, copy, n);
}
static int net_fight(void) { return mod_now_ms() < fight_until; }

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *mods = argv[1], *blobs = argv[2], *scenario = argv[3];
    const char *v;
    srand(1234);
    if ((v = getenv("LOSS"))) loss = atof(v);
    started = mod_now_ms();
    if ((v = getenv("FIGHT"))) fight_until = started + (uint64_t)(atof(v) * 1000);
    ModNet n = {net_data, net_get, net_part, net_fight, NULL};
    mod_sync_init(&n);
    mod_thread(deliver, NULL);
    int lobby = !strcmp(scenario, "lobby");
    old_host = !strcmp(scenario, "old");
    if (!old_host) {
        ModHost h;
        memset(&h, 0, sizeof h);
        h.lobby = lobby;
        h.host_id = 0xABCDEF0123456789ull;
        snprintf(h.name, sizeof h.name, "Alexis");
        h.share = 1;
        h.rate = (v = getenv("RATE")) ? atol(v) : 64L << 20;
        snprintf(h.mods, sizeof h.mods, "%s", mods);
        mod_host_set(&h);
    }
    mod_ask(lobby ? HOST_IP : BCAST, lobby);   /* (the lobby: the network's host, asked by its address) */
    Manifest m;
    uint64_t hash = 0;
    uint32_t from = 0;
    char err[160];
    int s;
    while ((s = mod_ask_state(&m, &hash, &from, err, sizeof err)) == MQ_ASKING) mod_sleep(10);
    printf("manifest %d %d %d %llu %llu %s\n", s, s == MQ_GOT ? m.nmods : 0, s == MQ_GOT ? m.nfiles : 0,
           s == MQ_GOT ? (unsigned long long)m.bytes : 0ull, (unsigned long long)(mod_now_ms() - started), err);
    if (s != MQ_GOT) return 0;
    if (lobby && (v = getenv("GAMEPLAY_SHA"))) {   /* a gameplay file's hash asked for anyway: never given */
        uint8_t sha[32];
        sha256_parse(v, sha);
        FetchFile f;
        memset(&f, 0, sizeof f);
        memcpy(f.sha, sha, 32);
        f.size = (uint32_t)atol(getenv("GAMEPLAY_SIZE"));
        f.origin = -1;
        mod_fetch_start(from, blobs, &f, 1, NULL, 0);
        mod_fetch_wait();
        printf("gameplay %d %s\n", mod_fetch_result(0), mod_fetch_why(0));
    }
    /* the queue: every file of every mod the manifest gives */
    static FetchFile q[MANI_FILES];
    static FetchOrigin o[MANI_MODS];
    int nq = 0, no = 0;
    for (int i = 0; i < m.nmods; i++) {
        const ManiMod *x = &m.mods[i];
        if (!(x->flags & MM_FILES) || x->why[0]) continue;
        int origin = -1;
        if (x->flags & MM_URL) { memcpy(o[no].url, x->url, sizeof o[no].url); memcpy(o[no].sha, x->url_sha, 32); origin = no++; }
        for (int k = 0; k < x->nfiles; k++) {
            const ManiFile *f = &m.files[x->first + k];
            memset(&q[nq], 0, sizeof q[nq]);
            memcpy(q[nq].sha, f->sha, 32);
            q[nq].size = f->size;
            q[nq].origin = origin;
            q[nq].mod = i;
            if ((v = getenv("OWN"))) {   /* (the player's own file: copied when its hash is this one) */
                uint8_t h[32];
                uint64_t sz;
                if (sha256_file(v, h, &sz) && !memcmp(h, f->sha, 32)) snprintf(q[nq].own, sizeof q[nq].own, "%s", v);
            }
            nq++;
        }
    }
    if (nq) corrupt_sha = sha256_u64(q[0].sha);
    corrupt = !strcmp(scenario, "corrupt");
    if ((v = getenv("NO_HOST_FILES"))) gone = 1;   /* (the files can only come from the origin) */
    uint64_t t0 = mod_now_ms();
    mod_fetch_start(from, blobs, q, nq, o, no);
    FetchStatus st;
    int saw_pause = 0;
    for (;;) {
        mod_fetch_status(&st);
        saw_pause |= st.paused;
        if (st.state != FS_RUNNING) break;
        if (st.total && st.done * 4 >= st.total) {
            if (!strcmp(scenario, "cancel")) mod_fetch_cancel();
            if (!strcmp(scenario, "gone")) gone = 1;
        }
        mod_sleep(5);
    }
    for (int i = 0; i < nq; i++) {
        char hex[65];
        sha256_hex(q[i].sha, hex);
        printf("file %s %d %s\n", hex, mod_fetch_result(i), mod_fetch_why(i));
    }
    uint64_t given = 0, paused = 0;
    mod_host_stats(&given, &paused);
    printf("done %d %d %llu %llu %llu %d\n", st.state, st.failed, (unsigned long long)(mod_now_ms() - t0), (unsigned long long)given,
           (unsigned long long)paused, saw_pause);
    mani_free(&m);
    return 0;
}
