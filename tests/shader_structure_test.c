/* Structured control flow against the dispatch loop (runtime/gpu_shader_cfg.c), on the CPU: for
 * each program and many random sequences of condition outcomes, the blocks the dispatch loop runs
 * (pc, SSY/PBK stacks, guard) must be the blocks the structure tree runs.
 *
 *   shader_structure_test synth            the programs below, each with its expected outcome
 *   shader_structure_test mutate           the checker itself: a tree with an if's arms swapped must fail
 *   shader_structure_test fuzz [count]     random programs: refused, or alike (never "structure", an inconsistency)
 *   shader_structure_test corpus <dir>     every program SSBU_DUMP_SHADERS wrote (built WITH_TRANSLATOR)
 *
 * When the guard stops either (a loop that never ends), both must have stopped and the shorter
 * trace be the start of the other: the dispatch loop counts blocks, a structured loop iterations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_cfg.h"

/* ---- synthetic programs: one instruction a line ----
 *   [label:] plain | ssy L | pbk L | bra L | sync | brk | exit | brx L L ...
 * with '?' after the operation when its condition may or may not hold, '!' when it never does. */
typedef struct Synth { const char *name, *expect, *text; } Synth;

static const Synth SYNTH[] = {
    {"diamond", "structured",
     "ssy L\n bra? A\n plain\n sync\n A: plain\n sync\n L: plain\n exit\n"},
    {"if without else", "structured",
     "ssy L\n bra? L\n plain\n sync\n L: plain\n exit\n"},
    {"nested ssy/sync", "structured",
     "ssy L1\n bra? A\n ssy L2\n bra? B\n plain\n sync\n B: plain\n sync\n L2: plain\n sync\n A: plain\n sync\n L1: plain\n exit\n"},
    {"pbk/brk loop", "structured",
     "pbk E\n H: plain\n brk?\n plain\n bra H\n E: plain\n exit\n"},
    {"loop with exit", "structured",
     "pbk E\n H: plain\n exit?\n brk?\n plain\n bra H\n E: plain\n exit\n"},
    {"early exit", "structured",
     "plain\n exit?\n plain\n ssy L\n bra? A\n plain\n sync\n A: plain\n sync\n L: exit\n"},
    {"exit in a branch", "structured",
     "ssy L\n bra? A\n plain\n exit?\n plain\n sync\n A: plain\n sync\n L: plain\n exit\n"},
    {"crossing branches", "structured",
     "bra? X\n plain\n bra? Y\n X: plain\n bra? Z\n plain\n Y: plain\n Z: exit\n"},
    {"while loop", "structured",
     "H: bra? E\n plain\n bra H\n E: exit\n"},
    {"two latches", "structured",
     "H: plain\n bra? H\n plain\n bra? H\n exit\n"},
    {"nested loops", "structured",
     "pbk E1\n H1: plain\n pbk E2\n H2: plain\n brk?\n bra H2\n E2: plain\n brk?\n bra H1\n E1: exit\n"},
    {"loop returning inside", "structured",
     "pbk E\n H: plain\n bra? R\n brk?\n bra H\n R: plain\n exit\n E: plain\n exit\n"},
    {"loop never ending", "structured",
     "pbk E\n H: plain\n brk?\n bra H\n E: exit\n"},
    {"jump table", "structured",
     "plain\n brx A B C A\n A: plain\n bra L\n B: plain\n bra L\n C: plain\n L: exit\n"},
    {"jump table in a loop", "structured",
     "pbk E\n H: brx A B\n A: plain\n brk?\n bra H\n B: plain\n bra H\n E: exit\n"},
    {"never-taken branch", "structured",
     "bra! X\n plain\n X: plain\n exit\n"},
    {"irreducible", "irreducible",
     "bra? B\n A: plain\n B: plain\n bra? A\n exit\n"},
    {"inner loop leaving both loops", "loop-exits",   /* (two continuations: one in the outer loop, one past it) */
     "pbk E1\n H1: plain\n H2: plain\n bra? E1\n bra? H2\n plain\n bra? H1\n E1: exit\n"},
    {"loop leaving to two places", "loop-exits",
     "H: plain\n bra? X\n bra? Y\n bra H\n X: plain\n bra Z\n Y: plain\n Z: exit\n"},
    {"stack differs", "stack",
     "bra? A\n ssy L\n A: plain\n L: exit\n"},
    {"stack popped empty", "stack-empty",
     "plain\n sync\n exit\n"},
    {"stack overflow", "stack-overflow",
     "ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n ssy L\n L: exit\n"},
    {"unknown jump table", "brx",
     "plain\n brx\n A: exit\n"},
};

