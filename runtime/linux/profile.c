/* The sampling profiler behind F6, on Linux (debug_profile.c is the Windows one, and this
 * writes the same report). For a few seconds, every millisecond, each of the process's threads
 * is interrupted by a signal and notes where it is (and, while busy, its call stack); then the
 * busiest functions (named from ssbu.syms next to the program, which the build writes),
 * modules and threads go to build/profile_<time>.txt, and its top lines to the log. A thread
 * stopped right after a `syscall` instruction counts as waiting. */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <link.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <windows.h>

#include "../debug_profile.h"

typedef unsigned long long U64;
typedef unsigned U32;

#define SLOTS (1u << 20)
typedef struct Sample { U64 rip; U32 count, name, thread; } Sample;
static Sample *table;
static U32 distinct;
static volatile LONG running;
static double duration;

typedef struct ThreadStat { pid_t tid; U32 samples, waiting, reported; char desc[64]; } ThreadStat;
static ThreadStat threads[512];
static U32 nthreads;

typedef struct Module { U64 base, size; char name[64]; } Module;
static Module modules[512];
static U32 nmodules, exe_module;

#define STACK_DEPTH 24
#define MAX_STACKS 65536
typedef struct Stack { U32 thread, depth, waiting; U64 pc[STACK_DEPTH]; } Stack;
static Stack *stacks;
static U32 nstacks;

static U64 *sym_va;
static U32 *sym_name, nsyms;
static char *sym_pool;

int profile_running(void) { return running != 0; }

static void add(U64 rip, U32 thread) {
    U32 h = (U32)(((rip ^ (U64)thread << 52) * 0x9E3779B97F4A7C15ull) >> 44) & (SLOTS - 1);
    while (table[h].rip && (table[h].rip != rip || table[h].thread != thread)) h = (h + 1) & (SLOTS - 1);
    if (!table[h].rip) { if (distinct >= SLOTS / 2) return; table[h].rip = rip; table[h].thread = thread; distinct++; }
    table[h].count++;
}

static int module_of(U64 rip) {
    for (U32 i = 0; i < nmodules; i++)
        if (rip >= modules[i].base && rip < modules[i].base + modules[i].size) return (int)i;
    return -1;
}

static int one_module(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (nmodules == 512) return 1;
    U64 lo = ~0ull, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            U64 a = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
            if (a < lo) lo = a;
            if (a + info->dlpi_phdr[i].p_memsz > hi) hi = a + info->dlpi_phdr[i].p_memsz;
        }
    if (hi <= lo) return 0;
    Module *m = &modules[nmodules];
    m->base = lo;
    m->size = hi - lo;
    const char *n = info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "ssbu";
    const char *slash = strrchr(n, '/');
    snprintf(m->name, sizeof m->name, "%s", slash ? slash + 1 : n);
    if (!info->dlpi_name || !*info->dlpi_name) exe_module = nmodules;
    nmodules++;
    return 0;
}

static void list_modules(void) {
    nmodules = 0;
    dl_iterate_phdr(one_module, NULL);
}

/* The program's symbols: "<address> <type> <name>" lines, sorted (llvm-nm -n), in ssbu.syms. */
static int load_symbols(void) {
    if (sym_va) return 1;
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 8);
    if (n <= 0) return 0;
    path[n] = 0;
    char *dot = strrchr(path, '/');
    snprintf(dot ? dot + 1 : path, sizeof path - (size_t)((dot ? dot + 1 : path) - path), "ssbu.syms");
    FILE *map = fopen(path, "r");
    if (!map) return 0;
    U32 cap = 1 << 18;
    size_t pool_used = 0, pool_cap = 1 << 22;
    sym_va = malloc(sizeof *sym_va * cap);
    sym_name = malloc(sizeof *sym_name * cap);
    sym_pool = malloc(pool_cap);
    char line[512];
    while (fgets(line, sizeof line, map)) {
        U64 va;
        char type, name[256];
        if (sscanf(line, "%llx %c %255s", &va, &type, name) != 3 || (type != 'T' && type != 't')) continue;
        if (nsyms && va < sym_va[nsyms - 1]) continue;
        if (nsyms == cap) {
            cap *= 2;
            sym_va = realloc(sym_va, sizeof *sym_va * cap);
            sym_name = realloc(sym_name, sizeof *sym_name * cap);
        }
        size_t len = strlen(name) + 1;
        if (pool_used + len > pool_cap) { pool_cap *= 2; sym_pool = realloc(sym_pool, pool_cap); }
        memcpy(sym_pool + pool_used, name, len);
        sym_va[nsyms] = va;
        sym_name[nsyms++] = (U32)pool_used;
        pool_used += len;
    }
    fclose(map);
    return 1;
}

