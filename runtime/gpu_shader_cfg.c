/* Control flow of a shader program: blocks, static SSY/PBK stacks, structure. See gpu_shader_cfg.h.
 *
 * Structuring, on the graph cfg_resolve leaves (blocks, each ending in a jump, a condition, a BRX
 * or the end):
 *   1. Depth-first search and dominators: a retreating edge to a block that does not dominate its
 *      source makes the graph irreducible.
 *   2. Natural loops, one per header, nested innermost first. A loop must leave to a single block
 *      (its follow). A block it leaves to that only leads to the end, and only from inside the
 *      loop, joins the loop instead (it returns from inside it).
 *   3. In each loop's body (and the program's), inner loops count as one node, an edge back to the
 *      header or out of the loop as a sink, an edge to the end as nothing (a return goes anywhere).
 *      That graph has no cycles: a condition's arms run up to its immediate post-dominator, where
 *      the sequence goes on. Blocks both arms reach before it are emitted in each (a budget caps it). */
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_cfg.h"

#define NONE 0xFFFFFFFFu
#define SINK 0xFFFFFFFEu   /* in a context's graph: leaving it (continue, break, return) */

/* ---- memory: everything a Cfg holds goes in its arena ---- */
typedef struct Chunk { struct Chunk *next; size_t used, cap; unsigned char data[]; } Chunk;

static void *alloc(Cfg *g, size_t n) {
    n = (n + 15) & ~(size_t)15;
    Chunk *c = g->mem;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;
        Chunk *k = malloc(sizeof *k + cap);
        if (!k) abort();
        k->next = c;
        k->used = 0;
        k->cap = cap;
        g->mem = c = k;
    }
    void *p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

void cfg_free(Cfg *g) {
    for (Chunk *c = g->mem, *next; c; c = next) { next = c->next; free(c); }
    memset(g, 0, sizeof *g);
}

