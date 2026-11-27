/* The room's manifest: see mod_manifest.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "mod_manifest.h"
#include "mod_sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#endif

static const char MAGIC[4] = {'S', 'N', 'M', 'F'};

/* ---- what a room's mod may hold ----------------------------------------------------------------- */
int mani_path_ok(const char *p) {
    /* data.arc's own top folders (prebuilt; holds plugins: not one of them) */
    static const char *const TOP[] = {"assist", "boss", "camera", "common", "effect", "enemy", "fighter", "finalsmash", "item",
                                      "miihat", "param", "pokemon", "render", "snapshot", "sound", "spirits", "stage", "standard", "ui"};
    static const char *const CODE[] = {".nro", ".lua", ".luac", ".dll", ".so", ".exe", ".dylib", ".bat", ".cmd", ".sh", ".ps1", ".py", ".js"};
    size_t len = strlen(p);
    if (!strcmp(p, MANI_CONFIG)) return 1;
    if (!len || len >= MANI_PATH || *p == '/' || p[len - 1] == '/') return 0;
    for (const char *c = p; *c; c++)
        if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_' || *c == '.' || *c == '-' || *c == '+' || *c == '/')) return 0;
    for (const char *seg = p; *seg;) {   /* no empty, "." or ".." part */
        const char *end = strchr(seg, '/');
        size_t n = end ? (size_t)(end - seg) : strlen(seg);
        if (!n || seg[0] == '.') return 0;   /* (no ".", "..", nor a hidden file: .DS_Store...) */
        seg += n + (end != NULL);
    }
    const char *slash = strchr(p, '/');
    if (!slash) return 0;   /* (a file at the top: the mod's own, never data.arc's) */
    int top = 0;
    for (size_t i = 0; i < sizeof TOP / sizeof TOP[0] && !top; i++) top = strlen(TOP[i]) == (size_t)(slash - p) && !memcmp(TOP[i], p, slash - p);
    if (!top) return 0;
    const char *dot = strrchr(p, '.');
    if (dot && dot > strrchr(p, '/'))
        for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++) if (!strcmp(dot, CODE[i])) return 0;
    return 1;
}

static int by_path(const void *a, const void *b) { return strcmp((*(const ManiFile *const *)a)->path, (*(const ManiFile *const *)b)->path); }

uint64_t mani_mod_id(const ManiFile *files, int n) {
    const ManiFile **order = malloc(sizeof *order * (size_t)(n ? n : 1));
    if (!order) return 0;
    for (int i = 0; i < n; i++) order[i] = &files[i];
    qsort(order, (size_t)n, sizeof *order, by_path);
    Sha256 s;
    sha256_init(&s);
    for (int i = 0; i < n; i++) {
        sha256_update(&s, order[i]->path, strlen(order[i]->path) + 1);
        sha256_update(&s, order[i]->sha, 32);
    }
    free(order);
    uint8_t h[32];
    sha256_final(&s, h);
    uint64_t id = sha256_u64(h);
    return id ? id : 1;
}

uint64_t mani_hash(const uint8_t *bytes, size_t n) {
    uint8_t h[32];
    sha256(bytes, n, h);
    return sha256_u64(h);
}

void mani_free(Manifest *m) {
    for (int i = 0; i < m->nfiles; i++) free(m->files[i].src);
    free(m->files);
    memset(m, 0, sizeof *m);
}

const ManiFile *mani_find_file(const Manifest *m, uint64_t sha64, int *mod) {
    for (int i = 0; i < m->nmods; i++) {
        const ManiMod *x = &m->mods[i];
        if (!(x->flags & MM_FILES) || x->why[0]) continue;
        for (int k = 0; k < x->nfiles; k++)
            if (sha256_u64(m->files[x->first + k].sha) == sha64) { if (mod) *mod = i; return &m->files[x->first + k]; }
    }
    return NULL;
}

/* ---- the host's: the hashes, made once ------------------------------------------------------------ */
typedef struct HashEntry { char *path; uint64_t size, mtime; uint8_t sha[32]; int used; } HashEntry;
typedef struct HashCache { HashEntry *e; int n, cap, changed; } HashCache;

static int by_entry(const void *a, const void *b) { return strcmp(((const HashEntry *)a)->path, ((const HashEntry *)b)->path); }

