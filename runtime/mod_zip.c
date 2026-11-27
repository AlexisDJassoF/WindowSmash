/* A room's visual mod from where it comes from (mod_fetch.h's origin): its .zip fetched over HTTPS
 * with the system's libcurl (loaded when needed: no hard dependency - without it the files come from
 * the host), checked against the SHA-256 the mod's info.toml declares, then its entries read - stored
 * or deflated, inflated here - and each one whose size and SHA-256 are a file the queue asks for put
 * in the cache. The .zip's folders do not matter: a file is known by its hash. Windows: not yet (the
 * mods folder is not read there either), so the files come from the host. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "mod_fetch.h"
#include "mod_manifest.h"
#include "mod_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

/* ---- inflate (RFC 1951), after zlib's puff.c ------------------------------------------------------ */
typedef struct Inf {
    const uint8_t *in; size_t inlen, incnt;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outcnt;
    int bad;
} Inf;

static int bits(Inf *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) { s->bad = 1; return 0; }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int stored(Inf *s) {
    s->bitbuf = 0, s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -1;
    unsigned len = s->in[s->incnt] | s->in[s->incnt + 1] << 8;
    unsigned nlen = s->in[s->incnt + 2] | s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xFFFF) || s->incnt + len > s->inlen || s->outcnt + len > s->outlen) return -1;
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->outcnt += len, s->incnt += len;
    return 0;
}

typedef struct Huff { short count[16], symbol[288]; } Huff;

static int decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        if (s->bad) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count, first += count;
        first <<= 1, code <<= 1;
    }
    return -1;
}

static int construct(Huff *h, const short *length, int n) {
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int codes(Inf *s, const Huff *lencode, const Huff *distcode) {
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                    4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0) return -1;
        if (symbol < 256) {
            if (s->outcnt == s->outlen) return -1;
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return -1;
            int len = lens[symbol] + bits(s, lext[symbol]);
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30) return -1;
            size_t dist = (size_t)(dists[symbol] + bits(s, dext[symbol]));
            if (s->bad || dist > s->outcnt || s->outcnt + (size_t)len > s->outlen) return -1;
            for (; len; len--, s->outcnt++) s->out[s->outcnt] = s->out[s->outcnt - dist];
        } else return 0;
    }
}

static int fixed(Inf *s) {
    static Huff lencode, distcode;
    static int ready;
    if (!ready) {
        short lengths[288];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, 30);
        ready = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(Inf *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    Huff lencode, distcode;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->bad || nlen > 286 || ndist > 30) return -1;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->bad || construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) return -1;
        if (symbol < 16) lengths[index++] = (short)symbol;
        else {
            int len = 0;
            if (symbol == 16) { if (!index) return -1; len = lengths[index - 1]; symbol = 3 + bits(s, 2); }
            else if (symbol == 17) symbol = 3 + bits(s, 3);
            else symbol = 11 + bits(s, 7);
            if (s->bad || index + symbol > nlen + ndist) return -1;
            while (symbol--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -1;   /* (incomplete: one code alone) */
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -1;
    return codes(s, &lencode, &distcode);
}

/* in[0, inlen) inflated into out[0, outlen): 1 when it fills it exactly */
int mod_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    Inf s = {in, inlen, 0, 0, 0, out, outlen, 0, 0};
    int last, err;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.bad) return 0;
        err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (err || s.bad) return 0;
    } while (!last);
    return s.outcnt == outlen;
}

/* ---- the .zip ------------------------------------------------------------------------------------ */
static unsigned le16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(FILE *f, long at, void *buf, size_t n) { return !fseek(f, at, SEEK_SET) && fread(buf, 1, n, f) == n; }

