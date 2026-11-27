/* The recovery from a mod that breaks the game: see mod_recovery.h.
 *
 * session.txt, a line each:
 *   state = starting | running | crashed | frozen
 *   kind = crash | fatal | frozen        (how it failed; not while starting or running)
 *   running = 0 | 1                      (it had got going before it failed)
 *   print = <16 hex digits>              (its mods)
 *   fails = <n>                          (failed sessions with that print just before it)
 *   mod = <folder>\t<hash>               (each of its mods)
 *   new = <folder>                       (new or changed since the last start that worked)
 *   read = <folder>\t<data.arc path>     (the last mod files read, the oldest first; when it failed)
 * last_good.txt: "<folder>\t<hash>" a line. */
#include "mod_recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mod_arc.h"
#include "mod_scan.h"

#define REC_MODS 256
#define RING 8

static void path_of(char *out, size_t n, const char *dir, const char *leaf) { snprintf(out, n, "%s/%s", dir, leaf); }

static void hex64(uint64_t v, char out[17]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--, v >>= 4) out[i] = H[v & 15];
    out[16] = 0;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* ---- reading the last session ------------------------------------------------------------------- */
int mod_recovery_read(const char *dir, Recovery *r) {
    char file[1024], line[700];
    memset(r, 0, sizeof *r);
    path_of(file, sizeof file, dir, "session.txt");
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char state[32] = "", kind[32] = "", last_mod[128] = "", last_path[256] = "";
    int fails = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strstr(line, " = ");
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 3;
        if (!strcmp(k, "state")) snprintf(state, sizeof state, "%s", v);
        else if (!strcmp(k, "kind")) snprintf(kind, sizeof kind, "%s", v);
        else if (!strcmp(k, "running")) r->was_running = atoi(v);
        else if (!strcmp(k, "print")) snprintf(r->print, sizeof r->print, "%s", v);
        else if (!strcmp(k, "fails")) fails = atoi(v);
        else if (!strcmp(k, "new") && r->nnews < REC_NEWS) snprintf(r->news[r->nnews++], sizeof r->news[0], "%s", v);
        else if (!strcmp(k, "read")) {   /* (the last one wins: the file read last) */
            const char *tab = strchr(v, '\t');
            if (!tab) continue;
            snprintf(last_mod, sizeof last_mod, "%.*s", (int)(tab - v), v);
            snprintf(last_path, sizeof last_path, "%s", tab + 1);
        }
    }
    fclose(f);
    remove(file);
    if (!strcmp(state, "crashed")) r->failed = !strcmp(kind, "fatal") ? REC_FATAL : REC_CRASH;
    else if (!strcmp(state, "frozen")) r->failed = REC_FROZEN;
    else if (!strcmp(state, "starting")) r->failed = REC_UNCLEAN;
    else if (!strcmp(state, "running")) r->failed = REC_UNCLEAN, r->was_running = 1;   /* (closed without a word: killed) */
    else return 0;
    if (last_mod[0]) {
        snprintf(r->suspect, sizeof r->suspect, "%s", last_mod);
        snprintf(r->suspect_path, sizeof r->suspect_path, "%s", last_path);
    } else if (r->nnews) {   /* (nothing of a mod read: the new ones) */
        r->suspect_new = 1;
        if (r->nnews == 1) snprintf(r->suspect, sizeof r->suspect, "%s", r->news[0]);
    }
    r->fails = fails + 1;
    r->no_mods = r->fails >= 2;
    return 1;
}

int mod_recovery_last_good(const char *dir, char keys[][128], int max) {
    char file[1024], line[400];
    path_of(file, sizeof file, dir, "last_good.txt");
    FILE *f = fopen(file, "r");
    int n = 0;
    if (!f) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        if (line[0]) snprintf(keys[n++], 128, "%s", line);
    }
    fclose(f);
    return n;
}

/* ---- the session being written -------------------------------------------------------------------- */
static struct {
    int on;
    char file[1024];                /* session.txt */
    char body[16384];               /* print, fails, mods, news: the lines every state has */
    size_t body_len;
    RecoveryMod mods[REC_MODS];
    int nmods;
    int running;
} S;

static const char *volatile ring_mod[RING], *volatile ring_path[RING];
static volatile unsigned ring_head;

void mod_recovery_read_file(const char *mod, const char *path) {
    unsigned h = ring_head;
    if (h && ring_path[(h - 1) % RING] == path) return;   /* (the same file read on: once) */
    ring_mod[h % RING] = mod, ring_path[h % RING] = path;
    ring_head = h + 1;
}

/* (no allocation, no stdio: a fault handler's too) */
static size_t put(char *b, size_t at, size_t cap, const char *s) {
    while (*s && at + 1 < cap) b[at++] = *s++;
    b[at] = 0;
    return at;
}