static void cache_add(HashCache *c, const char *path, uint64_t size, uint64_t mtime, const uint8_t sha[32]) {
    if (c->n == c->cap) {
        HashEntry *e = realloc(c->e, sizeof *e * (size_t)(c->cap = c->cap ? c->cap * 2 : 256));
        if (!e) return;
        c->e = e;
    }
    HashEntry *h = &c->e[c->n];
    if (!(h->path = strdup(path))) return;
    h->size = size, h->mtime = mtime, h->used = 1;
    memcpy(h->sha, sha, 32);
    c->n++;
}

static void cache_read(HashCache *c, const char *file) {
    FILE *f = file ? fopen(file, "rb") : NULL;
    if (!f) return;
    char line[4200];
    while (fgets(line, sizeof line, f)) {   /* <sha256> <size> <mtime ns> <path> */
        uint8_t sha[32];
        unsigned long long size, mtime;
        int at = 0;
        if (!sha256_parse(line, sha) || sscanf(line + 64, " %llu %llu %n", &size, &mtime, &at) < 2 || !at) continue;
        char *path = line + 64 + at;
        path[strcspn(path, "\r\n")] = 0;
        if (*path) cache_add(c, path, size, mtime, sha);
    }
    fclose(f);
    for (int i = 0; i < c->n; i++) c->e[i].used = 0;
    qsort(c->e, (size_t)c->n, sizeof *c->e, by_entry);
}

static void cache_write(const HashCache *c, const char *file) {
    if (!file || !c->changed) return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "# The SHA-256 of the mods' files, for the rooms hosted here (made again when a file changes)\n");
    for (int i = 0; i < c->n; i++) {
        if (!c->e[i].used) continue;   /* (files gone) */
        char hex[65];
        sha256_hex(c->e[i].sha, hex);
        fprintf(f, "%s %llu %llu %s\n", hex, (unsigned long long)c->e[i].size, (unsigned long long)c->e[i].mtime, c->e[i].path);
    }
    int ok = !ferror(f);
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, file)) remove(tmp);
}

static void cache_free(HashCache *c) {
    for (int i = 0; i < c->n; i++) free(c->e[i].path);
    free(c->e);
}

/* The file's hash: the cache's while its size and time are the same, else read */
static int file_hash(HashCache *c, int sorted, const char *path, uint64_t size, uint64_t mtime, uint8_t sha[32]) {
    if (sorted) {
        HashEntry key = {(char *)path, 0, 0, {0}, 0};
        HashEntry *h = bsearch(&key, c->e, (size_t)sorted, sizeof *c->e, by_entry);
        if (h && h->size == size && h->mtime == mtime) { memcpy(sha, h->sha, 32); h->used = 1; return 1; }
        if (h) h->used = 0;   /* (changed: the new one added) */
    }
    uint64_t got = 0;
    if (!sha256_file(path, sha, &got) || got != size) return 0;
    cache_add(c, path, size, mtime, sha);
    c->changed = 1;
    return 1;
}

static void copy_text(char *dst, size_t n, const char *src) {   /* (printable ASCII and UTF-8: what the overlay's mod_ascii takes) */
    size_t o = 0;
    for (; *src && o + 1 < n; src++) dst[o++] = (unsigned char)*src < 32 || *src == 127 ? '?' : *src;
    dst[o] = 0;
}

#ifndef _WIN32
/* the mod's config.json, whatever its case */
static int find_config(const ModList *l, const ModInfo *x, char *out, size_t n) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/%s", l->root, x->dir);
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    while (d && !found && (e = readdir(d))) if (!strcasecmp(e->d_name, MANI_CONFIG)) { snprintf(out, n, "%s/%s", dir, e->d_name); found = 1; }
    if (d) closedir(d);
    return found;
}

static uint64_t mtime_ns(const struct stat *st) { return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec; }
#endif

