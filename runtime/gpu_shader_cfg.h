/* Control flow of a shader program, for the translator (gpu_shader.c).
 *
 * The translator describes what each instruction does to control flow (a CfInsn); the Maxwell
 * encoding stays there. From that, this file finds the program's blocks as the dispatch loop has
 * them (cfg_blocks), resolves SYNC and BRK statically by following the SSY/PBK stacks along every
 * path (cfg_resolve), and turns the resulting graph into a tree of blocks, if/else, loops and
 * returns (cfg_structure) - or says why it cannot, and the program stays a dispatch loop.
 *
 * The two traces at the end run a program both ways, the dispatch loop's and the tree's, for a
 * given sequence of condition outcomes: tests compare them (tests/test_shader_structure.py). */
#pragma once
#include <stdint.h>

enum { CF_PLAIN, CF_SSY, CF_PBK, CF_BRA, CF_SYNC, CF_BRK, CF_EXIT, CF_BRX };
/* Whether a flow instruction acts: its predicate and flow test. SSY and PBK push whatever their
 * predicate says (as the dispatch loop does), unless it is !PT: CF_NEVER. */
enum { CF_NEVER, CF_ALWAYS, CF_MAYBE };

typedef struct CfInsn {
    uint32_t addr;
    uint8_t kind, cond;          /* CF_*, CF_NEVER/ALWAYS/MAYBE */
    uint32_t target;             /* SSY, PBK, BRA: the address */
    uint32_t ntable;             /* BRX: its jump table, as addresses (0 entries: not known) */
    const uint32_t *table;
} CfInsn;

#define CFG_EXIT  0xFFFFFFFFu    /* a successor: the program's end */
#define CFG_LIMIT 1000000u       /* the iteration guard: the dispatch loop's, and each structured loop's */

typedef struct CfgBlock {
    uint32_t addr, first, end;   /* its instructions: [first, end) */
    int32_t term;                /* the instruction whose outcome picks the successor (a condition, BRX), or -1 */
    uint32_t nsucc, *succ;       /* successor blocks (CFG_EXIT: the end); a condition's: taken, then not */
    uint8_t reachable;
} CfgBlock;

/* The structure: SEQ runs its kids in order; IF the first kid when the block's last instruction
 * acts, the second when not; SWITCH (a BRX) the kid of the successor its table entry is; LOOP its
 * body until a BREAK (or the guard); CONTINUE ends the iteration, RETURN the program. */
enum { SN_SEQ, SN_BLOCK, SN_IF, SN_SWITCH, SN_LOOP, SN_CONTINUE, SN_BREAK, SN_RETURN };
typedef struct SNode {
    uint8_t kind;
    uint8_t breaks;              /* LOOP: it has a BREAK (else it is only left by returning) */
    uint32_t block;              /* BLOCK; IF, SWITCH: the block whose last instruction decides */
    uint32_t n, size;            /* kids; instructions, as emitted (LOOP: once known) */
    struct SNode **kid;
} SNode;

typedef struct Cfg {
    const CfInsn *ins;
    uint32_t n;
    uint8_t *leader, *ssy_target, *pbk_target;   /* per instruction, as the dispatch loop has them */
    uint32_t nblocks;
    CfgBlock *block;
    SNode *root;                 /* cfg_structure's tree */
    const char *reject;          /* why not: stack, stack-overflow, stack-empty, brx, irreducible, loop-exits, size, structure */
    void *mem;
} Cfg;

/* The blocks: every instruction a branch goes to, or after one, starts one. */
void cfg_blocks(Cfg *g, const CfInsn *ins, uint32_t n);
/* Successors of every reachable block, with SYNC and BRK resolved from the stacks. 0 (and
 * g->reject) when a stack differs between two paths into a block, overflows, or is popped empty,
 * or a BRX's table is not known. */
int cfg_resolve(Cfg *g);
/* The tree (g->root), or 0 and g->reject. After cfg_resolve. */
int cfg_structure(Cfg *g);
void cfg_free(Cfg *g);

/* ---- traces ---- */
typedef uint32_t CfgChoice(void *user);   /* the next outcome: bit 0 for a condition, the value for a BRX's index */
enum { CFG_END_EXIT = 1, CFG_END_LIMIT };
typedef struct CfgTrace { uint32_t *addr, n, cap; int end; } CfgTrace;   /* blocks run, by address; how it ended */

/* As the dispatch loop runs it: pc, the stacks, the guard counting blocks. */
void cfg_trace_dispatch(const Cfg *g, CfgChoice *next, void *user, uint32_t limit, CfgTrace *out);
/* As the structured HLSL runs it: loops with a run flag and a guard of their own. */
void cfg_trace_tree(const Cfg *g, CfgChoice *next, void *user, uint32_t limit, CfgTrace *out);