/* The entries of the .zip at path whose content is a file the queue asks for: put in blob_dir. How many. */
int mod_zip_extract(const char *path, const char *blob_dir, const FetchFile *files, int n, int which, char *why, size_t wn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, wn, "cannot read the .zip"); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65536 + 22 ? size : 65536 + 22;
    uint8_t *end = malloc((size_t)(tail > 0 ? tail : 1));
    int got = 0;
    if (!end || tail < 22 || !read_at(f, size - tail, end, (size_t)tail)) { snprintf(why, wn, "not a .zip"); goto out; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0 && eocd < 0; i--) if (le32(end + i) == 0x06054b50) eocd = i;
    if (eocd < 0) { snprintf(why, wn, "not a .zip"); goto out; }
    unsigned count = le16(end + eocd + 10);
    uint32_t cd_size = le32(end + eocd + 12), cd_at = le32(end + eocd + 16);
    if (cd_at == 0xFFFFFFFFu || count == 0xFFFF || (long)cd_at + (long)cd_size > size) { snprintf(why, wn, "a .zip this game cannot read (zip64)"); goto out; }
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd || !read_at(f, (long)cd_at, cd, cd_size)) { free(cd); snprintf(why, wn, "a broken .zip"); goto out; }
    size_t at = 0;
    for (unsigned e = 0; e < count && at + 46 <= cd_size; e++) {
        const uint8_t *h = cd + at;
        if (le32(h) != 0x02014b50) break;
        unsigned flags = le16(h + 8), method = le16(h + 10), nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
        uint32_t csize = le32(h + 20), usize = le32(h + 24), local = le32(h + 42);
        at += 46 + nlen + xlen + clen;
        if ((flags & 1) || (method != 0 && method != 8) || !usize || usize > MANI_FILE_MAX) continue;
        int want = -1;   /* (a file of that size the queue asks for, not here yet) */
        for (int i = 0; i < n && want < 0; i++)
            if (files[i].origin == which && files[i].size == usize && !mod_blob_has(blob_dir, files[i].sha, usize)) want = i;
        if (want < 0) continue;
        uint8_t lh[30];
        if (!read_at(f, (long)local, lh, 30) || le32(lh) != 0x04034b50) continue;
        long data = (long)local + 30 + le16(lh + 26) + le16(lh + 28);
        if (data + (long)csize > size) continue;
        uint8_t *packed = malloc(csize ? csize : 1), *plain = method == 8 ? malloc(usize) : NULL;
        int ok = packed && read_at(f, data, packed, csize) && (method == 0 ? csize == usize : plain && mod_inflate(packed, csize, plain, usize));
        const uint8_t *bytes = method == 0 ? packed : plain;
        uint8_t sha[32];
        if (ok) sha256(bytes, usize, sha);
        for (int i = 0; ok && i < n; i++) {
            if (files[i].origin != which || files[i].size != usize || memcmp(files[i].sha, sha, 32) || mod_blob_has(blob_dir, sha, usize)) continue;
            char blob[1100], part[1110];
            mod_blob_path(blob_dir, sha, blob, sizeof blob);
            snprintf(part, sizeof part, "%s.part", blob);
            FILE *o = fopen(part, "wb");
            int w = o && fwrite(bytes, 1, usize, o) == usize;
            if (o && fclose(o)) w = 0;
            remove(blob);
            if (w && !rename(part, blob)) got++;
            else remove(part);
            break;
        }
        free(packed);
        free(plain);
    }
    free(cd);
    if (!got && !why[0]) snprintf(why, wn, "none of the room's files are in it");
out:
    free(end);
    fclose(f);
    return got;
}

/* ---- HTTPS: the system's libcurl ----------------------------------------------------------------- */
#ifndef _WIN32
typedef struct { FILE *f; uint64_t n, most; } Sink;
static size_t sink(char *p, size_t size, size_t count, void *user) {
    Sink *s = user;
    size_t n = size * count;
    if (s->n + n > s->most || fwrite(p, 1, n, s->f) != n) return 0;   /* (0: libcurl stops) */
    s->n += n;
    return n;
}