int mani_build(Manifest *m, const ModList *l, const ManiOptions *o) {
    memset(m, 0, sizeof *m);
    m->host_id = o->host_id;
    copy_text(m->host, sizeof m->host, o->host ? o->host : "");
    m->lobby = o->lobby;
#ifndef _WIN32
    HashCache cache = {0};
    cache_read(&cache, o->hash_cache);
    int sorted = cache.n, cap = 0;
    for (int i = 0; i < l->n; i++) {
        const ModInfo *x = &l->mods[i];
        if (!x->enabled) continue;
        int gameplay = x->gameplay;
        int files = gameplay ? o->share && !x->no_share && !o->lobby : o->share && !x->no_share;
        if (!gameplay && !files) continue;   /* (a visual mod not shared: not told at all) */
        if (m->nmods == MANI_MODS) { fprintf(stderr, "[mods] rooms: more than %d mods; %s and the ones after it are not announced\n", MANI_MODS, x->key); break; }
        /* its files (all of them: the id is theirs, given or not) */
        int first = m->nfiles, bad = 0, skipped = 0;
        uint64_t bytes = 0;
        for (int k = 0; k < x->npaths && !bad; k++) {
            const char *p = x->paths[k];
            if (!strncmp(p, "stream;", 7) || !strncmp(p, "stream:", 7)) continue;
            if (!mani_path_ok(p)) { if (!skipped++) fprintf(stderr, "[mods] rooms: %s: %s is not given (not a data.arc file)\n", x->key, p); continue; }
            char src[4096];
            snprintf(src, sizeof src, "%s/%s/%s", l->root, x->dir, x->real[k]);
            struct stat st;
            if (stat(src, &st) || !S_ISREG(st.st_mode) || !st.st_size) continue;
            if ((uint64_t)st.st_size > MANI_FILE_MAX) {
                fprintf(stderr, "[mods] rooms: %s: %s is %llu MB, over %u MB a file; the mod is not announced\n", x->key, p,
                        (unsigned long long)st.st_size >> 20, MANI_FILE_MAX >> 20);
                bad = 1;
                break;
            }
            if (m->nfiles == cap) {
                ManiFile *f = realloc(m->files, sizeof *f * (size_t)(cap = cap ? cap * 2 : 256));
                if (!f) { bad = 2; break; }
                m->files = f;
            }
            ManiFile *f = &m->files[m->nfiles];
            memset(f, 0, sizeof *f);
            snprintf(f->path, sizeof f->path, "%s", p);
            f->size = (uint32_t)st.st_size;
            if (!file_hash(&cache, sorted, src, (uint64_t)st.st_size, mtime_ns(&st), f->sha)) {
                fprintf(stderr, "[mods] rooms: %s: cannot read %s; the mod is not announced\n", x->key, src);
                bad = 1;
                break;
            }
            f->src = strdup(src);
            m->nfiles++;
            bytes += f->size;
        }
        char config[4352];
        if (!bad && x->has_config && find_config(l, x, config, sizeof config)) {   /* (its config.json: where its new files go) */
            struct stat st;
            if (!stat(config, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= MANI_CONFIG_MAX) {
                if (m->nfiles == cap) {
                    ManiFile *g = realloc(m->files, sizeof *g * (size_t)(cap = cap ? cap * 2 : 256));
                    if (g) m->files = g;
                }
                ManiFile *f = m->nfiles < cap ? &m->files[m->nfiles] : NULL;
                if (f) {
                    memset(f, 0, sizeof *f);
                    snprintf(f->path, sizeof f->path, MANI_CONFIG);
                    f->size = (uint32_t)st.st_size;
                    if (file_hash(&cache, sorted, config, (uint64_t)st.st_size, mtime_ns(&st), f->sha) && (f->src = strdup(config))) {
                        m->nfiles++;
                        bytes += f->size;
                    }
                }
            }
        }
        int n = m->nfiles - first;
        if (!bad && files && (m->nfiles > MANI_FILES || m->bytes + bytes > MANI_TOTAL)) {
            fprintf(stderr, "[mods] rooms: %s: over %d files or %llu GB in all with the mods before it; %s\n", x->key, MANI_FILES,
                    (unsigned long long)(MANI_TOTAL >> 30), gameplay ? "told without its files" : "not announced");
            if (!gameplay) bad = 1;
            files = 0;
        }
        if (bad || !n) {   /* (nothing of it to tell) */
            for (int k = first; k < m->nfiles; k++) free(m->files[k].src);
            m->nfiles = first;
            if (bad == 2) break;
            continue;
        }
        ManiMod *y = &m->mods[m->nmods++];
        memset(y, 0, sizeof *y);
        y->id = mani_mod_id(m->files + first, n);
        y->flags = (uint8_t)((gameplay ? MM_GAMEPLAY : 0) | (files ? MM_FILES : 0));
        copy_text(y->name, sizeof y->name, x->name);
        copy_text(y->version, sizeof y->version, x->version);
        snprintf(y->fighter, sizeof y->fighter, "%s", x->fighter);
        memcpy(y->to, x->to, sizeof y->to);
        if (!gameplay && x->url[0] && sha256_parse(x->url_sha256, y->url_sha)) {
            snprintf(y->url, sizeof y->url, "%s", x->url);
            y->flags |= MM_URL;
        }
        if (files) {
            y->first = first, y->nfiles = n, y->bytes = bytes;
            m->bytes += bytes;
        } else {   /* (told by its id alone: its files not kept) */
            for (int k = first; k < m->nfiles; k++) free(m->files[k].src);
            m->nfiles = first;
        }
    }
    for (int i = 0; i < cache.n; i++) cache.changed |= !cache.e[i].used;
    cache_write(&cache, o->hash_cache);
    cache_free(&cache);
#else
    (void)l;
#endif
    return 1;
}

int mani_hash_all(const ModList *l, const char *hash_cache, ManiFile **out) {
    *out = NULL;
    int n = 0;
#ifndef _WIN32
    HashCache cache = {0};
    cache_read(&cache, hash_cache);
    int sorted = cache.n, cap = 0;
    ManiFile *v = NULL;
    for (int i = 0; i < l->n; i++) {
        const ModInfo *x = &l->mods[i];
        for (int k = 0; k < x->npaths; k++) {
            if (!mani_path_ok(x->paths[k])) continue;
            char src[4096];
            snprintf(src, sizeof src, "%s/%s/%s", l->root, x->dir, x->real[k]);
            struct stat st;
            if (stat(src, &st) || !S_ISREG(st.st_mode) || !st.st_size || (uint64_t)st.st_size > MANI_FILE_MAX) continue;
            if (n == cap) {
                ManiFile *g = realloc(v, sizeof *g * (size_t)(cap = cap ? cap * 2 : 256));
                if (!g) break;
                v = g;
            }
            ManiFile *f = &v[n];
            memset(f, 0, sizeof *f);
            snprintf(f->path, sizeof f->path, "%s", x->paths[k]);
            f->size = (uint32_t)st.st_size;
            if (!file_hash(&cache, sorted, src, (uint64_t)st.st_size, mtime_ns(&st), f->sha) || !(f->src = strdup(src))) continue;
            n++;
        }
    }
    for (int i = 0; i < sorted; i++) cache.e[i].used = 1;   /* (the manifest's own pass keeps it tidy: nothing dropped here) */
    cache_write(&cache, hash_cache);
    cache_free(&cache);
    *out = v;
#else
    (void)l; (void)hash_cache;
#endif
    return n;
}

void mani_free_files(ManiFile *files, int n) {
    for (int i = 0; i < n; i++) free(files[i].src);
    free(files);
}

/* ---- the encoding ------------------------------------------------------------------------------- */
typedef struct Out { uint8_t *p; size_t n, cap; int bad; } Out;
static void put(Out *o, const void *b, size_t n) {
    if (o->bad) return;
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->n + n) cap *= 2;
        uint8_t *p = realloc(o->p, cap);
        if (!p) { o->bad = 1; return; }
        o->p = p, o->cap = cap;
    }
    memcpy(o->p + o->n, b, n);
    o->n += n;
}
static void put8(Out *o, unsigned v) { uint8_t b = (uint8_t)v; put(o, &b, 1); }
static void put16(Out *o, unsigned v) { uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; put(o, b, 2); }
static void put32(Out *o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }
static void put64(Out *o, uint64_t v) { put32(o, (uint32_t)(v >> 32)); put32(o, (uint32_t)v); }
static void put_text(Out *o, const char *s, size_t most) { size_t n = strlen(s); if (n > most) n = most; put8(o, (unsigned)n); put(o, s, n); }