static void write_whole(const char *file, const char *text, size_t n) {
#ifdef _WIN32
    HANDLE h = CreateFileA(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, text, (DWORD)n, &w, NULL);
    CloseHandle(h);
#else
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    while (n) {
        ssize_t w = write(fd, text, n);
        if (w <= 0) break;
        text += w, n -= (size_t)w;
    }
    close(fd);
#endif
}

/* The file as it is now: state (and kind), running, the body, the files read (when it failed) */
static void write_session(const char *state, const char *kind, int with_reads) {
    static char out[24576];
    size_t o = 0;
    o = put(out, o, sizeof out, "state = ");
    o = put(out, o, sizeof out, state);
    o = put(out, o, sizeof out, "\n");
    if (kind) { o = put(out, o, sizeof out, "kind = "); o = put(out, o, sizeof out, kind); o = put(out, o, sizeof out, "\n"); }
    o = put(out, o, sizeof out, S.running ? "running = 1\n" : "running = 0\n");
    o = put(out, o, sizeof out, S.body);
    unsigned h = ring_head, from = h > RING ? h - RING : 0;
    for (unsigned i = from; with_reads && i < h; i++) {
        const char *m = ring_mod[i % RING], *p = ring_path[i % RING];
        if (!m || !p) continue;
        o = put(out, o, sizeof out, "read = ");
        o = put(out, o, sizeof out, m);
        o = put(out, o, sizeof out, "\t");
        o = put(out, o, sizeof out, p);
        o = put(out, o, sizeof out, "\n");
    }
    write_whole(S.file, out, o);
}

static int cmp_mod(const void *a, const void *b) { return strcmp(((const RecoveryMod *)a)->key, ((const RecoveryMod *)b)->key); }

int mod_recovery_begin(const char *dir, const RecoveryMod *mods, int n, const Recovery *prev) {
    memset(&S, 0, sizeof S);
    if (n <= 0) return 0;
    S.nmods = n < REC_MODS ? n : REC_MODS;
    memcpy(S.mods, mods, sizeof *mods * (size_t)S.nmods);
    qsort(S.mods, (size_t)S.nmods, sizeof *S.mods, cmp_mod);
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < S.nmods; i++) {
        h = fnv(h, S.mods[i].key, strlen(S.mods[i].key) + 1);
        h = fnv(h, &S.mods[i].hash, sizeof S.mods[i].hash);
    }
    char print[17];
    hex64(h, print);
    int same = prev && prev->failed && !strcmp(prev->print, print);
    if (same && prev->no_mods) return 1;   /* (twice with these mods, nobody chose: without them, and nothing written) */
    /* the new ones: not in last_good.txt with the same hash */
    char file[1024], line[400];
    path_of(file, sizeof file, dir, "last_good.txt");
    static RecoveryMod good[REC_MODS];
    int ngood = 0;
    FILE *f = fopen(file, "r");
    while (f && ngood < REC_MODS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        snprintf(good[ngood].key, sizeof good[0].key, "%s", line);
        good[ngood++].hash = strtoull(tab + 1, NULL, 16);
    }
    if (f) fclose(f);
    size_t o = 0;
    char num[32], hx[17];
    o = put(S.body, o, sizeof S.body, "print = ");
    o = put(S.body, o, sizeof S.body, print);
    snprintf(num, sizeof num, "\nfails = %d\n", same ? prev->fails : 0);
    o = put(S.body, o, sizeof S.body, num);
    for (int i = 0; i < S.nmods; i++) {
        hex64(S.mods[i].hash, hx);
        o = put(S.body, o, sizeof S.body, "mod = ");
        o = put(S.body, o, sizeof S.body, S.mods[i].key);
        o = put(S.body, o, sizeof S.body, "\t");
        o = put(S.body, o, sizeof S.body, hx);
        o = put(S.body, o, sizeof S.body, "\n");
    }
    for (int i = 0; i < S.nmods; i++) {
        int known = 0;
        for (int k = 0; k < ngood && !known; k++) known = !strcmp(good[k].key, S.mods[i].key) && good[k].hash == S.mods[i].hash;
        if (known) continue;
        o = put(S.body, o, sizeof S.body, "new = ");
        o = put(S.body, o, sizeof S.body, S.mods[i].key);
        o = put(S.body, o, sizeof S.body, "\n");
    }
    S.body_len = o;
    path_of(S.file, sizeof S.file, dir, "session.txt");
    ring_head = 0;
    S.on = 1;
    write_session("starting", NULL, 0);
    return 0;
}

void mod_recovery_running(const char *dir) {
    if (!S.on) return;
    S.running = 1;
    write_session("running", NULL, 0);
    char file[1024];
    path_of(file, sizeof file, dir, "last_good.txt");
    FILE *f = fopen(file, "w");
    if (!f) return;
    for (int i = 0; i < S.nmods; i++) fprintf(f, "%s\t%016llx\n", S.mods[i].key, (unsigned long long)S.mods[i].hash);
    fclose(f);
}