/* ---- blocks ---- */
static int find_insn(const Cfg *g, uint32_t addr) {
    uint32_t lo = 0, hi = g->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g->ins[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g->n && g->ins[lo].addr == addr ? (int)lo : -1;
}

/* The block starting at an address: what the dispatch loop's switch has a case for. */
static uint32_t block_at(const Cfg *g, uint32_t addr) {
    uint32_t lo = 0, hi = g->nblocks;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g->block[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g->nblocks && g->block[lo].addr == addr ? lo : CFG_EXIT;
}

void cfg_blocks(Cfg *g, const CfInsn *ins, uint32_t n) {
    memset(g, 0, sizeof *g);
    g->ins = ins;
    g->n = n;
    g->leader = alloc(g, n + 1);
    g->ssy_target = alloc(g, n + 1);
    g->pbk_target = alloc(g, n + 1);
    if (!n) return;
    int unknown_table = 0;
    for (uint32_t i = 0; i < n; i++) unknown_table |= ins[i].kind == CF_BRX && !ins[i].ntable;
    g->leader[0] = 1;
    for (uint32_t i = 0; i < n; i++) {
        const CfInsn *in = &ins[i];
        if (unknown_table) g->leader[i] = 1;   /* a BRX could go anywhere */
        if (in->kind == CF_BRA || in->kind == CF_SSY || in->kind == CF_PBK) {
            int t = find_insn(g, in->target);
            if (t >= 0) {
                g->leader[t] = 1;
                if (in->kind == CF_SSY) g->ssy_target[t] = 1;
                if (in->kind == CF_PBK) g->pbk_target[t] = 1;
            }
        }
        if (in->kind == CF_BRX)
            for (uint32_t e = 0; e < in->ntable; e++) {
                int t = find_insn(g, in->table[e]);
                if (t >= 0) g->leader[t] = 1;
            }
        if ((in->kind == CF_BRA || in->kind == CF_BRX || in->kind == CF_SYNC || in->kind == CF_BRK || in->kind == CF_EXIT) && i + 1 < n)
            g->leader[i + 1] = 1;
    }
    for (uint32_t i = 0; i < n; i++) g->nblocks += g->leader[i];
    g->block = alloc(g, sizeof *g->block * g->nblocks);
    for (uint32_t i = 0, b = 0; i < n; i++) {
        if (!g->leader[i]) continue;
        if (b) g->block[b - 1].end = i;
        g->block[b] = (CfgBlock){ins[i].addr, i, n, -1, 0, NULL, 0};
        b++;
    }
}

/* ---- stacks ---- */
typedef struct Stacks { uint8_t ssp, psp; uint32_t ssy[16], pbk[16]; } Stacks;

/* The stacks as a block's code sees them: the dispatch loop's case first pops its own address
 * off the top of either (a block both SYNC and a branch reach, past its SSY, gets there with the
 * address pushed on one path and not on the other). */
static Stacks on_entry(const Cfg *g, uint32_t b, Stacks s) {
    const CfgBlock *bl = &g->block[b];
    if (g->ssy_target[bl->first] && s.ssp && s.ssy[s.ssp - 1] == bl->addr) s.ssp--;
    if (g->pbk_target[bl->first] && s.psp && s.pbk[s.psp - 1] == bl->addr) s.psp--;
    return s;
}

static int same_stacks(const Stacks *a, const Stacks *b) {
    return a->ssp == b->ssp && a->psp == b->psp && !memcmp(a->ssy, b->ssy, a->ssp * 4) && !memcmp(a->pbk, b->pbk, a->psp * 4);
}

int cfg_resolve(Cfg *g) {
    if (!g->nblocks) return 1;
    for (uint32_t i = 0; i < g->n; i++)
        if (g->ins[i].kind == CF_BRX && !g->ins[i].ntable) { g->reject = "brx"; return 0; }
    Stacks *entry = calloc(g->nblocks, sizeof *entry);
    uint32_t *work = malloc(sizeof *work * g->nblocks), nwork = 0;
    uint32_t *succ = malloc(sizeof *succ * (g->n + 2));
    Stacks *after = malloc(sizeof *after * (g->n + 2));
    g->block[0].reachable = 1;
    entry[0] = on_entry(g, 0, entry[0]);
    work[nwork++] = 0;
    while (nwork && !g->reject) {
        uint32_t b = work[--nwork];
        CfgBlock *bl = &g->block[b];
        Stacks s = entry[b];
        uint32_t ns = 0, next = b + 1 < g->nblocks ? b + 1 : CFG_EXIT;
        int ended = 0;
        for (uint32_t i = bl->first; i < bl->end && !ended && !g->reject; i++) {
            const CfInsn *in = &g->ins[i];
            if (in->kind == CF_PLAIN || in->cond == CF_NEVER) continue;
            uint32_t after_i = i + 1 < g->n ? block_at(g, g->ins[i + 1].addr) : CFG_EXIT;
            switch (in->kind) {
            case CF_SSY: case CF_PBK: {
                uint8_t *sp = in->kind == CF_SSY ? &s.ssp : &s.psp;
                if (*sp == 16) { g->reject = "stack-overflow"; break; }
                (in->kind == CF_SSY ? s.ssy : s.pbk)[(*sp)++] = in->target;
                break;
            }
            case CF_BRA:
                if (in->cond == CF_MAYBE) { bl->term = (int32_t)i; after[ns] = s; succ[ns++] = block_at(g, in->target); after[ns] = s; succ[ns++] = after_i; }
                else { after[ns] = s; succ[ns++] = block_at(g, in->target); }
                ended = 1;
                break;
            case CF_SYNC: case CF_BRK: {
                Stacks popped = s;
                uint8_t *sp = in->kind == CF_SYNC ? &popped.ssp : &popped.psp;
                if (!*sp) { g->reject = "stack-empty"; break; }
                uint32_t to = (in->kind == CF_SYNC ? popped.ssy : popped.pbk)[--*sp];
                after[ns] = popped; succ[ns++] = block_at(g, to);
                if (in->cond == CF_MAYBE) { bl->term = (int32_t)i; after[ns] = s; succ[ns++] = after_i; }
                ended = 1;
                break;
            }
            case CF_BRX:
                bl->term = (int32_t)i;
                for (uint32_t e = 0; e < in->ntable; e++) {
                    uint32_t t = block_at(g, in->table[e]), k = 0;
                    while (k < ns && succ[k] != t) k++;
                    if (k == ns) { after[ns] = s; succ[ns++] = t; }
                }
                ended = 1;
                break;
            case CF_EXIT:
                if (in->cond == CF_MAYBE) { bl->term = (int32_t)i; after[ns] = s; succ[ns++] = CFG_EXIT; after[ns] = s; succ[ns++] = after_i; }
                else { after[ns] = s; succ[ns++] = CFG_EXIT; }
                ended = 1;
                break;
            }
        }
        if (g->reject) break;
        if (!ended) { after[ns] = s; succ[ns++] = next; }
        bl->nsucc = ns;
        bl->succ = alloc(g, sizeof *bl->succ * ns);
        memcpy(bl->succ, succ, sizeof *succ * ns);
        for (uint32_t k = 0; k < ns && !g->reject; k++) {
            uint32_t t = succ[k];
            if (t == CFG_EXIT) continue;
            Stacks in = on_entry(g, t, after[k]);
            if (!g->block[t].reachable) { g->block[t].reachable = 1; entry[t] = in; work[nwork++] = t; }
            else if (!same_stacks(&entry[t], &in)) g->reject = "stack";
        }
    }
    free(entry);
    free(work);
    free(succ);
    free(after);
    return !g->reject;
}

/* ---- structure ---- */
typedef struct Loop {
    uint32_t header, size, follow;   /* follow: NONE if it is only left by returning */
    int parent;                      /* -1: the program */
    uint8_t *body;                   /* per block */
    uint32_t *ipdom;                 /* its graph's immediate post-dominators, per block */
    SNode *node;
    uint32_t emitted;                /* instructions its node emits */
} Loop;

typedef struct S {
    Cfg *g;
    uint32_t nb;
    uint32_t *idom, *rpo;            /* dominators; reverse postorder number per block (NONE: unreachable) */
    uint32_t *pred_start, *pred_end, *pred;
    Loop *loop;
    uint32_t nloops;
    int *innermost;                  /* per block: the smallest loop holding it, -1 none */
    uint8_t *is_header;
    uint32_t *top_ipdom;
    uint32_t budget, spent, max_succ;
    int failed;
} S;

static int fail(S *st, const char *why) {
    if (!st->failed) { st->failed = 1; st->g->reject = why; }
    return 0;
}

static int dominates(const S *st, uint32_t a, uint32_t b) {
    for (;;) {
        if (a == b) return 1;
        if (b == 0) return 0;
        b = st->idom[b];
    }
}

/* Reverse postorder and retreating edges, without recursion. */
static int dfs(S *st, uint32_t *order, uint32_t *nback, uint32_t *back_from, uint32_t *back_to) {
    Cfg *g = st->g;
    uint8_t *color = calloc(st->nb, 1);
    uint32_t *stack = malloc(sizeof *stack * st->nb), *pos = calloc(st->nb, sizeof *pos), depth = 0, done = st->nb;
    stack[depth++] = 0;
    color[0] = 1;
    *nback = 0;
    while (depth) {
        uint32_t b = stack[depth - 1];
        CfgBlock *bl = &g->block[b];
        if (pos[b] < bl->nsucc) {
            uint32_t t = bl->succ[pos[b]++];
            if (t == CFG_EXIT) continue;
            if (color[t] == 1) { back_from[*nback] = b; back_to[*nback] = t; ++*nback; }
            else if (!color[t]) { color[t] = 1; stack[depth++] = t; }
        } else {
            color[b] = 2;
            order[--done] = b;
            depth--;
        }
    }
    memmove(order, order + done, sizeof *order * (st->nb - done));
    uint32_t n = st->nb - done;
    free(color); free(stack); free(pos);
    return (int)n;
}

static void dominators(S *st, const uint32_t *order, uint32_t n) {
    Cfg *g = st->g;
    uint32_t nb = st->nb;
    for (uint32_t i = 0; i < nb; i++) { st->rpo[i] = NONE; st->idom[i] = NONE; }
    for (uint32_t i = 0; i < n; i++) st->rpo[order[i]] = i;
    /* predecessors (reachable ones, each once): pred[pred_start[b] .. pred_end[b]) */
    st->pred_start = calloc(nb + 1, sizeof *st->pred_start);
    st->pred_end = malloc(sizeof *st->pred_end * nb);
    for (uint32_t b = 0; b < nb; b++)
        if (st->rpo[b] != NONE)
            for (uint32_t k = 0; k < g->block[b].nsucc; k++)
                if (g->block[b].succ[k] != CFG_EXIT) st->pred_start[g->block[b].succ[k] + 1]++;
    for (uint32_t b = 0; b < nb; b++) st->pred_start[b + 1] += st->pred_start[b];
    st->pred = malloc(sizeof *st->pred * (st->pred_start[nb] + 1));
    memcpy(st->pred_end, st->pred_start, sizeof *st->pred_end * nb);
    for (uint32_t b = 0; b < nb; b++)
        if (st->rpo[b] != NONE)
            for (uint32_t k = 0; k < g->block[b].nsucc; k++) {
                uint32_t t = g->block[b].succ[k], j;
                if (t == CFG_EXIT) continue;
                for (j = st->pred_start[t]; j < st->pred_end[t] && st->pred[j] != b; j++) {}
                if (j == st->pred_end[t]) st->pred[st->pred_end[t]++] = b;
            }
    /* Cooper, Harvey and Kennedy's iteration, in reverse postorder */
    st->idom[0] = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t b = order[i], d = NONE;
            for (uint32_t j = st->pred_start[b]; j < st->pred_end[b]; j++) {
                uint32_t p = st->pred[j];
                if (st->idom[p] == NONE) continue;
                if (d == NONE) { d = p; continue; }
                uint32_t x = p, y = d;
                while (x != y) {
                    while (st->rpo[x] > st->rpo[y]) x = st->idom[x];
                    while (st->rpo[y] > st->rpo[x]) y = st->idom[y];
                }
                d = x;
            }
            if (d != st->idom[b]) { st->idom[b] = d; changed = 1; }
        }
    }
}