int mani_encode(const Manifest *m, uint8_t **out, size_t *n) {
    Out o = {0};
    put(&o, MAGIC, 4);
    put8(&o, MANI_VERSION);
    put64(&o, m->host_id);
    char name[MANI_NAME] = {0};
    strncpy(name, m->host, sizeof name);
    put(&o, name, sizeof name);
    put8(&o, m->lobby ? 1 : 0);
    put8(&o, (unsigned)m->nmods);
    int nfiles = 0;
    for (int i = 0; i < m->nmods; i++) nfiles += m->mods[i].flags & MM_FILES ? m->mods[i].nfiles : 0;
    put16(&o, (unsigned)nfiles);
    for (int i = 0; i < m->nmods; i++) {
        const ManiMod *x = &m->mods[i];
        put64(&o, x->id);
        put8(&o, x->flags);
        put_text(&o, x->name, sizeof x->name - 1);
        put_text(&o, x->version, sizeof x->version - 1);
        put_text(&o, x->fighter, sizeof x->fighter - 1);
        put(&o, x->to, MOD_COLORS);
        put16(&o, x->flags & MM_FILES ? (unsigned)x->nfiles : 0);
        if (x->flags & MM_URL) {
            size_t ul = strlen(x->url);
            put16(&o, (unsigned)ul);
            put(&o, x->url, ul);
            put(&o, x->url_sha, 32);
        }
    }
    for (int i = 0; i < m->nmods; i++) {
        const ManiMod *x = &m->mods[i];
        if (!(x->flags & MM_FILES)) continue;
        for (int k = 0; k < x->nfiles; k++) {
            const ManiFile *f = &m->files[x->first + k];
            put_text(&o, f->path, MANI_PATH - 1);
            put32(&o, f->size);
            put(&o, f->sha, 32);
        }
    }
    if (o.bad) { free(o.p); return 0; }
    *out = o.p;
    *n = o.n;
    return 1;
}