typedef struct Prog { CfInsn ins[256]; uint32_t n, tables[256][8]; } Prog;

static int parse(const char *text, Prog *p) {
    char labels[256][16];
    uint32_t nlabels = 0, label_at[256];
    char lines[256][128];
    uint32_t n = 0;
    for (const char *s = text; *s; ) {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len >= sizeof lines[0] || n == 256) return 0;
        memcpy(lines[n], s, len);
        lines[n][len] = 0;
        n++;
        s = e ? e + 1 : s + len;
    }
    for (uint32_t i = 0; i < n; i++) {   /* labels first: addresses are 8 apart */
        char *colon = strchr(lines[i], ':');
        if (!colon) continue;
        *colon = 0;
        sscanf(lines[i], "%15s", labels[nlabels]);
        label_at[nlabels++] = 8 * (i + 1);
        memmove(lines[i], colon + 1, strlen(colon + 1) + 1);
    }
    memset(p, 0, sizeof *p);
    p->n = n;
    for (uint32_t i = 0; i < n; i++) {
        CfInsn *in = &p->ins[i];
        in->addr = 8 * (i + 1);
        char op[16] = "", arg[8][16];
        int nargs = sscanf(lines[i], "%15s %15s %15s %15s %15s %15s %15s %15s %15s", op, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5],
                           arg[6], arg[7]) - 1;
        size_t len = strlen(op);
        in->cond = CF_ALWAYS;
        if (len && op[len - 1] == '?') { in->cond = CF_MAYBE; op[--len] = 0; }
        else if (len && op[len - 1] == '!') { in->cond = CF_NEVER; op[--len] = 0; }
        static const char *const kinds[] = {"plain", "ssy", "pbk", "bra", "sync", "brk", "exit", "brx"};
        int kind = -1;
        for (int k = 0; k < 8; k++) if (!strcmp(op, kinds[k])) kind = k;
        if (kind < 0) return 0;
        in->kind = (uint8_t)kind;
        for (int a = 0; a < nargs; a++) {
            uint32_t l = 0;
            while (l < nlabels && strcmp(labels[l], arg[a])) l++;
            if (l == nlabels) return 0;
            if (kind == CF_BRX) p->tables[i][in->ntable++] = label_at[l];
            else in->target = label_at[l];
        }
        if (kind == CF_BRX) in->table = in->ntable ? p->tables[i] : NULL;
    }
    return 1;
}

/* ---- checking ---- */
typedef struct Rng { uint64_t s; int mode; } Rng;   /* mode 0 random, 1 always 0, 2 always 1 */

static uint32_t choice(void *user) {
    Rng *r = user;
    if (r->mode) return r->mode - 1;
    r->s ^= r->s << 13; r->s ^= r->s >> 7; r->s ^= r->s << 17;
    return (uint32_t)(r->s >> 32);
}

static int verbose;