static U32 symbol_of(U64 rip) {   /* (the program is not position-independent: its addresses are the map's) */
    if (!nsyms || rip < sym_va[0] || module_of(rip) != (int)exe_module) return 0;
    U32 lo = 0, hi = nsyms;
    while (hi - lo > 1) {
        U32 mid = (lo + hi) / 2;
        if (sym_va[mid] <= rip) lo = mid; else hi = mid;
    }
    return sym_name[lo] + 1;
}

static ThreadStat *thread_stat(pid_t tid) {
    for (U32 i = 0; i < nthreads; i++) if (threads[i].tid == tid) return &threads[i];
    if (nthreads == 512) return NULL;
    threads[nthreads] = (ThreadStat){.tid = tid};
    const char *d = rt_thread_description(tid);
    if (d && *d) snprintf(threads[nthreads].desc, sizeof threads[nthreads].desc, "%s", d);
    else {
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(threads[nthreads].desc, sizeof threads[nthreads].desc, f)) threads[nthreads].desc[strcspn(threads[nthreads].desc, "\n")] = 0;
            fclose(f);
        }
    }
    return &threads[nthreads++];
}

/* ---- the report (as debug_profile.c's) ----------------------------------------------------- */
typedef struct Entry { const char *name; U64 rip; U32 count; } Entry;
static int by_count(const void *a, const void *b) { return (int)((const Entry *)b)->count - (int)((const Entry *)a)->count; }
static int by_rip(const void *a, const void *b) {
    U64 x = ((const Sample *)a)->rip, y = ((const Sample *)b)->rip;
    return x < y ? -1 : x > y;
}

static void busiest(FILE *f, const Sample *s, U32 n, const char *pool, Entry *e, int thread, U64 busy, U32 limit) {
    U32 ne = 0;
    for (U32 i = 0; i < n; i++) {
        if (thread >= 0 && s[i].thread != (U32)thread) continue;
        const char *name = s[i].name ? pool + s[i].name - 1 : NULL;
        if (name && ne && e[ne - 1].name && !strcmp(e[ne - 1].name, name)) { e[ne - 1].count += s[i].count; continue; }
        if (!name && ne && !e[ne - 1].name && e[ne - 1].rip == s[i].rip) { e[ne - 1].count += s[i].count; continue; }
        e[ne++] = (Entry){name, s[i].rip, s[i].count};
    }
    qsort(e, ne, sizeof *e, by_count);
    for (U32 i = 0; i < ne && i < limit; i++) {
        int m = module_of(e[i].rip);
        if (e[i].name) fprintf(f, "  %5.2f%%  %s\n", 100.0 * e[i].count / (busy ? busy : 1), e[i].name);
        else fprintf(f, "  %5.2f%%  %s+%#llx\n", 100.0 * e[i].count / (busy ? busy : 1), m >= 0 ? modules[m].name : "?",
                     m >= 0 ? e[i].rip - modules[m].base : e[i].rip);
    }
}