/* ---- reading one ------------------------------------------------------------------------------ */
typedef struct In { const uint8_t *p; size_t n, at; int bad; } In;
static const uint8_t *take(In *i, size_t n) {
    if (i->bad || n > i->n - i->at) { i->bad = 1; return NULL; }
    const uint8_t *p = i->p + i->at;
    i->at += n;
    return p;
}
static unsigned get8(In *i) { const uint8_t *p = take(i, 1); return p ? p[0] : 0; }
static unsigned get16(In *i) { const uint8_t *p = take(i, 2); return p ? (unsigned)(p[0] << 8 | p[1]) : 0; }
static uint32_t get32(In *i) { uint32_t a = get16(i); return a << 16 | get16(i); }
static uint64_t get64(In *i) { uint64_t a = get32(i); return a << 32 | get32(i); }
static int get_text(In *i, char *out, size_t size) {   /* 0: longer than out takes */
    unsigned n = get8(i);
    const uint8_t *p = take(i, n);
    if (!p || n >= size) { out[0] = 0; return 0; }
    for (unsigned k = 0; k < n; k++) out[k] = p[k] < 32 || p[k] == 127 ? '?' : (char)p[k];
    out[n] = 0;
    return 1;
}

static void drop(ManiMod *x, const char *fmt, const char *what) {
    if (x->why[0]) return;
    snprintf(x->why, sizeof x->why, fmt, what);
}