/* 1 if the two runs agree; otherwise prints where they part. */
static int compare(const char *name, const Cfg *g, Rng seed, uint32_t limit) {
    CfgTrace d = {0}, t = {0};
    Rng r = seed;
    cfg_trace_dispatch(g, choice, &r, limit, &d);
    r = seed;
    cfg_trace_tree(g, choice, &r, limit, &t);
    int ok;
    uint32_t i = 0;
    while (i < d.n && i < t.n && d.addr[i] == t.addr[i]) i++;
    if (d.end == CFG_END_EXIT && t.end == CFG_END_EXIT) ok = d.n == t.n && i == d.n;
    else ok = i == (d.n < t.n ? d.n : t.n);   /* a guard stopped one: the other may have run further */
    if (!ok) {
        printf("MISMATCH %s: sequence %s seed %#llx, limit %u: block %u differs (dispatch %#x of %u%s, structured %#x of %u%s)\n", name,
               seed.mode ? (seed.mode == 1 ? "all-0" : "all-1") : "random", (unsigned long long)seed.s, limit, i,
               i < d.n ? d.addr[i] : 0, d.n, d.end == CFG_END_LIMIT ? " guard" : "", i < t.n ? t.addr[i] : 0, t.n,
               t.end == CFG_END_LIMIT ? " guard" : "");
    }
    free(d.addr);
    free(t.addr);
    return ok;
}

static int check_all(const char *name, const Cfg *g, int sequences) {
    int ok = 1;
    for (int k = 0; k < sequences && ok; k++) {
        Rng seed = {0x9E3779B97F4A7C15ull * (uint64_t)(k + 1) | 1, 0};
        ok &= compare(name, g, seed, CFG_LIMIT);
        if (ok && k % 8 == 0) ok &= compare(name, g, seed, 3 + k % 40);   /* small guards: where each stops */
    }
    for (int mode = 1; mode <= 2 && ok; mode++) {   /* conditions that never (always) hold: endless loops meet the guard */
        Rng seed = {1, mode};
        ok &= compare(name, g, seed, CFG_LIMIT);
        ok &= compare(name, g, seed, 17);
    }
    return ok;
}

static int structure(Cfg *g, const CfInsn *ins, uint32_t n) {
    cfg_blocks(g, ins, n);
    return cfg_resolve(g) && cfg_structure(g);
}

static int run_synth(void) {
    int failures = 0;
    for (size_t k = 0; k < sizeof SYNTH / sizeof *SYNTH; k++) {
        const Synth *s = &SYNTH[k];
        Prog p;
        if (!parse(s->text, &p)) { printf("FAIL %s: cannot parse\n", s->name); failures++; continue; }
        Cfg g;
        int ok = structure(&g, p.ins, p.n);
        const char *got = ok ? "structured" : g.reject;
        if (strcmp(got, s->expect)) { printf("FAIL %s: %s, expected %s\n", s->name, got, s->expect); failures++; }
        else if (ok && !check_all(s->name, &g, 400)) failures++;
        else printf("ok   %s: %s\n", s->name, got);
        cfg_free(&g);
    }
    printf("%d failure(s)\n", failures);
    return failures != 0;
}

static SNode *first_if(SNode *s) {
    if (!s) return NULL;
    if (s->kind == SN_IF && s->kid[0] != s->kid[1]) return s;
    for (uint32_t k = 0; k < s->n; k++) {
        SNode *f = first_if(s->kid[k]);
        if (f) return f;
    }
    return NULL;
}

/* The checker must notice a wrong tree: swap the arms of a program's first if. */
static int run_mutate(void) {
    int missed = 0, tried = 0;
    for (size_t k = 0; k < sizeof SYNTH / sizeof *SYNTH; k++) {
        Prog p;
        Cfg g;
        if (!parse(SYNTH[k].text, &p) || !structure(&g, p.ins, p.n)) { if (g.mem) cfg_free(&g); continue; }
        SNode *f = first_if(g.root);
        if (f) {
            SNode *x = f->kid[0];
            f->kid[0] = f->kid[1];
            f->kid[1] = x;
            tried++;
            printf("swapped an if in %s: ", SYNTH[k].name);
            fflush(stdout);
            if (check_all(SYNTH[k].name, &g, 50)) { printf("NOT NOTICED\n"); missed++; }
        }
        cfg_free(&g);
    }
    printf("%d mutation(s), %d not noticed\n", tried, missed);
    return missed != 0 || !tried;
}