/* A block a loop leaves to that it can take in: everything from it leads only to the end, it is
 * the only way into all of that, and only the loop goes to it. */
static int absorbable(S *st, const uint8_t *body, uint32_t y, uint8_t *mark, uint32_t *list) {
    Cfg *g = st->g;
    for (uint32_t j = st->pred_start[y]; j < st->pred_end[y]; j++)
        if (!body[st->pred[j]]) return 0;
    uint32_t n = 0;
    int ok = 1;
    mark[y] = 1;
    list[n++] = y;
    for (uint32_t i = 0; i < n && ok; i++) {
        uint32_t d = list[i];
        if (st->is_header[d] || !dominates(st, y, d)) ok = 0;
        for (uint32_t k = 0; k < g->block[d].nsucc && ok; k++) {
            uint32_t t = g->block[d].succ[k];
            if (t != CFG_EXIT && !mark[t]) { mark[t] = 1; list[n++] = t; }
        }
    }
    for (uint32_t i = 0; i < n; i++) mark[list[i]] = 0;
    return ok;
}

static void absorb(S *st, uint8_t *body, uint32_t y, uint32_t *list) {
    Cfg *g = st->g;
    uint32_t n = 0;
    body[y] = 1;
    list[n++] = y;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < g->block[list[i]].nsucc; k++) {
            uint32_t t = g->block[list[i]].succ[k];
            if (t != CFG_EXIT && !body[t]) { body[t] = 1; list[n++] = t; }
        }
}

