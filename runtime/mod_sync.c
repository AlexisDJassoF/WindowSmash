/* The room's mods on the network: see mod_sync.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include "mod_sync.h"
#include "mod_sha256.h"
#include "mod_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ModNet net;
void mod_sync_init(const ModNet *n) { net = *n; }

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static unsigned get16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

static uint64_t random64(void) {
#ifdef _WIN32
    unsigned a = 0, b = 0;
    rand_s(&a);
    rand_s(&b);
    return (uint64_t)a << 32 | b;
#else
    uint64_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&v, sizeof v, 1, f) != 1) v = 0; fclose(f); }
    return v ? v : mod_now_ms() * 0x9E3779B97F4A7C15ull;
#endif
}

#define MODQ_SIZE 13
#define MODLIST_HEAD (3 + 8 + 8 + 8 + 2 + 2)

/* ---- the host ------------------------------------------------------------------------------------ */
static ModLock host_lock = MOD_LOCK_INIT;
static ModWake host_wake = MOD_WAKE_INIT;
static ModHost host;
static int hosting, ready, building;
static unsigned generation;
static Manifest man;
static uint8_t *enc;
static size_t enc_n;
static uint64_t enc_hash;
static uint64_t given, paused_gets;

static void build_thread(void *unused) {
    (void)unused;
    for (;;) {
        mod_lock(&host_lock);
        ModHost h = host;
        unsigned gen = generation;
        mod_unlock(&host_lock);
        ModList *l = calloc(1, sizeof *l);
        Manifest m;
        uint8_t *b = NULL;
        size_t n = 0;
        int ok = 0;
        if (l) {
            mod_scan(l, h.mods);
            ManiOptions o = {h.host_id, h.name, h.lobby, h.share, h.hash_cache[0] ? h.hash_cache : NULL};
            ok = mani_build(&m, l, &o) && mani_encode(&m, &b, &n);
            if (!ok) mani_free(&m);
            mod_list_free(l);
            free(l);
        }
        mod_lock(&host_lock);
        if (gen != generation) {   /* (asked again meanwhile: made again) */
            mod_unlock(&host_lock);
            if (ok) { mani_free(&m); free(b); }
            continue;
        }
        if (ok) {
            mani_free(&man);
            free(enc);
            man = m, enc = b, enc_n = n, enc_hash = mani_hash(b, n);
            ready = 1;
            int files = 0, gameplay = 0;
            for (int i = 0; i < man.nmods; i++) files += man.mods[i].flags & MM_FILES ? 1 : 0, gameplay += man.mods[i].flags & MM_GAMEPLAY ? 1 : 0;
            fprintf(stderr, "[mods] room manifest (%s): %d mod%s (%d that change how the game plays), %d with files, %d files, %llu MB, %zu bytes\n",
                    h.lobby ? "online lobby" : "private room", man.nmods, man.nmods == 1 ? "" : "s", gameplay, files, man.nfiles,
                    (unsigned long long)(man.bytes >> 20), enc_n);
        } else fprintf(stderr, "[mods] cannot make the room's manifest (out of memory)\n");
        building = 0;
        mod_wake(&host_wake);
        mod_unlock(&host_lock);
        return;
    }
}

static int same_host(const ModHost *a, const ModHost *b) {
    return a->lobby == b->lobby && a->host_id == b->host_id && !strcmp(a->name, b->name) && a->share == b->share &&
           !strcmp(a->mods, b->mods) && !strcmp(a->hash_cache, b->hash_cache);
}

static void serve_thread(void *unused);

void mod_host_set(const ModHost *h) {
    static int serving;
    mod_lock(&host_lock);
    if (!h) { hosting = 0; mod_unlock(&host_lock); return; }
    int again = !hosting || !same_host(&host, h) || (!ready && !building);
    host = *h;
    hosting = 1;
    if (again) {
        ready = 0;
        generation++;
        if (!building) { building = 1; if (!mod_thread(build_thread, NULL)) building = 0; }
    }
    if (!serving) serving = mod_thread(serve_thread, NULL);
    mod_unlock(&host_lock);
}

int mod_host_ready(void) {
    mod_lock(&host_lock);
    int r = hosting && ready;
    mod_unlock(&host_lock);
    return r;
}

void mod_host_stats(uint64_t *bytes, uint64_t *paused) {
    mod_lock(&host_lock);
    if (bytes) *bytes = given;
    if (paused) *paused = paused_gets;
    mod_unlock(&host_lock);
}