typedef struct Count { U32 key[3], count; } Count;
static int count_by_key(const void *a, const void *b) { return memcmp(((const Count *)a)->key, ((const Count *)b)->key, sizeof ((const Count *)a)->key); }
static int count_by_count(const void *a, const void *b) {
    U32 x = ((const Count *)a)->count, y = ((const Count *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}
static U32 frame_key(U64 pc) {
    U32 s = symbol_of(pc);
    if (s) return s;
    int m = module_of(pc);
    return m < 0 ? 0xFFFFFFFFu : 0x80000000u | (U32)m;
}
static const char *key_name(U32 key) {
    if (key == 0xFFFFFFFFu) return "?";
    if (key & 0x80000000u) return modules[key & 0x7FFFFFFFu].name;
    return sym_pool + key - 1;
}
static U32 tally(Count *c, U32 n) {
    if (!n) return 0;
    qsort(c, n, sizeof *c, count_by_key);
    U32 out = 0;
    for (U32 i = 0; i < n; i++) {
        if (out && !count_by_key(&c[out - 1], &c[i])) c[out - 1].count += c[i].count;
        else c[out++] = c[i];
    }
    qsort(c, out, sizeof *c, count_by_count);
    return out;
}

static void call_paths(FILE *f) {
    U32 t = 0, t_busy = 0, n = 0;
    for (U32 i = 0; i < nthreads; i++)
        if (threads[i].samples - threads[i].waiting > t_busy) { t = i; t_busy = threads[i].samples - threads[i].waiting; }
    U32 nwait = 0;
    for (U32 i = 0; i < nstacks; i++) { n += stacks[i].thread == t && !stacks[i].waiting; nwait += stacks[i].thread == t && stacks[i].waiting; }
    if (!n) return;
    Count *c = malloc(sizeof *c * (size_t)(n + nwait) * STACK_DEPTH);
    U32 nc = 0;
    for (U32 i = 0; i < nstacks; i++) {
        if (stacks[i].thread != t || stacks[i].waiting) continue;
        U32 keys[STACK_DEPTH], nk = 0;
        for (U32 d = 0; d < stacks[i].depth; d++) {
            U32 k = frame_key(stacks[i].pc[d]), seen = 0;
            for (U32 j = 0; j < nk && !seen; j++) seen = keys[j] == k;
            if (!seen) { keys[nk++] = k; c[nc++] = (Count){{k, 0, 0}, 1}; }
        }
    }
    nc = tally(c, nc);
    fprintf(f, "\nThread %d %s: time in or under each function (%% of %u sampled stacks, at most %u frames deep):\n",
            (int)threads[t].tid, threads[t].desc, n, STACK_DEPTH);
    for (U32 i = 0; i < nc && i < 60; i++) fprintf(f, "  %5.1f%%  %s\n", 100.0 * c[i].count / n, key_name(c[i].key[0]));
    nc = 0;
    for (U32 i = 0; i < nstacks; i++)
        if (stacks[i].thread == t && !stacks[i].waiting) c[nc++] = (Count){{frame_key(stacks[i].pc[0]), 0, 0}, 1};
    nc = tally(c, nc);
    U32 top = nc < 12 ? nc : 12;
    Count *callee = malloc(sizeof *callee * (top ? top : 1)), *paths = c + nc;
    memcpy(callee, c, sizeof *callee * top);
    fprintf(f, "\nThread %d: how it reaches its busiest functions (%% of that function's samples):\n", (int)threads[t].tid);
    for (U32 j = 0; j < top; j++) {
        U32 np = 0;
        for (U32 i = 0; i < nstacks; i++) {
            const Stack *k = &stacks[i];
            if (k->thread != t || k->waiting || frame_key(k->pc[0]) != callee[j].key[0]) continue;
            Count p = {{0, 0, 0}, 1};
            for (U32 d = 1; d < 4 && d < k->depth; d++) p.key[d - 1] = frame_key(k->pc[d]);
            paths[np++] = p;
        }
        np = tally(paths, np);
        fprintf(f, "  %s (%.1f%% of the thread):\n", key_name(callee[j].key[0]), 100.0 * callee[j].count / n);
        for (U32 i = 0; i < np && i < 5; i++) {
            fprintf(f, "    %5.1f%%  <-", 100.0 * paths[i].count / callee[j].count);
            for (U32 d = 0; d < 3 && paths[i].key[d]; d++) fprintf(f, " %s%s", d ? "<- " : "", key_name(paths[i].key[d]));
            fprintf(f, "\n");
        }
    }
    /* the guest main thread's waits: from the first frame outside the C library, what it waited in */
    for (U32 m = 0; m < nthreads; m++) {
        if (!strstr(threads[m].desc, "main thread")) continue;
        U32 nw = 0;
        Count *w = malloc(sizeof *w * (nstacks ? nstacks : 1));
        for (U32 i = 0; i < nstacks; i++) {
            const Stack *k = &stacks[i];
            if (k->thread != m || !k->waiting) continue;
            U32 d = 0;
            while (d < k->depth) {
                int mod = module_of(k->pc[d]);
                if (mod >= 0 && !strstr(modules[mod].name, "libc.so") && !strstr(modules[mod].name, "ld-linux")) break;
                d++;
            }
            Count p = {{0, 0, 0}, 1};
            for (U32 j = 0; j < 3 && d + j < k->depth; j++) p.key[j] = frame_key(k->pc[d + j]);
            if (!p.key[0]) p.key[0] = 0xFFFFFFFFu;
            w[nw++] = p;
        }
        nw = tally(w, nw);
        fprintf(f, "\nThread %d %s: where it waits (%u of its samples were waiting; %% of those with stacks):\n",
                (int)threads[m].tid, threads[m].desc, threads[m].waiting);
        U32 sum = 0;
        for (U32 i = 0; i < nw; i++) sum += w[i].count;
        for (U32 i = 0; i < nw && i < 15; i++) {
            fprintf(f, "  %5.1f%%  in", 100.0 * w[i].count / (sum ? sum : 1));
            for (U32 d = 0; d < 3 && w[i].key[d]; d++) fprintf(f, " %s%s", d ? "<- " : "", key_name(w[i].key[d]));
            fprintf(f, "\n");
        }
        free(w);
    }
    free(callee);
    free(c);
}

static void report(U64 total, U64 busy) {
    char path[160];
    SYSTEMTIME st;
    GetLocalTime(&st);
    struct stat sb;
    snprintf(path, sizeof path, "%sprofile_%04u%02u%02u_%02u%02u%02u.txt", !stat("build", &sb) && S_ISDIR(sb.st_mode) ? "build/" : "",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[profile] cannot write %s (errno %d)\n", path, errno); f = stderr; }
    fprintf(f, "Sampling profile: %.0f s, %llu samples (%llu busy, %llu in system calls - mostly waiting)\n\n", duration, total, busy, total - busy);
    fprintf(f, "Threads (samples, busy %%):\n");
    for (U32 i = 0; i < nthreads; i++)
        if (threads[i].samples > threads[i].waiting)
            fprintf(f, "  %6d %-32s %6u samples, %5.1f%% busy\n", (int)threads[i].tid, threads[i].desc, threads[i].samples,
                    100.0 * (threads[i].samples - threads[i].waiting) / threads[i].samples);
    Sample *s = malloc(sizeof *s * (distinct ? distinct : 1));
    U32 n = 0;
    for (U32 i = 0; i < SLOTS; i++) if (table[i].rip) s[n++] = table[i];
    qsort(s, n, sizeof *s, by_rip);
    fprintf(f, "\nBusy time by module:\n");
    U64 *per_module = calloc(nmodules + 1, sizeof *per_module);
    for (U32 i = 0; i < n; i++) {
        int m = module_of(s[i].rip);
        per_module[m < 0 ? nmodules : (U32)m] += s[i].count;
    }
    for (U32 m = 0; m <= nmodules; m++)
        if (per_module[m] * 1000 >= busy && busy)
            fprintf(f, "  %5.1f%%  %s\n", 100.0 * per_module[m] / busy, m < nmodules ? modules[m].name : "(no module)");
    if (!load_symbols()) fprintf(f, "\n(ssbu.syms not found next to the program: functions unnamed)\n");
    for (U32 i = 0; i < n; i++) s[i].name = symbol_of(s[i].rip);
    Entry *e = malloc(sizeof *e * (n ? n : 1));
    fprintf(f, "\nBusiest code (%% of busy samples):\n");
    busiest(f, s, n, sym_pool, e, -1, busy, 80);
    for (int round = 0; round < 4; round++) {
        U32 best = 0, best_busy = 0;
        for (U32 i = 0; i < nthreads; i++) {
            U32 b = threads[i].samples - threads[i].waiting;
            if (!threads[i].reported && b > best_busy) { best = i; best_busy = b; }
        }
        if (best_busy * 50 < busy) break;
        threads[best].reported = 1;
        fprintf(f, "\nThread %d %s (%.1f%% busy) - its busiest code (%% of its busy samples):\n", (int)threads[best].tid,
                threads[best].desc, 100.0 * best_busy / threads[best].samples);
        busiest(f, s, n, sym_pool, e, (int)best, best_busy, 25);
    }
    call_paths(f);
    if (f != stderr) {
        fclose(f);
        fprintf(stderr, "[profile] written to %s\n", path);
    }
    free(e); free(s); free(per_module);
}

/* ---- sampling ------------------------------------------------------------------------------ */
#define SIG_PROFILE (SIGRTMIN + 3)
static struct { U64 rip; int waiting, want_stack, depth; void *frames[STACK_DEPTH + 8]; } shot;
static sem_t shot_done;

static void on_sample(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    int saved = errno;
    ucontext_t *uc = ucv;
    U64 rip = (U64)uc->uc_mcontext.gregs[REG_RIP];
    const unsigned char *at = (const unsigned char *)rip;
    shot.rip = rip;
    shot.waiting = (rip & 0xFFF) >= 2 && at[-2] == 0x0F && at[-1] == 0x05;   /* (stopped just after a `syscall`) */
    shot.depth = shot.want_stack ? backtrace(shot.frames, STACK_DEPTH + 8) : 0;
    sem_post(&shot_done);
    errno = saved;
}

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    pid_t self = gettid();
    list_modules();
    memset(table, 0, sizeof *table * SLOTS);
    distinct = nstacks = nthreads = 0;
    pid_t tids[512];
    U32 nt = 0;
    U64 total = 0, busy = 0, t0 = GetTickCount64(), refreshed = 0;
    for (;;) {
        U64 now = GetTickCount64();
        if (now - t0 >= (U64)(duration * 1000)) break;
        if (!nt || now - refreshed > 250) {   /* threads come and go */
            nt = 0;
            DIR *d = opendir("/proc/self/task");
            for (struct dirent *e; d && (e = readdir(d)) && nt < 512;) {
                pid_t tid = (pid_t)atoi(e->d_name);
                if (tid > 0 && tid != self) tids[nt++] = tid;
            }
            if (d) closedir(d);
            refreshed = now;
        }
        for (U32 i = 0; i < nt; i++) {
            ThreadStat *ts = thread_stat(tids[i]);
            shot.want_stack = ts && nstacks < MAX_STACKS;
            while (sem_trywait(&shot_done) == 0) {}   /* (a sample that came after its wait gave up) */
            if (syscall(SYS_tgkill, getpid(), tids[i], SIG_PROFILE)) continue;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 20000000L;
            if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            if (sem_timedwait(&shot_done, &until)) continue;
            total++;
            busy += !shot.waiting;
            add(shot.rip, ts ? (U32)(ts - threads) : 511);
            if (ts) {
                ts->samples++;
                ts->waiting += shot.waiting;
                if ((!shot.waiting || strstr(ts->desc, "main thread")) && shot.depth && nstacks < MAX_STACKS) {
                    int from = 0;   /* (from the interrupted frame: past the handler's own) */
                    for (int k = 0; k < shot.depth; k++) if ((U64)shot.frames[k] == shot.rip) { from = k; break; }
                    Stack *k = &stacks[nstacks++];
                    k->thread = (U32)(ts - threads);
                    k->waiting = shot.waiting;
                    k->depth = 0;
                    for (int j = from; j < shot.depth && k->depth < STACK_DEPTH; j++) k->pc[k->depth++] = (U64)shot.frames[j] - (j > from);
                }
            }
        }
        Sleep(1);
    }
    report(total, busy);
    InterlockedExchange(&running, 0);
    return 0;
}

void profile_start(double seconds) {
    if (InterlockedCompareExchange(&running, 1, 0)) return;
    static int ready;
    if (!ready) {
        ready = 1;
        sem_init(&shot_done, 0, 0);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_sample;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIG_PROFILE, &sa, NULL);
        table = malloc(sizeof *table * SLOTS);
        stacks = malloc(sizeof *stacks * MAX_STACKS);
    }
    duration = seconds;
    fprintf(stderr, "[profile] sampling for %.0f s\n", seconds);
    CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
}