static int by_size(const void *a, const void *b) {
    const Loop *x = a, *y = b;
    return x->size != y->size ? (x->size < y->size ? -1 : 1) : (x->header < y->header ? -1 : x->header > y->header);
}

static int find_loops(S *st, uint32_t nback, const uint32_t *back_from, const uint32_t *back_to) {
    Cfg *g = st->g;
    uint32_t nb = st->nb;
    st->loop = calloc(nback + 1, sizeof *st->loop);
    uint32_t *list = malloc(sizeof *list * nb);
    for (uint32_t e = 0; e < nback; e++) {
        uint32_t h = back_to[e], l = 0;
        while (l < st->nloops && st->loop[l].header != h) l++;
        if (l == st->nloops) {
            st->loop[st->nloops++] = (Loop){h, 1, NONE, -1, alloc(g, nb), NULL, NULL, 0};
            st->loop[l].body[h] = 1;
            st->is_header[h] = 1;
        }
        Loop *L = &st->loop[l];
        uint32_t n = 0;
        if (!L->body[back_from[e]]) { L->body[back_from[e]] = 1; L->size++; list[n++] = back_from[e]; }
        while (n) {
            uint32_t b = list[--n];
            for (uint32_t j = st->pred_start[b]; j < st->pred_end[b]; j++) {
                uint32_t p = st->pred[j];
                if (!L->body[p]) { L->body[p] = 1; L->size++; list[n++] = p; }
            }
        }
    }
    qsort(st->loop, st->nloops, sizeof *st->loop, by_size);
    uint8_t *mark = calloc(nb, 1);
    uint32_t targets[64];
    for (uint32_t l = 0; l < st->nloops && !st->failed; l++) {
        Loop *L = &st->loop[l];
        for (uint32_t m = 0; m < l; m++)
            if (L->body[st->loop[m].header])
                for (uint32_t b = 0; b < nb; b++) L->body[b] |= st->loop[m].body[b];
        uint32_t nt = 0;
        for (uint32_t b = 0; b < nb; b++) {
            if (!L->body[b]) continue;
            for (uint32_t k = 0; k < g->block[b].nsucc; k++) {
                uint32_t t = g->block[b].succ[k], j = 0;
                if (t == CFG_EXIT || L->body[t]) continue;
                while (j < nt && targets[j] != t) j++;
                if (j == nt && nt == 64) fail(st, "loop-exits");
                else if (j == nt) targets[nt++] = t;
            }
        }
        uint32_t follow = NONE, kept = 0;
        for (uint32_t j = 0; j < nt; j++)
            if (!absorbable(st, L->body, targets[j], mark, list)) { follow = targets[j]; kept++; }
        if (kept > 1 || st->failed) { fail(st, "loop-exits"); break; }
        if (!kept)   /* every way out only returns: the furthest one stays the follow */
            for (uint32_t j = 0; j < nt; j++)
                if (follow == NONE || g->block[targets[j]].addr > g->block[follow].addr) follow = targets[j];
        for (uint32_t j = 0; j < nt; j++)
            if (targets[j] != follow) absorb(st, L->body, targets[j], list);
        L->follow = follow;
        L->size = 0;
        for (uint32_t b = 0; b < nb; b++) L->size += L->body[b];
    }
    free(mark);
    free(list);
    if (st->failed) return 0;
    qsort(st->loop, st->nloops, sizeof *st->loop, by_size);   /* (sizes changed: inner ones still first) */
    for (uint32_t b = 0; b < nb; b++) st->innermost[b] = -1;
    for (uint32_t l = 0; l < st->nloops; l++) {
        Loop *L = &st->loop[l];
        for (uint32_t b = 0; b < nb; b++)
            if (L->body[b] && st->innermost[b] < 0) st->innermost[b] = (int)l;
        for (uint32_t m = l + 1; m < st->nloops && L->parent < 0; m++)
            if (st->loop[m].body[L->header]) L->parent = (int)m;
    }
    return 1;
}