int mani_parse(Manifest *m, const uint8_t *bytes, size_t n, int lobby, char *err, size_t en) {
    memset(m, 0, sizeof *m);
    In in = {bytes, n, 0, 0};
    const uint8_t *magic = take(&in, 4);
    if (!magic || memcmp(magic, MAGIC, 4)) { snprintf(err, en, "not a room manifest"); return 0; }
    unsigned version = get8(&in);
    if (version != MANI_VERSION) { snprintf(err, en, "manifest version %u (this game reads %d)", version, MANI_VERSION); return 0; }
    m->host_id = get64(&in);
    const uint8_t *name = take(&in, MANI_NAME);
    if (name) for (int k = 0; k < MANI_NAME && name[k]; k++) m->host[k] = name[k] < 32 || name[k] > 126 ? '?' : (char)name[k];
    unsigned flags = get8(&in);
    m->lobby = lobby || (flags & 1);   /* (a room of the lobby, whatever it says) */
    unsigned nmods = get8(&in), nfiles = get16(&in);
    if (in.bad) { snprintf(err, en, "manifest cut short"); return 0; }
    if (nmods > MANI_MODS || nfiles > MANI_FILES) { snprintf(err, en, "too many mods or files (%u, %u)", nmods, nfiles); return 0; }
    if (!m->host_id) { snprintf(err, en, "no host id"); return 0; }
    m->nmods = (int)nmods;
    unsigned listed = 0;
    for (unsigned i = 0; i < nmods && !in.bad; i++) {
        ManiMod *x = &m->mods[i];
        x->id = get64(&in);
        x->flags = (uint8_t)get8(&in);
        int ok = get_text(&in, x->name, sizeof x->name);
        ok &= get_text(&in, x->version, sizeof x->version);
        ok &= get_text(&in, x->fighter, sizeof x->fighter);
        const uint8_t *to = take(&in, MOD_COLORS);
        unsigned count = get16(&in);
        if (in.bad) break;
        if (!ok) drop(x, "a name too long%s", "");
        for (const char *c = x->fighter; *c; c++)
            if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_') drop(x, "a bad fighter name%s", "");
        for (int c = 0; c < MOD_COLORS; c++) {
            x->to[c] = (signed char)to[c];
            if (x->to[c] < -1 || x->to[c] >= MOD_COLORS) drop(x, "a color past c07%s", "");
        }
        if (x->flags & ~(MM_GAMEPLAY | MM_FILES | MM_URL)) drop(x, "flags this game does not know%s", "");
        if (!(x->flags & MM_FILES) != !count) drop(x, "a file count that does not match%s", "");
        if (!x->id) drop(x, "no id%s", "");
        x->first = (int)listed;
        x->nfiles = (int)count;
        listed += count;
        if (x->flags & MM_URL) {
            unsigned ul = get16(&in);
            const uint8_t *u = take(&in, ul), *h = take(&in, 32);
            if (in.bad) break;
            int good = ul < sizeof x->url && ul > 8 && !memcmp(u, "https://", 8) && !(x->flags & MM_GAMEPLAY);
            for (unsigned k = 0; k < ul && good; k++) good = u[k] > 32 && u[k] < 127;
            if (good) { memcpy(x->url, u, ul); x->url[ul] = 0; memcpy(x->url_sha, h, 32); }
            else x->flags &= (uint8_t)~MM_URL;   /* (not one to fetch: from the host, then) */
        }
    }
    if (in.bad) { snprintf(err, en, "manifest cut short"); return 0; }
    if (listed != nfiles) { snprintf(err, en, "the mods list %u files, the manifest %u", listed, nfiles); return 0; }
    m->files = calloc(nfiles ? nfiles : 1, sizeof *m->files);
    if (!m->files) { snprintf(err, en, "out of memory"); return 0; }
    m->nfiles = (int)nfiles;
    for (unsigned k = 0; k < nfiles && !in.bad; k++) {
        ManiFile *f = &m->files[k];
        int ok = get_text(&in, f->path, sizeof f->path);
        f->size = get32(&in);
        const uint8_t *h = take(&in, 32);
        if (h) memcpy(f->sha, h, 32);
        if (!ok) f->path[0] = 0;
    }
    if (in.bad) { mani_free(m); snprintf(err, en, "manifest cut short"); return 0; }
    if (in.at != n) { mani_free(m); snprintf(err, en, "%zu bytes past the manifest's end", n - in.at); return 0; }
    /* each mod's files: allowed, within the limits, its own id; the kind its paths say */
    for (int i = 0; i < m->nmods; i++) {
        ManiMod *x = &m->mods[i];
        uint64_t bytes = 0;
        int gameplay = 0;
        for (int k = 0; k < x->nfiles; k++) {
            const ManiFile *f = &m->files[x->first + k];
            if (!mani_path_ok(f->path)) { drop(x, "a file it may not have (%s)", f->path[0] ? f->path : "no path"); continue; }
            if (!f->size || f->size > MANI_FILE_MAX) drop(x, "a file over the size limit (%s)", f->path);
            for (int j = 0; j < k; j++) if (!strcmp(m->files[x->first + j].path, f->path)) drop(x, "a file twice (%s)", f->path);
            if (!strcmp(f->path, MANI_CONFIG) && f->size > MANI_CONFIG_MAX) drop(x, "a config.json too big (%s)", "1 MB");
            gameplay |= strcmp(f->path, MANI_CONFIG) && mod_path_kind(f->path) == MK_OTHER;
            bytes += f->size;
        }
        if (gameplay) x->flags |= MM_GAMEPLAY;   /* (what its files are, whatever it says) */
        if (gameplay) x->flags &= (uint8_t)~MM_URL;
        if (x->nfiles && !x->why[0] && mani_mod_id(m->files + x->first, x->nfiles) != x->id) drop(x, "an id that is not its files'%s", "");
        if (!x->why[0] && m->bytes + bytes > MANI_TOTAL) drop(x, "more than the room's %s in all", "4 GB");
        if (!x->why[0]) { x->bytes = bytes; m->bytes += bytes; }
        if (m->lobby && (x->flags & MM_GAMEPLAY) && (x->flags & MM_FILES)) {   /* (the lobby: never taken) */
            x->flags &= (uint8_t)~MM_FILES;
            x->bytes = 0;
            if (!x->why[0]) m->bytes -= bytes;
        }
    }
    return 1;
}