static void answer_query(uint32_t src, const uint8_t *p, size_t n) {
    if (n < MODQ_SIZE) return;
    uint64_t nonce = get64(p + 3);
    unsigned from = get16(p + 11);
    mod_lock(&host_lock);
    if (!hosting || (host.may_ask && !host.may_ask(src))) { mod_unlock(&host_lock); return; }
    uint8_t out[MODLIST_HEAD + MODLIST_CHUNK];
    out[0] = 'M', out[1] = 'L', out[2] = 1;
    put64(out + 3, nonce);
    put64(out + 11, host.host_id);
    if (!ready) {   /* (being made: who asks waits) */
        put64(out + 19, 0);
        put16(out + 27, 0);
        put16(out + 29, 0);
        mod_unlock(&host_lock);
        net.data(src, out, MODLIST_HEAD);
        return;
    }
    size_t total = (enc_n + MODLIST_CHUNK - 1) / MODLIST_CHUNK;
    put64(out + 19, enc_hash);
    put16(out + 29, (unsigned)total);
    uint8_t (*copy)[MODLIST_HEAD + MODLIST_CHUNK] = malloc(sizeof *copy * MODQ_WINDOW);   /* (sent once the lock is let go) */
    size_t sizes[MODQ_WINDOW];
    if (!copy) { mod_unlock(&host_lock); return; }
    int k = 0;
    for (size_t c = from; c < total && k < MODQ_WINDOW; c++, k++) {
        size_t at = c * MODLIST_CHUNK, len = enc_n - at < MODLIST_CHUNK ? enc_n - at : MODLIST_CHUNK;
        put16(out + 27, (unsigned)c);
        memcpy(copy[k], out, MODLIST_HEAD);
        memcpy(copy[k] + MODLIST_HEAD, enc + at, len);
        sizes[k] = MODLIST_HEAD + len;
    }
    mod_unlock(&host_lock);
    for (int i = 0; i < k; i++) net.data(src, copy[i], sizes[i]);
    free(copy);
}

/* MODGETs waiting for the server's thread */
typedef struct Get { ModRoute to; uint64_t sha; uint32_t first; unsigned count; } Get;
#define GETS 128
static Get gets[GETS];
static unsigned gets_head, gets_tail;

void mod_sync_get(const ModRoute *from, const uint8_t *p, size_t n) {
    if (n < 14) return;
    Get g = {*from, get64(p), get32(p + 8), get16(p + 12)};
    if (!g.count || g.count > MOD_WINDOW) return;
    mod_lock(&host_lock);
    if (!hosting) { mod_unlock(&host_lock); return; }
    int dup = 0;
    for (unsigned i = gets_tail; i != gets_head && !dup; i++) {
        const Get *q = &gets[i % GETS];
        dup = q->sha == g.sha && q->first == g.first && q->to.ip == g.to.ip && q->to.port == g.to.port;
    }
    if (!dup && gets_head - gets_tail < GETS) gets[gets_head++ % GETS] = g;
    mod_wake(&host_wake);
    mod_unlock(&host_lock);
}

static void serve_thread(void *unused) {
    (void)unused;
    FILE *f = NULL;
    char open_path[4096] = "";
    double tokens = 0;
    uint64_t last = mod_now_ms();
    for (;;) {
        mod_lock(&host_lock);
        while (gets_head == gets_tail) mod_wait(&host_wake, &host_lock, 1000);
        Get g = gets[gets_tail++ % GETS];
        long rate = host.rate > 0 ? host.rate : 4L << 20;
        int ok = hosting && ready, gameplay = 0;
        char path[4096] = "";
        uint32_t size = 0;
        if (ok) {
            int mi = -1;
            const ManiFile *mf = mani_find_file(&man, g.sha, &mi);
            if (mf && mf->src) {
                snprintf(path, sizeof path, "%s", mf->src);
                size = mf->size;
                gameplay = (man.mods[mi].flags & MM_GAMEPLAY) != 0;
            }
            if (gameplay && host.lobby) path[0] = 0;   /* (never in the lobby: the manifest has none, and this says so twice) */
        }
        mod_unlock(&host_lock);
        uint8_t out[8 + 4 + MOD_BLOCK];
        put64(out, g.sha);
        if (!ok) continue;
        if (!path[0]) { put32(out + 8, MOD_NOT_HERE); net.part(&g.to, out, 12); continue; }
        if (net.fight && net.fight()) {   /* (a fight: not a byte meanwhile - who asks waits) */
            put32(out + 8, MOD_PAUSED);
            net.part(&g.to, out, 12);
            mod_lock(&host_lock);
            paused_gets++;
            mod_unlock(&host_lock);
            continue;
        }
        if (strcmp(path, open_path)) {
            if (f) fclose(f);
            f = fopen(path, "rb");
            snprintf(open_path, sizeof open_path, "%s", f ? path : "");
        }
        if (!f) { put32(out + 8, MOD_NOT_HERE); net.part(&g.to, out, 12); continue; }
        uint32_t blocks = (size + MOD_BLOCK - 1) / MOD_BLOCK;
        for (uint32_t b = g.first; b < g.first + g.count && b < blocks; b++) {
            size_t len = size - (size_t)b * MOD_BLOCK < MOD_BLOCK ? size - (size_t)b * MOD_BLOCK : MOD_BLOCK;
            if (fseek(f, (long)b * MOD_BLOCK, SEEK_SET) || fread(out + 12, 1, len, f) != len) break;
            for (;;) {   /* (room_mods_rate: a bucket of a tenth of a second's bytes) */
                uint64_t t = mod_now_ms();
                tokens += (double)(t - last) * rate / 1000.0;
                last = t;
                if (tokens > rate / 10.0) tokens = rate / 10.0;
                if (tokens >= (double)len) break;
                mod_sleep(2);
            }
            tokens -= (double)len;
            put32(out + 8, b);
            net.part(&g.to, out, 12 + len);
            mod_lock(&host_lock);
            given += len;
            mod_unlock(&host_lock);
        }
    }
}