/* ---- a context's graph: a loop's body (or the program), inner loops as their headers ---- */
static int in_body(const S *st, int ctx, uint32_t b) { return ctx < 0 ? st->g->block[b].reachable : st->loop[ctx].body[b]; }

/* The loop directly inside ctx that holds b, -1 if none, -2 if b is inside one but not its header. */
static int child_loop(const S *st, int ctx, uint32_t b) {
    int l = st->innermost[b];
    if (l == ctx) return -1;
    while (l >= 0 && st->loop[l].parent != ctx) l = st->loop[l].parent;
    if (l < 0) return -2;
    return st->loop[l].header == b ? l : -2;
}

/* Where an edge to t goes in ctx's graph: NONE (the end), SINK (out of the context), or a node. */
static uint32_t rep(S *st, int ctx, uint32_t t) {
    if (t == CFG_EXIT) return NONE;
    if (ctx >= 0 && t == st->loop[ctx].header) return SINK;
    if (!in_body(st, ctx, t)) {
        if (ctx < 0 || t != st->loop[ctx].follow) fail(st, "structure");
        return SINK;
    }
    if (child_loop(st, ctx, t) == -2) fail(st, "structure");
    return t;
}

static uint32_t node_succ(S *st, int ctx, uint32_t b, int is_root, uint32_t *out) {
    uint32_t n = 0;
    int l = is_root ? -1 : child_loop(st, ctx, b);
    if (l >= 0) {
        if (st->loop[l].follow != NONE) out[n++] = rep(st, ctx, st->loop[l].follow);
    } else {
        const CfgBlock *bl = &st->g->block[b];
        for (uint32_t k = 0; k < bl->nsucc; k++) {
            uint32_t r = rep(st, ctx, bl->succ[k]), j = 0;
            if (r == NONE) continue;
            while (j < n && out[j] != r) j++;
            if (j == n) out[n++] = r;
        }
    }
    if (!n) out[n++] = SINK;
    return n;
}

/* Immediate post-dominators in ctx's graph (acyclic): in postorder, where the successors'
 * post-dominator chains meet (SINK numbered 0, every node above its successors). */
static uint32_t pnum(const uint32_t *num, uint32_t x) { return x == SINK ? 0 : num[x]; }