/* Random programs: nested if/else (SSY/SYNC), loops (PBK/BRK, continues), exits, jump tables,
 * and now and then a stray branch to anywhere. Each must either be refused or run alike. */
typedef struct Gen { char text[16384]; size_t len; uint32_t insns, labels; Rng r; } Gen;

static void emitf(Gen *g, const char *fmt, const char *a, const char *b) {
    if (g->len + 64 >= sizeof g->text) return;
    g->len += (size_t)snprintf(g->text + g->len, sizeof g->text - g->len, fmt, a, b);
    g->insns++;
}

static uint32_t pick(Gen *g, uint32_t n) { return choice(&g->r) % n; }

static void gen_seq(Gen *g, int depth, const char *brk, const char *cont) {
    uint32_t items = 1 + pick(g, 4);
    for (uint32_t k = 0; k < items && g->insns < 200; k++) {
        char a[16], b[16];
        uint32_t what = pick(g, depth > 3 ? 4 : 10);
        switch (what) {
        case 0: case 1: emitf(g, " plain\n", "", ""); break;
        case 2: emitf(g, " exit?\n", "", ""); break;
        case 3:
            if (brk && pick(g, 2)) emitf(g, " brk?\n", "", "");
            else if (cont && pick(g, 2)) emitf(g, " bra? %s\n", cont, "");
            else emitf(g, " plain\n", "", "");
            break;
        case 4: case 5: case 6:   /* if/else */
            snprintf(a, sizeof a, "L%u", g->labels++);
            snprintf(b, sizeof b, "L%u", g->labels++);
            emitf(g, " ssy %s\n", b, "");
            emitf(g, " bra? %s\n", a, "");
            /* (a BRK or continue inside leaves the SSY entry behind: refused, as "stack", mostly) */
            gen_seq(g, depth + 1, pick(g, 8) ? NULL : brk, pick(g, 8) ? NULL : cont);
            emitf(g, " sync\n", "", "");
            emitf(g, " %s: plain\n", a, "");
            g->insns--;
            gen_seq(g, depth + 1, pick(g, 8) ? NULL : brk, pick(g, 8) ? NULL : cont);
            emitf(g, " sync\n", "", "");
            emitf(g, " %s: plain\n", b, "");
            break;
        case 7: case 8: {   /* loop */
            char h[16], e[16];
            snprintf(h, sizeof h, "L%u", g->labels++);
            snprintf(e, sizeof e, "L%u", g->labels++);
            emitf(g, " pbk %s\n", e, "");
            emitf(g, " %s: plain\n", h, "");
            emitf(g, " brk?\n", "", "");
            gen_seq(g, depth + 1, "brk", h);
            emitf(g, " bra %s\n", h, "");
            emitf(g, " %s: plain\n", e, "");
            break;
        }
        case 9:   /* a jump table over what follows */
            snprintf(a, sizeof a, "L%u", g->labels++);
            snprintf(b, sizeof b, "L%u", g->labels++);
            emitf(g, " brx %s %s\n", a, b);
            emitf(g, " %s: plain\n", a, "");
            emitf(g, " %s: plain\n", b, "");
            break;
        }
    }
}

