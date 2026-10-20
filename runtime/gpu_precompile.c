/* The start-up's shader screen.
 *
 * tools/shader_aot.py --index lists every shader key the game's data holds
 * (dump/aot/keys_v<SHADER_CACHE_VERSION>.txt, beside the programs themselves in programs.pack).
 * Before the game's thread starts, the keys whose files are not in the disk cache are found
 * (one directory listing: well under a second) and, if there are any, compiled by
 * tools/shaderaot.c processes - one a logical processor but one, at low priority - while the
 * window shows how far they are. Processes, not the renderer's compiler threads: the translator
 * gives up on some programs with rt_fatal, which would end the game, and shaderaot counts those
 * and carries on. Esc starts the game at once; what is compiled stays, and the next start
 * compiles the rest. The processes die with the game (shaderaot --die-with-parent), and write
 * each file whole (a .tmp renamed), so closing the window leaves nothing half written.
 *
 * Windows keeps Prepare Shaders.bat (tools/shader_aot.py): there this does nothing. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_precompile.h"
#include "gpu_shader_cache.h"

volatile int precompile_active, precompile_skip;

/* "done <compiled> <cached> <left out>" */
int precompile_done_line(const char *line, unsigned long counts[3]) {
    return sscanf(line, "done %lu %lu %lu", &counts[0], &counts[1], &counts[2]) == 3;
}

#ifdef _WIN32
void precompile_run(void) {
    const char *v = getenv("SSBU_PRECOMPILE");
    if (v && atoi(v)) fprintf(stderr, "[shaders] the start-up shader screen is Linux's: on Windows, Prepare Shaders.bat compiles them\n");
}

long precompile_missing(const char *keys_path, const char *cache_dir, FILE *missing) {
    (void)keys_path; (void)cache_dir; (void)missing;
    return -1;
}
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gpu_backend.h"
#include "host.h"

/* ---- which keys the cache lacks --------------------------------------------------------- */
typedef struct NameSet { char **slot; size_t cap; } NameSet;

static size_t name_hash(const char *s) {
    size_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}
static void name_add(NameSet *ns, const char *name) {
    for (size_t i = name_hash(name) & (ns->cap - 1);; i = (i + 1) & (ns->cap - 1)) {
        if (!ns->slot[i]) { ns->slot[i] = strdup(name); return; }
        if (!strcmp(ns->slot[i], name)) return;
    }
}
static int name_has(const NameSet *ns, const char *name) {
    for (size_t i = name_hash(name) & (ns->cap - 1);; i = (i + 1) & (ns->cap - 1)) {
        if (!ns->slot[i]) return 0;
        if (!strcmp(ns->slot[i], name)) return 1;
    }
}

long precompile_missing(const char *keys_path, const char *cache_dir, FILE *missing) {
    FILE *keys = fopen(keys_path, "r");
    if (!keys) return -1;
    /* the cache's file names, at most half the table full */
    size_t files = 0;
    DIR *d = opendir(cache_dir);
    for (struct dirent *e; d && (e = readdir(d));) files++;
    NameSet ns = {NULL, 1024};
    while (ns.cap < files * 2) ns.cap <<= 1;
    ns.slot = calloc(ns.cap, sizeof *ns.slot);
    if (d) {
        rewinddir(d);
        size_t added = 0;
        for (struct dirent *e; (e = readdir(d)) && added < ns.cap / 2; added++) name_add(&ns, e->d_name);
        closedir(d);
    }
    long n = 0;
    char line[128], kind[4], path[1200];
    unsigned long long hash;
    unsigned mask;
    while (fgets(line, sizeof line, keys)) {
        int got = sscanf(line, "%3s %llx %x", kind, &hash, &mask);
        if (got >= 2 && !strcmp(kind, "vs")) shader_cache_file(path, sizeof path, "", SHADER_VERTEX, hash, 0, 0);
        else if (got == 3 && !strcmp(kind, "ps")) shader_cache_file(path, sizeof path, "", SHADER_PIXEL, hash, mask, 0);
        else continue;
        if (name_has(&ns, path + 1)) continue;   /* (path + 1: the name, past the empty directory's '/') */
        strncat(path, ".failed", sizeof path - strlen(path) - 1);
        if (name_has(&ns, path + 1)) continue;   /* (shaderaot tried it and could not: not every start again) */
        n++;
        if (missing) fputs(line, missing);
    }
    fclose(keys);
    for (size_t i = 0; i < ns.cap; i++) free(ns.slot[i]);
    free(ns.slot);
    return n;
}