static uint32_t *post_dominators(S *st, int ctx) {
    Cfg *g = st->g;
    uint32_t nb = st->nb;
    uint32_t *ipdom = alloc(g, sizeof *ipdom * nb), *num = malloc(sizeof *num * nb), *stack = malloc(sizeof *stack * nb);
    uint32_t *pos = calloc(nb, sizeof *pos), *sstart = malloc(sizeof *sstart * nb), *scount = malloc(sizeof *scount * nb);
    uint32_t cap = 4 * nb + 64, used = 0, depth = 0, count = 0, *sbuf = malloc(sizeof *sbuf * cap);
    uint8_t *color = calloc(nb, 1);
    for (uint32_t b = 0; b < nb; b++) { ipdom[b] = NONE; num[b] = NONE; }
    uint32_t root = ctx < 0 ? rep(st, ctx, 0) : st->loop[ctx].header;
    for (uint32_t t = root, is_root = 1; !st->failed;) {   /* visit t: its successors, onto the stack */
        color[t] = 1;
        if (used + st->max_succ + 1 > cap) sbuf = realloc(sbuf, sizeof *sbuf * (cap = cap * 2 + st->max_succ + 1));
        sstart[t] = used;
        scount[t] = node_succ(st, ctx, t, is_root && ctx >= 0, sbuf + used);
        used += scount[t];
        stack[depth++] = t;
        is_root = 0;
        t = NONE;
        while (depth && t == NONE && !st->failed) {
            uint32_t b = stack[depth - 1];
            if (pos[b] < scount[b]) {
                uint32_t x = sbuf[sstart[b] + pos[b]++];
                if (x == SINK || color[x] == 2) continue;
                if (color[x] == 1) fail(st, "structure");   /* a cycle: not expected once loops are nodes */
                else t = x;
                continue;
            }
            color[b] = 2;
            num[b] = ++count;
            uint32_t d = NONE;
            for (uint32_t k = 0; k < scount[b]; k++) {
                uint32_t x = sbuf[sstart[b] + k];
                if (d == NONE) { d = x; continue; }
                uint32_t y = d;
                while (x != y) {
                    while (pnum(num, x) > pnum(num, y)) x = ipdom[x];
                    while (pnum(num, y) > pnum(num, x)) y = ipdom[y];
                }
                d = x;
            }
            ipdom[b] = d;
            depth--;
        }
        if (t == NONE) break;
    }
    free(num); free(stack); free(pos); free(sbuf); free(sstart); free(scount); free(color);
    return ipdom;
}

static uint32_t ipdom_of(S *st, int ctx, uint32_t b) {
    uint32_t **slot = ctx < 0 ? &st->top_ipdom : &st->loop[ctx].ipdom;
    if (!*slot) *slot = post_dominators(st, ctx);
    return (*slot)[b];
}

/* ---- the tree ---- */
typedef struct Vec { SNode **v; uint32_t n, cap; } Vec;

static SNode *node(S *st, int kind) {
    SNode *s = alloc(st->g, sizeof *s);
    s->kind = (uint8_t)kind;
    return s;
}

static void push(Vec *v, SNode *x) {
    if (v->n == v->cap) v->v = realloc(v->v, sizeof *v->v * (v->cap = v->cap * 2 + 8));
    v->v[v->n++] = x;
}

static SNode *seal(S *st, Vec *v) {
    SNode *s = node(st, SN_SEQ);
    s->n = v->n;
    s->kid = alloc(st->g, sizeof *s->kid * (v->n + 1));
    if (v->n) memcpy(s->kid, v->v, sizeof *v->v * v->n);
    free(v->v);
    return s;
}

static void spend(S *st, uint32_t n) {
    st->spent += n;
    if (st->spent > st->budget) fail(st, "size");
}

static SNode *build_loop(S *st, int l);