static int https_get(const char *url, const char *to, char *why, size_t wn) {
    static void *lib;
    static int tried;
    if (!tried) {
        tried = 1;
        lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libcurl-gnutls.so.4", RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib) { snprintf(why, wn, "no libcurl on this system"); return 0; }
    void *(*init)(void) = (void *(*)(void))dlsym(lib, "curl_easy_init");
    int (*setopt)(void *, int, ...) = (int (*)(void *, int, ...))dlsym(lib, "curl_easy_setopt");
    int (*perform)(void *) = (int (*)(void *))dlsym(lib, "curl_easy_perform");
    void (*cleanup)(void *) = (void (*)(void *))dlsym(lib, "curl_easy_cleanup");
    const char *(*strerror_)(int) = (const char *(*)(int))dlsym(lib, "curl_easy_strerror");
    if (!init || !setopt || !perform || !cleanup) { snprintf(why, wn, "this libcurl lacks what is needed"); return 0; }
    FILE *f = fopen(to, "wb");
    if (!f) { snprintf(why, wn, "cannot write the .zip"); return 0; }
    Sink s = {f, 0, MANI_TOTAL};
    void *c = init();
    if (!c) { fclose(f); remove(to); snprintf(why, wn, "libcurl did not start"); return 0; }
    enum { URL = 10002, WRITEDATA = 10001, WRITEFUNCTION = 20011, FOLLOW = 52, MAXREDIRS = 68, FAIL = 45, CONNECT_TIMEOUT = 78,
           LOW_LIMIT = 19, LOW_TIME = 20, PROTOCOLS = 181, REDIR_PROTOCOLS = 182, USERAGENT = 10018, NOSIGNAL = 99 };
    setopt(c, URL, url);
    setopt(c, WRITEFUNCTION, sink);
    setopt(c, WRITEDATA, &s);
    setopt(c, FOLLOW, 1L);
    setopt(c, MAXREDIRS, 5L);
    setopt(c, FAIL, 1L);
    setopt(c, CONNECT_TIMEOUT, 15L);
    setopt(c, LOW_LIMIT, 1024L);   /* (slower than 1 KB/s for 30 s: given up) */
    setopt(c, LOW_TIME, 30L);
    setopt(c, PROTOCOLS, 2L);      /* (HTTPS only, redirects too) */
    setopt(c, REDIR_PROTOCOLS, 2L);
    setopt(c, NOSIGNAL, 1L);
    setopt(c, USERAGENT, "WindowSmash");
    int r = perform(c);
    cleanup(c);
    int ok = fclose(f) == 0 && r == 0;
    if (!ok) {
        remove(to);
        snprintf(why, wn, "the download failed (%s)", r && strerror_ ? strerror_(r) : "cannot write it");
    }
    return ok;
}
#else
static int https_get(const char *url, const char *to, char *why, size_t wn) {
    (void)url; (void)to;
    snprintf(why, wn, "not on Windows yet");
    return 0;
}
#endif

int mod_origin_fetch(const FetchOrigin *o, const char *blob_dir, const FetchFile *files, int n, int which, char *why, size_t wn) {
    if (strncmp(o->url, "https://", 8)) { snprintf(why, wn, "not an https address"); return 0; }
    const char *test = getenv("SSBU_MODS_ORIGIN_DIR");   /* (tests: the .zip files are there, named as the address's last part) */
    char zip[1100], hex[65];
    sha256_hex(o->sha, hex);
    snprintf(zip, sizeof zip, "%s/origin-%s.zip.part", blob_dir, hex);
    fprintf(stderr, "[mods] fetching %s\n", o->url);
    if (test && *test) {
        const char *name = strrchr(o->url, '/');
        char src[1100];
        snprintf(src, sizeof src, "%s/%s", test, name ? name + 1 : o->url);
        FILE *a = fopen(src, "rb"), *b = a ? fopen(zip, "wb") : NULL;
        if (!a || !b) { if (a) fclose(a); snprintf(why, wn, "the download failed (not found)"); return 0; }
        char buf[65536];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b);
        fclose(a);
        fclose(b);
    } else if (!https_get(o->url, zip, why, wn)) return 0;
    uint8_t sha[32];
    if (!sha256_file(zip, sha, NULL) || memcmp(sha, o->sha, 32)) {
        remove(zip);
        snprintf(why, wn, "its .zip is not the one the mod declares (SHA-256)");
        return 0;
    }
    int got = mod_zip_extract(zip, blob_dir, files, n, which, why, wn);
    remove(zip);
    fprintf(stderr, "[mods] %s: %d file%s from its .zip\n", o->url, got, got == 1 ? "" : "s");
    return got;
}