/* ---- the compiler's processes ------------------------------------------------------------- */
typedef struct Child { pid_t pid; int out; unsigned long counts[3]; char buf[256]; size_t len; } Child;

/* shaderaot with these arguments, at low priority, its stdout a pipe (or to_file); 0 if it could not start */
static pid_t spawn(char *const argv[], int *out, const char *to_file) {
    int p[2] = {-1, -1};
    int fd = to_file ? open(to_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (to_file ? fd < 0 : pipe2(p, O_CLOEXEC)) return 0;
    pid_t pid = fork();
    if (pid == 0) {   /* (only what is safe between fork and exec in a threaded process) */
        setpriority(PRIO_PROCESS, 0, 10);
        dup2(to_file ? fd : p[1], 1);
        execv(argv[0], argv);
        _exit(127);
    }
    if (to_file) close(fd);
    else close(p[1]);
    if (pid < 0) { if (!to_file) close(p[0]); return 0; }
    if (out) { *out = p[0]; fcntl(p[0], F_SETFL, O_NONBLOCK); }
    return pid;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Reads what a child printed; its count of keys from its last whole "done" line. 0 at its end. */
static int child_read(Child *c) {
    for (;;) {
        ssize_t n = read(c->out, c->buf + c->len, sizeof c->buf - 1 - c->len);
        if (n == 0) return 0;
        if (n < 0) return errno == EAGAIN || errno == EINTR;
        c->len += (size_t)n;
        c->buf[c->len] = 0;
        char *nl;
        while ((nl = strchr(c->buf, '\n'))) {
            *nl = 0;
            unsigned long counts[3];
            if (precompile_done_line(c->buf, counts)) memcpy(c->counts, counts, sizeof counts);
            c->len -= (size_t)(nl + 1 - c->buf);
            memmove(c->buf, nl + 1, c->len + 1);
        }
        if (c->len == sizeof c->buf - 1) c->len = 0;   /* (a line too long for a "done" one: dropped) */
    }
}

static void stop_children(Child *ch, int n) {
    for (int i = 0; i < n; i++) if (ch[i].pid) kill(ch[i].pid, SIGTERM);
    for (int tries = 0; tries < 20; tries++) {   /* a second for them to go, then they are killed */
        int left = 0;
        for (int i = 0; i < n; i++) if (ch[i].pid && waitpid(ch[i].pid, NULL, WNOHANG) == 0) left++; else ch[i].pid = 0;
        if (!left) return;
        usleep(50000);
    }
    for (int i = 0; i < n; i++) if (ch[i].pid) { kill(ch[i].pid, SIGKILL); waitpid(ch[i].pid, NULL, 0); ch[i].pid = 0; }
}

static void eta_text(char *out, size_t n, long done, long total, double first_at) {
    if (done <= 0 || first_at <= 0) { snprintf(out, n, "%ld / %ld   (starting)", done, total); return; }
    double rate = done / (now_s() - first_at + 1e-3), left = (total - done) / (rate > 0 ? rate : 1);
    if (left >= 90) snprintf(out, n, "%ld / %ld   about %.0f min left", done, total, left / 60 + 0.5);
    else snprintf(out, n, "%ld / %ld   about %.0f s left", done, total, left < 1 ? 1 : left);
}

static int file_exists(const char *path) { return access(path, F_OK) == 0; }

/* The compiler: SSBU_SHADERAOT; else ./shaderaot (the package's); else a build tree's, beside build-linux */
static int find_compiler(char *out, size_t n) {
    const char *env = getenv("SSBU_SHADERAOT");
    if (env && *env) { snprintf(out, n, "%s", env); return file_exists(out); }
    snprintf(out, n, "./shaderaot");
    if (file_exists(out)) return 1;
    snprintf(out, n, "%s/../../build-linux-tools/shaderaot", rt_data_dir);
    return file_exists(out);
}

void precompile_run(void) {
    const char *v = getenv("SSBU_PRECOMPILE");
    if (v && !atoi(v)) return;
    char aot[1024], pack[1100], keys[1100], missing[1100], cache[1024], exe[1100];
    snprintf(aot, sizeof aot, "%s/../aot", rt_data_dir);
    snprintf(pack, sizeof pack, "%s/programs.pack", aot);
    snprintf(keys, sizeof keys, "%s/keys_v%u.txt", aot, SHADER_CACHE_VERSION);
    snprintf(missing, sizeof missing, "%s/missing.txt", aot);
    if (getenv("SSBU_SHADER_CACHE")) snprintf(cache, sizeof cache, "%s", getenv("SSBU_SHADER_CACHE"));   /* (as gpu_d3d11.c's cache_dir) */
    else snprintf(cache, sizeof cache, "%s/../shadercache", rt_data_dir);
    mkdir(cache, 0755);
    if (!file_exists(pack)) { fprintf(stderr, "[shaders] no shader index (dump/aot/programs.pack): starting without precompiling\n"); return; }
    if (!find_compiler(exe, sizeof exe)) { fprintf(stderr, "[shaders] no shaderaot to compile with: starting without precompiling\n"); return; }
    double t0 = now_s();
    precompile_active = 1;
    precompile_skip = 0;

    /* the list of keys for this cache version, made again when it changed */
    if (!file_exists(keys)) {
        char tmp[1200];
        snprintf(tmp, sizeof tmp, "%s.tmp", keys);
        char *argv[] = {exe, pack, cache, "0", "1", "--dry", "--die-with-parent", NULL};
        pid_t pid = spawn(argv, NULL, tmp);
        int status = -1;
        fprintf(stderr, "[shaders] indexing the game's shaders for cache version %u\n", SHADER_CACHE_VERSION);
        while (pid && waitpid(pid, &status, WNOHANG) == 0) {
            if (precompile_skip) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); status = -1; break; }
            if (!gfx_progress("Indexing shaders", 0, 0, "reading the game's shaders")) usleep(50000);
        }
        if (pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) rename(tmp, keys);
        else { unlink(tmp); fprintf(stderr, "[shaders] indexing failed or skipped: starting without precompiling\n"); precompile_active = 0; return; }
    }

    FILE *m = fopen(missing, "w");
    long total = m ? precompile_missing(keys, cache, m) : -1;
    if (m) fclose(m);
    if (total <= 0) {
        if (total < 0) fprintf(stderr, "[shaders] cannot read %s: starting without precompiling\n", keys);
        unlink(missing);
        precompile_active = 0;
        return;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN), parts = cpus > 1 ? cpus - 1 : 1;
    if (parts > 64) parts = 64;
    if (parts > total) parts = total;
    fprintf(stderr, "[shaders] %ld shaders missing from the cache: compiling them with %ld processes (Esc: play now)\n", total, parts);
    Child ch[64];
    memset(ch, 0, sizeof ch);
    char part_s[64][12], parts_s[12];
    snprintf(parts_s, sizeof parts_s, "%ld", parts);
    for (long i = 0; i < parts; i++) {
        snprintf(part_s[i], sizeof part_s[i], "%ld", i);
        char *argv[] = {exe, pack, cache, part_s[i], parts_s, "--keys", missing, "--progress", "--die-with-parent", NULL};
        ch[i].pid = spawn(argv, &ch[i].out, NULL);
        if (!ch[i].pid) fprintf(stderr, "[shaders] cannot start %s\n", exe);
    }
    double first_at = 0;
    long done = 0, failed = 0;
    for (;;) {
        struct pollfd pf[64];
        int nopen = 0;
        for (long i = 0; i < parts; i++) if (ch[i].out > 0) pf[nopen++] = (struct pollfd){ch[i].out, POLLIN, 0};
        if (!nopen) break;
        poll(pf, (nfds_t)nopen, 50);
        done = failed = 0;
        for (long i = 0; i < parts; i++) {
            if (ch[i].out > 0 && !child_read(&ch[i])) { close(ch[i].out); ch[i].out = -1; }
            done += (long)(ch[i].counts[0] + ch[i].counts[1] + ch[i].counts[2]);
            failed += (long)ch[i].counts[2];
        }
        if (done && !first_at) first_at = now_s();
        if (precompile_skip) break;
        char status[96];
        eta_text(status, sizeof status, done > total ? total : done, total, first_at);
        gfx_progress("Preparing shaders", (unsigned)(done > total ? total : done), (unsigned)total, status);
    }
    int skipped = precompile_skip;
    stop_children(ch, (int)parts);
    for (long i = 0; i < parts; i++) if (ch[i].out > 0) close(ch[i].out);
    unlink(missing);
    precompile_active = 0;
    if (done > total) done = total;
    fprintf(stderr, "[shaders] precompiled %ld of %ld (%ld failed or left out, %ld left%s) in %.0f s\n", done - failed, total, failed,
            total - done, skipped ? ": Esc pressed" : "", now_s() - t0);
}
#endif