/* The sequence from block t on, in ctx, up to `stop` (a node of ctx's graph, or SINK). */
static void emit_from(S *st, int ctx, Vec *seq, uint32_t t, uint32_t stop, int entering) {
    Cfg *g = st->g;
    while (!st->failed) {
        if (t == CFG_EXIT) { push(seq, node(st, SN_RETURN)); return; }
        if (ctx >= 0 && t == st->loop[ctx].header && !entering) { push(seq, node(st, SN_CONTINUE)); return; }
        if (!in_body(st, ctx, t)) {
            if (ctx < 0 || t != st->loop[ctx].follow) { fail(st, "structure"); return; }
            push(seq, node(st, SN_BREAK));
            return;
        }
        if (t == stop) return;
        int l = entering ? -1 : child_loop(st, ctx, t);
        entering = 0;
        if (l == -2) { fail(st, "structure"); return; }
        if (l >= 0) {
            SNode *ln = build_loop(st, l);
            if (!ln) return;
            push(seq, ln);
            if (st->loop[l].follow == NONE) return;   /* (the loop ends in a return) */
            t = st->loop[l].follow;
            continue;
        }
        const CfgBlock *bl = &g->block[t];
        SNode *bn = node(st, SN_BLOCK);
        bn->block = t;
        bn->size = bl->end - bl->first;
        push(seq, bn);
        spend(st, bn->size + 1);
        if (bl->term < 0) { t = bl->succ[0]; continue; }
        uint32_t j = ipdom_of(st, ctx, t);
        if (j == NONE) fail(st, "structure");
        if (st->failed) return;
        SNode *k = node(st, g->ins[bl->term].kind == CF_BRX ? SN_SWITCH : SN_IF);
        k->block = t;
        k->n = bl->nsucc;
        k->kid = alloc(g, sizeof *k->kid * bl->nsucc);
        for (uint32_t s = 0; s < bl->nsucc; s++) {
            Vec arm = {0};
            emit_from(st, ctx, &arm, bl->succ[s], j, 0);
            k->kid[s] = seal(st, &arm);
        }
        push(seq, k);
        if (j == SINK) return;
        t = j;
    }
}

static SNode *build_loop(S *st, int l) {
    Loop *L = &st->loop[l];
    if (L->node) { spend(st, L->emitted); return st->failed ? NULL : L->node; }
    uint32_t before = st->spent;
    SNode *ln = node(st, SN_LOOP);
    Vec body = {0};
    emit_from(st, l, &body, L->header, SINK, 1);
    ln->n = 1;
    ln->kid = alloc(st->g, sizeof *ln->kid);
    ln->kid[0] = seal(st, &body);
    ln->breaks = L->follow != NONE;
    L->emitted = st->spent - before;
    L->node = ln;
    return st->failed ? NULL : ln;
}

int cfg_structure(Cfg *g) {
    if (g->reject) return 0;
    if (!g->nblocks) { g->root = alloc(g, sizeof *g->root); return 1; }
    S st = {g, g->nblocks};
    st.idom = malloc(sizeof *st.idom * st.nb);
    st.rpo = malloc(sizeof *st.rpo * st.nb);
    st.innermost = malloc(sizeof *st.innermost * st.nb);
    st.is_header = calloc(st.nb, 1);
    uint32_t edges = 0;
    for (uint32_t b = 0; b < st.nb; b++) {
        edges += g->block[b].nsucc;
        if (g->block[b].nsucc > st.max_succ) st.max_succ = g->block[b].nsucc;
    }
    uint32_t *order = malloc(sizeof *order * st.nb), *back_from = malloc(sizeof *back_from * (edges + 1)), *back_to = malloc(sizeof *back_to * (edges + 1));
    uint32_t nback = 0;
    uint32_t n = (uint32_t)dfs(&st, order, &nback, back_from, back_to);
    dominators(&st, order, n);
    for (uint32_t e = 0; e < nback && !st.failed; e++)
        if (!dominates(&st, back_to[e], back_from[e])) fail(&st, "irreducible");
    if (!st.failed) find_loops(&st, nback, back_from, back_to);
    if (!st.failed) {
        st.budget = g->n * 2 + 256;   /* blocks emitted more than once: at most about the program again */
        Vec top = {0};
        emit_from(&st, -1, &top, 0, SINK, 0);
        g->root = seal(&st, &top);
    }
    free(order); free(back_from); free(back_to);
    free(st.idom); free(st.rpo); free(st.innermost); free(st.is_header);
    free(st.pred_start); free(st.pred_end); free(st.pred);
    free(st.loop);
    if (st.failed) g->root = NULL;
    return !st.failed;
}

/* ---- traces ---- */
static void trace_add(CfgTrace *t, uint32_t addr) {
    if (t->n == t->cap) t->addr = realloc(t->addr, sizeof *t->addr * (t->cap = t->cap * 2 + 256));
    t->addr[t->n++] = addr;
}