void mod_recovery_failed(int kind) {
    if (!S.on) return;
    write_session(kind == REC_FROZEN ? "frozen" : "crashed", kind == REC_FATAL ? "fatal" : kind == REC_FROZEN ? "frozen" : "crash", 1);
}

void mod_recovery_end(const char *dir) {
    (void)dir;
    if (!S.on) return;
    remove(S.file);
    S.on = 0;
}

/* ---- the runtime's ------------------------------------------------------------------------------ */
volatile int mod_recovery_mods_off;

char mod_gameplay_note[200];

void mod_recovery_note(char *out, unsigned n) {
    if (!n) return;
    out[0] = 0;
    if (mod_recovery_mods_off) snprintf(out, n, "Mods: off this start (the recovery) - others in a room see the game's own looks");
    else if (mod_gameplay_note[0]) snprintf(out, n, "%s", mod_gameplay_note);
}

const char *mod_recovery_dir(char *out, unsigned n) {
    char root[1024];
    mod_root(root, sizeof root);
    size_t l = strlen(root);
    if (l >= 5 && !strcmp(root + l - 5, "/mods")) root[l - 5] = 0;
    else if (l >= 5 && !strcmp(root + l - 5, "\\mods")) root[l - 5] = 0;
    snprintf(out, n, "%s/cache/mods", root);
    return out;
}

static Recovery state;
static int state_read;
Recovery *mod_recovery_state(void) {
    if (!state_read) {
        state_read = 1;
        char dir[1100];
        if (mod_recovery_read(mod_recovery_dir(dir, sizeof dir), &state)) {
            static const char *const KIND[] = {"", "did not start (closed or killed)", "crashed", "stopped (fatal error)", "froze"};
            fprintf(stderr, "[mods] the last session with mods %s%s; suspect: %s\n", KIND[state.failed], state.was_running ? " while playing" : "",
                    state.suspect[0] ? state.suspect : state.suspect_new ? "the new mods" : "none (probably not the mods)");
        }
    }
    return &state;
}

void mod_recovery_chosen(void) { mod_recovery_state()->no_mods = 0; }

static void make_dirs(const char *dir) {   /* cache/, cache/mods/ */
    char p[1100];
    snprintf(p, sizeof p, "%s", dir);
    char *slash = strrchr(p, '/');
    if (slash) {
        *slash = 0;
#ifdef _WIN32
        CreateDirectoryA(p, NULL);
#else
        mkdir(p, 0755);
#endif
        *slash = '/';
    }
#ifdef _WIN32
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

int mod_recovery_session(const ModArcFile *files, int n) {
    static RecoveryMod mods[REC_MODS];
    int nm = 0;
    for (int i = 0; i < n; i++) {   /* each mod's files: their paths, sizes and dates */
        int k = 0;
        while (k < nm && strcmp(mods[k].key, files[i].mod)) k++;
        if (k == nm) {
            if (nm == REC_MODS) continue;
            snprintf(mods[nm].key, sizeof mods[0].key, "%s", files[i].mod);
            mods[nm++].hash = 0xcbf29ce484222325ull;
        }
        uint64_t h = fnv(mods[k].hash, files[i].dest, strlen(files[i].dest) + 1);
        h = fnv(h, &files[i].size, sizeof files[i].size);
        mods[k].hash = fnv(h, &files[i].mtime, sizeof files[i].mtime);
    }
    char dir[1100];
    mod_recovery_dir(dir, sizeof dir);
    make_dirs(dir);
    Recovery *prev = mod_recovery_state();
    if (mod_recovery_begin(dir, mods, nm, prev->failed ? prev : NULL)) {
        mod_recovery_mods_off = 1;
        fprintf(stderr, "[mods] the game failed twice in a row with these mods and nothing was chosen: this start goes without mods "
                        "(they stay enabled; the menu's Mods page says more)\n");
        return 0;
    }
    return 1;
}

void mod_recovery_frames(int presenting_for_s) {
    static int done;
    if (done || presenting_for_s < 20 || !S.on) return;
    done = 1;
    char dir[1100];
    mod_recovery_running(mod_recovery_dir(dir, sizeof dir));
    fprintf(stderr, "[mods] 20 s of frames: these mods work (cache/mods/last_good.txt)\n");
}

static volatile int frozen;
void mod_recovery_frozen(void) { frozen = 1; mod_recovery_failed(REC_FROZEN); }
void mod_recovery_thawed(void) {   /* (frames again after a freeze: as it was) */
    if (!frozen || !S.on) return;
    frozen = 0;
    write_session(S.running ? "running" : "starting", NULL, 0);
}

void mod_recovery_close(void) {
    char dir[1100];
    mod_recovery_end(mod_recovery_dir(dir, sizeof dir));
}