static int run_fuzz(int count) {
    int structured = 0, failures = 0, refused[8] = {0};
    static const char *const reasons[] = {"stack", "stack-overflow", "stack-empty", "brx", "irreducible", "loop-exits", "size", "structure"};
    for (int k = 0; k < count; k++) {
        Gen *g = calloc(1, sizeof *g);
        g->r = (Rng){0x2545F4914F6CDD1Dull * (uint64_t)(k + 1) | 1, 0};
        gen_seq(g, 0, NULL, NULL);
        emitf(g, " exit\n", "", "");
        /* stray branches: to any label, from anywhere */
        Prog p;
        if (!parse(g->text, &p)) { printf("FAIL fuzz %d: cannot parse\n%s", k, g->text); failures++; free(g); continue; }
        uint32_t strays = pick(g, 4) == 0 ? 1 + pick(g, 2) : 0;
        for (uint32_t s = 0; s < strays && p.n > 2; s++) {
            uint32_t at = pick(g, p.n - 1), to = pick(g, p.n);
            if (p.ins[at].kind == CF_PLAIN) { p.ins[at].kind = CF_BRA; p.ins[at].cond = CF_MAYBE; p.ins[at].target = p.ins[to].addr; }
        }
        Cfg c;
        char name[32];
        snprintf(name, sizeof name, "fuzz %d", k);
        if (structure(&c, p.ins, p.n)) {
            structured++;
            if (!check_all(name, &c, 60)) { failures++; if (verbose) printf("%s", g->text); }
        } else
            for (int r = 0; r < 8; r++) refused[r] += !strcmp(c.reject, reasons[r]);
        cfg_free(&c);
        free(g);
    }
    printf("%d random programs: %d structured, %d failure(s); refused:", count, structured, failures);
    for (int r = 0; r < 8; r++) if (refused[r]) printf(" %s %d", reasons[r], refused[r]);
    printf("\n");
    return failures != 0 || refused[7] || !structured;
}

#ifdef WITH_TRANSLATOR
#include <dirent.h>
typedef uint32_t u32;
typedef uint8_t u8;
struct CfInsn *shader_flow(const u8 *program, u32 size, u32 (*read)(void *, u32, u32), void *user, u32 *n);
_Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

static int run_corpus(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { printf("cannot open %s\n", dir); return 2; }
    struct dirent *e;
    int total = 0, structured = 0, failures = 0;
    struct { char why[24]; int count; } reasons[16];
    int nreasons = 0;
    static unsigned char buf[0x40000];
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 4, ".bin")) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t size = fread(buf, 1, sizeof buf, f);
        fclose(f);
        u32 n;
        CfInsn *ins = shader_flow(buf, (u32)size, NULL, NULL, &n);
        int branchy = 0;
        for (u32 i = 0; i < n; i++) branchy |= ins[i].kind != CF_PLAIN && ins[i].kind != CF_EXIT;
        if (branchy) {
            total++;
            Cfg g;
            if (structure(&g, ins, n)) {
                structured++;
                if (!check_all(e->d_name, &g, 100)) failures++;
                else if (verbose) printf("ok   %s\n", e->d_name);
            } else {
                int r = 0;
                while (r < nreasons && strcmp(reasons[r].why, g.reject)) r++;
                if (r == nreasons && nreasons < 16) { snprintf(reasons[r].why, sizeof reasons[r].why, "%s", g.reject); reasons[nreasons++].count = 0; }
                if (r < nreasons) reasons[r].count++;
                if (verbose) printf("     %s: %s\n", e->d_name, g.reject);
            }
            cfg_free(&g);
        }
        free(ins);
    }
    closedir(d);
    printf("%d programs with branches: %d structured, traces identical in all but %d\n", total, structured, failures);
    for (int r = 0; r < nreasons; r++) printf("  dispatch (%s): %d\n", reasons[r].why, reasons[r].count);
    return failures != 0;
}
#endif

int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a], "-v")) verbose = 1;
    if (argc > 1 && !strcmp(argv[1], "synth")) return run_synth();
    if (argc > 1 && !strcmp(argv[1], "mutate")) return run_mutate();
    if (argc > 1 && !strcmp(argv[1], "fuzz")) return run_fuzz(argc > 2 ? atoi(argv[2]) : 2000);
#ifdef WITH_TRANSLATOR
    if (argc > 2 && !strcmp(argv[1], "corpus")) return run_corpus(argv[2]);
#endif
    fprintf(stderr, "usage: shader_structure_test synth | mutate | corpus <dir> [-v]\n");
    return 2;
}