void cfg_trace_dispatch(const Cfg *g, CfgChoice *next, void *user, uint32_t limit, CfgTrace *out) {
    out->n = 0;
    out->end = CFG_END_LIMIT;
    if (!g->n) { out->end = CFG_END_EXIT; return; }
    uint32_t pc = g->ins[0].addr, ssy[16] = {0}, pbk[16] = {0}, ssp = 0, psp = 0;
    for (uint32_t guard = 0; guard < limit; guard++) {
        uint32_t b = block_at(g, pc);
        if (b == CFG_EXIT) { out->end = CFG_END_EXIT; return; }   /* the switch's default */
        const CfgBlock *bl = &g->block[b];
        trace_add(out, pc);
        if (g->ssy_target[bl->first] && ssp > 0 && ssy[(ssp - 1) & 15] == pc) ssp--;
        if (g->pbk_target[bl->first] && psp > 0 && pbk[(psp - 1) & 15] == pc) psp--;
        uint32_t npc = pc;
        int ended = 0;
        for (uint32_t i = bl->first; i < bl->end && !ended; i++) {
            const CfInsn *in = &g->ins[i];
            if (in->kind == CF_PLAIN || in->cond == CF_NEVER) continue;
            uint32_t after = i + 1 < g->n ? g->ins[i + 1].addr : 0xFFFFFFFFu;
            switch (in->kind) {
            case CF_SSY: ssy[ssp & 15] = in->target; ssp++; break;
            case CF_PBK: pbk[psp & 15] = in->target; psp++; break;
            case CF_BRA:
                npc = in->cond == CF_ALWAYS || (next(user) & 1) ? in->target : after;
                ended = 1;
                break;
            case CF_SYNC: case CF_BRK: {
                uint32_t *sp = in->kind == CF_SYNC ? &ssp : &psp, *stack = in->kind == CF_SYNC ? ssy : pbk;
                if (in->cond == CF_ALWAYS || (next(user) & 1)) { *sp = *sp - 1; npc = stack[*sp & 15]; }
                else npc = after;
                ended = 1;
                break;
            }
            case CF_BRX:
                npc = in->table[next(user) % in->ntable];
                ended = 1;
                break;
            case CF_EXIT:
                if (in->cond == CF_ALWAYS || (next(user) & 1)) { out->end = CFG_END_EXIT; return; }
                break;
            }
        }
        if (!ended) {
            if (b + 1 == g->nblocks) { out->end = CFG_END_EXIT; return; }
            npc = g->block[b + 1].addr;
        }
        pc = npc;
    }
}

typedef struct Run {
    const Cfg *g;
    CfgChoice *next;
    void *user;
    uint32_t limit;
    CfgTrace *out;
    uint8_t *run;       /* the innermost loop's flag */
    int done;
} Run;

static void exec(Run *r, const SNode *s) {
    const Cfg *g = r->g;
    switch (s->kind) {
    case SN_SEQ:
        for (uint32_t k = 0; k < s->n && !r->done; k++) exec(r, s->kid[k]);
        break;
    case SN_BLOCK: trace_add(r->out, g->block[s->block].addr); break;
    case SN_IF: exec(r, s->kid[(r->next(r->user) & 1) ? 0 : 1]); break;
    case SN_SWITCH: {
        const CfgBlock *bl = &g->block[s->block];
        const CfInsn *in = &g->ins[bl->term];
        uint32_t to = block_at(g, in->table[r->next(r->user) % in->ntable]), k = 0;
        while (k < bl->nsucc && bl->succ[k] != to) k++;
        if (k < bl->nsucc) exec(r, s->kid[k]);
        else { r->out->end = CFG_END_EXIT; r->done = 1; }
        break;
    }
    case SN_LOOP: {
        uint8_t run = 1, *outer = r->run;
        r->run = &run;
        for (uint32_t i = 0; run && i < r->limit && !r->done; i++) exec(r, s->kid[0]);
        r->run = outer;
        if (!r->done && run) { r->out->end = CFG_END_LIMIT; r->done = 1; }   /* the guard: then the program's end */
        break;
    }
    case SN_CONTINUE: break;
    case SN_BREAK: *r->run = 0; break;
    case SN_RETURN: r->out->end = CFG_END_EXIT; r->done = 1; break;
    }
}

void cfg_trace_tree(const Cfg *g, CfgChoice *next, void *user, uint32_t limit, CfgTrace *out) {
    Run r = {g, next, user, limit, out, NULL, 0};
    out->n = 0;
    out->end = CFG_END_EXIT;
    if (g->root) exec(&r, g->root);
    if (!r.done) out->end = CFG_END_EXIT;
}