/* ---- who joins: the manifest -------------------------------------------------------------------- */
static ModLock ask_lock = MOD_LOCK_INIT;
static int ask_state = MQ_IDLE, ask_lobby, ask_running;
static uint32_t ask_host, ask_from;   /* asked; who answers (a private room: the first one that does) */
static uint64_t ask_nonce, ask_hash, ask_id, ask_heard;
static unsigned ask_total, ask_have;
static uint8_t *ask_buf, *ask_got;
static size_t ask_last_len;
static Manifest ask_man;
static char ask_err[160];
static unsigned ask_gen;

static void ask_reset_chunks(void) {
    free(ask_buf);
    free(ask_got);
    ask_buf = ask_got = NULL;
    ask_total = ask_have = 0;
    ask_last_len = 0;
}

static unsigned first_missing(void) {
    if (!ask_total) return 0;
    for (unsigned c = 0; c < ask_total; c++) if (!ask_got[c]) return c;
    return ask_total;
}

static void ask_thread(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    uint64_t started = mod_now_ms(), sent = 0;
    for (;;) {
        mod_lock(&ask_lock);
        if (gen != ask_gen || ask_state != MQ_ASKING) { ask_running = 0; mod_unlock(&ask_lock); return; }
        uint64_t t = mod_now_ms();
        if (t - (ask_heard ? ask_heard : started) > MODQ_GIVE_UP_MS) {
            ask_state = MQ_NONE;
            fprintf(stderr, "[mods] no room manifest from the host (%s)\n", ask_heard ? "it stopped answering" : "an older game, or none to give");
            ask_running = 0;
            mod_unlock(&ask_lock);
            return;
        }
        uint8_t q[MODQ_SIZE];
        int send = t - sent >= 400;
        if (send) {
            q[0] = 'M', q[1] = 'Q', q[2] = 1;
            put64(q + 3, ask_nonce);
            put16(q + 11, first_missing());
            sent = t;
        }
        uint32_t to = ask_from ? ask_from : ask_host;
        mod_unlock(&ask_lock);
        if (send) net.data(to, q, sizeof q);
        mod_sleep(20);
    }
}

void mod_ask(uint32_t host_ip, int lobby) {
    mod_lock(&ask_lock);
    ask_gen++;
    ask_reset_chunks();
    mani_free(&ask_man);
    ask_state = MQ_ASKING;
    ask_host = host_ip, ask_from = 0, ask_lobby = lobby;
    ask_nonce = random64();
    ask_hash = ask_id = ask_heard = 0;
    ask_err[0] = 0;
    unsigned gen = ask_gen;
    mod_unlock(&ask_lock);
    if (!mod_thread(ask_thread, (void *)(uintptr_t)gen)) {
        mod_lock(&ask_lock);
        ask_state = MQ_NONE;
        mod_unlock(&ask_lock);
    }
}

void mod_ask_stop(void) {
    mod_lock(&ask_lock);
    ask_gen++;
    ask_state = MQ_IDLE;
    ask_reset_chunks();
    mani_free(&ask_man);
    mod_unlock(&ask_lock);
}

static Manifest copy_manifest(const Manifest *m) {
    Manifest c = *m;
    c.files = calloc((size_t)(m->nfiles ? m->nfiles : 1), sizeof *c.files);
    if (c.files) memcpy(c.files, m->files, sizeof *c.files * (size_t)m->nfiles);
    else c.nfiles = 0, c.nmods = 0;
    for (int i = 0; i < c.nfiles; i++) c.files[i].src = NULL;
    return c;
}

int mod_ask_state(Manifest *m, uint64_t *hash, uint32_t *from, char *err, size_t en) {
    mod_lock(&ask_lock);
    int s = ask_state;
    if (s == MQ_GOT) {
        if (m) *m = copy_manifest(&ask_man);
        if (hash) *hash = ask_hash;
        if (from) *from = ask_from;
    }
    if (err && en) snprintf(err, en, "%s", ask_err);
    mod_unlock(&ask_lock);
    return s;
}

static void got_chunk(uint32_t src, const uint8_t *p, size_t n) {
    if (n < MODLIST_HEAD) return;
    uint64_t nonce = get64(p + 3), id = get64(p + 11), hash = get64(p + 19);
    unsigned chunk = get16(p + 27), total = get16(p + 29);
    size_t len = n - MODLIST_HEAD;
    mod_lock(&ask_lock);
    if (ask_state != MQ_ASKING || nonce != ask_nonce || (ask_from && src != ask_from) ||
        (!ask_from && ask_lobby && src != ask_host)) { mod_unlock(&ask_lock); return; }
    ask_from = src;
    ask_heard = mod_now_ms();
    if (!total) { mod_unlock(&ask_lock); return; }   /* (being made) */
    if (len > MODLIST_CHUNK || chunk >= total || (chunk + 1 < total && len != MODLIST_CHUNK) || !len) { mod_unlock(&ask_lock); return; }
    if (ask_total && (hash != ask_hash || id != ask_id || total != ask_total)) ask_reset_chunks();   /* (the host's mods changed meanwhile) */
    if (!ask_total) {
        ask_buf = malloc((size_t)total * MODLIST_CHUNK);
        ask_got = calloc(total, 1);
        if (!ask_buf || !ask_got) {
            ask_reset_chunks();
            ask_state = MQ_BAD;
            snprintf(ask_err, sizeof ask_err, "out of memory");
            mod_unlock(&ask_lock);
            return;
        }
        ask_total = total, ask_hash = hash, ask_id = id;
    }
    if (!ask_got[chunk]) {
        memcpy(ask_buf + (size_t)chunk * MODLIST_CHUNK, p + MODLIST_HEAD, len);
        ask_got[chunk] = 1;
        ask_have++;
        if (chunk + 1 == total) ask_last_len = len;
    }
    if (ask_have == ask_total) {
        size_t size = (size_t)(ask_total - 1) * MODLIST_CHUNK + ask_last_len;
        Manifest m;
        char err[160];
        if (mani_hash(ask_buf, size) != ask_hash) {
            ask_state = MQ_BAD;
            snprintf(ask_err, sizeof ask_err, "the manifest does not match its hash");
        } else if (!mani_parse(&m, ask_buf, size, ask_lobby, err, sizeof err)) {
            ask_state = MQ_BAD;
            snprintf(ask_err, sizeof ask_err, "%s", err);
        } else if (m.host_id != ask_id) {
            mani_free(&m);
            ask_state = MQ_BAD;
            snprintf(ask_err, sizeof ask_err, "the manifest is another host's");
        } else {
            ask_man = m;
            ask_state = MQ_GOT;
            int dropped = 0;
            for (int i = 0; i < m.nmods; i++) dropped += m.mods[i].why[0] != 0;
            fprintf(stderr, "[mods] room manifest from %s: %d mod%s, %d file%s, %llu MB%s\n", m.host, m.nmods, m.nmods == 1 ? "" : "s", m.nfiles,
                    m.nfiles == 1 ? "" : "s", (unsigned long long)(m.bytes >> 20), dropped ? " (some refused: see below)" : "");
            for (int i = 0; i < m.nmods; i++)
                if (m.mods[i].why[0]) fprintf(stderr, "[mods]   %s: refused - %s\n", m.mods[i].name, m.mods[i].why);
        }
        if (ask_state == MQ_BAD) fprintf(stderr, "[mods] the room's manifest was refused: %s\n", ask_err);
        ask_reset_chunks();
    }
    mod_unlock(&ask_lock);
}

void mod_sync_data(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 3 || p[0] != 'M' || p[2] != 1) return;
    if (p[1] == 'Q') answer_query(src, p, n);
    else if (p[1] == 'L') got_chunk(src, p, n);
}

void mod_sync_part(uint32_t src, const uint8_t *p, size_t n) { mod_fetch_part(src, p, n); }
int mod_sync_send_get(uint32_t to, const void *p, size_t n) { return net.get ? net.get(to, p, n) : 0; }
int mod_sync_reachable(uint32_t to) { return !net.reachable || net.reachable(to); }
