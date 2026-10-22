/* Maxwell shader programs as instructions (gpu_shader_decode.h): what the translators share. */
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_decode.h"

static const char *const UNKNOWN = "?";
static const struct { const char *name, *pattern; } OPS[] = {
    {"?", ""},
#define ROW(name, pat) {#name, pat},
    OPCODES(ROW)
#undef ROW
};
static struct { u16 mask, value, op, bits; } table[OP_COUNT];
static u32 table_size;

static int by_specificity(const void *a, const void *b) {
    return (int)((const __typeof__(table[0]) *)b)->bits - (int)((const __typeof__(table[0]) *)a)->bits;
}

void shader_decode_init(void) {
    if (table_size) return;
    for (u32 i = 1; i < OP_COUNT; i++) {
        u16 mask = 0, value = 0;
        for (int b = 0; b < 16; b++) {
            char ch = OPS[i].pattern[b];
            mask = (u16)(mask << 1 | (ch != '-'));
            value = (u16)(value << 1 | (ch == '1'));
        }
        table[table_size++] = (__typeof__(table[0])){mask, value, (u16)i, (u16)__builtin_popcount(mask)};
    }
    qsort(table, table_size, sizeof table[0], by_specificity);
}

u32 shader_op_decode(u64 w) {
    shader_decode_init();
    u16 top = (u16)(w >> 48);
    for (u32 i = 0; i < table_size; i++)
        if ((top & table[i].mask) == table[i].value) return table[i].op;
    return OP_UNKNOWN;
}

const char *shader_op_name(u32 op) { return op < OP_COUNT ? OPS[op].name : UNKNOWN; }

/* ---- operands ---------------------------------------------------------------------- */
u32 shader_imm20(u64 w) { u32 v = BITS(w, 20, 19); return BITS(w, 56, 1) ? v | 0xFFF80000u : v; }
u32 shader_fimm20(u64 w) { return BITS(w, 20, 19) << 12 | BITS(w, 56, 1) << 31; }

int shader_form_of(u32 op) {
    const char *n = shader_op_name(op), *u = strrchr(n, '_');
    size_t len = strlen(n);
    if (len > 3 && !strcmp(n + len - 3, "32I")) return FORM_32I;   /* IADD32I, FMUL32I, HADD2_32I, ... */
    if (!u) return FORM_REG;
    if (!strcmp(u, "_reg")) return FORM_REG;
    if (!strcmp(u, "_cbuf")) return FORM_CBUF;
    if (!strcmp(u, "_imm")) return FORM_IMM;
    if (!strcmp(u, "_rc")) return FORM_RC;
    if (!strcmp(u, "_cr")) return FORM_CR;
    if (!strcmp(u, "_32I")) return FORM_32I;
    return FORM_REG;
}

/* ---- control flow ------------------------------------------------------------------- */
i32 shader_branch_offset(u64 w) { i32 o = (i32)BITS(w, 20, 24); return o & 0x800000 ? o - 0x1000000 : o; }

u32 shader_align_target(u32 t) { return (t & 31) == 0x10 ? t + 8 : t; }
u32 shader_branch_target(const ShaderInsn *in) { return shader_align_target(in->addr + 8 + (u32)shader_branch_offset(in->w)); }

int shader_is_branchy(u32 op) {
    return op == OP_BRA || op == OP_BRX || op == OP_SSY || op == OP_PBK || op == OP_SYNC || op == OP_BRK;
}

int shader_flow_test(u32 test) {
    switch (test) {
    case 15: return SHADER_TEST_ALWAYS;
    case 0: case 16: case 18: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29: return SHADER_TEST_NEVER;
    case 1: case 2: case 3: case 4: case 5: case 6: case 9: case 10: case 11: case 12: case 13: case 14:
    case 17: case 19: case 20: case 22: case 30: case 31: return SHADER_TEST_CC;
    }
    return SHADER_TEST_NEVER_CC;   /* NUM, NaN */
}

ShaderInsn *shader_decode_program(const u8 *prog, u32 size, u32 *count) {
    u32 cap = 256, furthest = 0, n = 0;
    ShaderInsn *ins = malloc(sizeof *ins * cap);
    for (u32 a = 0x50; a + 8 <= size; a += 8) {
        if (((a - 0x50) / 8) % 4 == 0) continue;   /* scheduling control word */
        u64 w = *(const u64 *)(prog + a);
        if (w == 0xE2400FFFFF87000Full) break;
        if (n == cap) ins = realloc(ins, sizeof *ins * (cap *= 2));
        ShaderInsn in = {a, w, shader_op_decode(w)};
        ins[n++] = in;
        if (in.op == OP_BRA || in.op == OP_SSY || in.op == OP_PBK) {
            u32 t = shader_branch_target(&in);
            if (t > furthest) furthest = t;
        }
        if (in.op == OP_BRX) furthest = size;
        if (in.op == OP_EXIT && BITS(w, 16, 4) == 7 && BITS(w, 0, 5) == 15 && a >= furthest) break;
    }
    *count = n;
    return ins;
}

static int find(const ShaderInsn *ins, u32 n, u32 addr) {
    for (u32 i = 0; i < n; i++) if (ins[i].addr == addr) return (int)i;
    return -1;
}

/* Jump tables of indirect branches. The compiler emits
 *     IMNMX.U32 rI, rI, N-1 ;  SHL rJ, rI, 2 ;  LDC rT, c[bank][rJ + off] ;  BRX rT - k
 * (rJ often rI) with the table's entries relative to the BRX. If every BRX of the program resolves, only
 * their targets need to be blocks; otherwise every instruction must be one. A resolved BRX's
 * targets go in its CfInsn. */
static int brx_resolved(const ShaderInsn *ins, u32 n, CfInsn *flow, ShaderInfo *info, CbufReader *read, void *user, u32 **targets,
                        u32 *ntargets) {
    if (!read) return 0;
    int all = 1;
    for (u32 i = 0; i < n; i++) {
        if (ins[i].op != OP_BRX) continue;
        u32 r = BITS(ins[i].w, 8, 8), bank = 0, off = 0, index_reg = RZ, count = 0;
        int j = (int)i - 1, found = 0;
        for (; j >= 0 && j > (int)i - 24; j--)
            if (ins[j].op == OP_LDC && BITS(ins[j].w, 0, 8) == r) {
                bank = BITS(ins[j].w, 36, 5);
                off = (u32)((i32)(BITS(ins[j].w, 20, 16) << 16) >> 16);
                index_reg = BITS(ins[j].w, 8, 8);
                found = 1;
                break;
            }
        for (int k = j - 1; found && k >= 0 && k > j - 24; k--)
            if (ins[k].op == OP_SHL_imm && BITS(ins[k].w, 0, 8) == index_reg) index_reg = BITS(ins[k].w, 8, 8);   /* (the index, scaled) */
            else if ((ins[k].op == OP_IMNMX_imm) && BITS(ins[k].w, 0, 8) == index_reg) { count = shader_imm20(ins[k].w) + 1; break; }
        if (!found || !count || count > 256) { all = 0; continue; }
        if (info->ntables < 4) {
            info->table[info->ntables].bank = bank; info->table[info->ntables].offset = off;
            info->table[info->ntables++].count = count;
        }
        u32 first = *ntargets;
        *targets = realloc(*targets, sizeof **targets * (*ntargets + count));
        for (u32 e = 0; e < count; e++) {
            u32 t = shader_align_target(read(user, bank, off + 4 * e) + ins[i].addr + 8 + (u32)shader_branch_offset(ins[i].w));
            if (find(ins, n, t) < 0) { all = 0; break; }
            (*targets)[(*ntargets)++] = t;
        }
        if (*ntargets - first == count) flow[i].ntable = count;
        else *ntargets = first;
        flow[i].table = (const u32 *)(uintptr_t)first;   /* (an index until the last table is in) */
    }
    for (u32 i = 0; i < n; i++)
        if (flow[i].ntable) flow[i].table = *targets + (uintptr_t)flow[i].table;
        else flow[i].table = NULL;
    return all;
}

static int flow_kind(u32 op) {
    switch (op) {
    case OP_SSY: return CF_SSY;
    case OP_PBK: return CF_PBK;
    case OP_BRA: return CF_BRA;
    case OP_SYNC: return CF_SYNC;
    case OP_BRK: return CF_BRK;
    case OP_EXIT: return CF_EXIT;
    case OP_BRX: return CF_BRX;
    }
    return CF_PLAIN;
}

/* What each instruction does to control flow, as the translators act on it. */
CfInsn *shader_describe_flow(const ShaderInsn *ins, u32 n, ShaderInfo *info, CbufReader *read, void *user, u32 **targets, u32 *ntargets,
                             int *reads_cc) {
    CfInsn *flow = calloc(n + 1, sizeof *flow);
    int brx = 0;
    *reads_cc = 0;
    for (u32 i = 0; i < n; i++) {
        const ShaderInsn *in = &ins[i];
        CfInsn *f = &flow[i];
        f->addr = in->addr;
        f->kind = (u8)flow_kind(in->op);
        if (f->kind == CF_PLAIN) continue;
        brx |= f->kind == CF_BRX;
        if (f->kind == CF_SSY || f->kind == CF_PBK || f->kind == CF_BRA) f->target = shader_branch_target(in);
        u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
        if (p == PT && neg) { f->cond = CF_NEVER; continue; }
        int test = shader_flow_test(BITS(in->w, 0, 5)), taken = test == SHADER_TEST_ALWAYS || test == SHADER_TEST_CC;
        *reads_cc |= test == SHADER_TEST_CC || test == SHADER_TEST_NEVER_CC;
        if (f->kind == CF_SSY || f->kind == CF_PBK || f->kind == CF_BRX) f->cond = CF_ALWAYS;   /* (whatever the predicate) */
        else if (!taken) f->cond = CF_NEVER;
        else f->cond = p == PT && test == SHADER_TEST_ALWAYS ? CF_ALWAYS : CF_MAYBE;
    }
    if (brx) brx_resolved(ins, n, flow, info, read, user, targets, ntargets);
    return flow;
}

/* For tests (gpu_shader.h): a program's instructions as gpu_shader_cfg.h describes their control flow. */
CfInsn *shader_flow(const u8 *program, u32 size, CbufReader *read, void *user, u32 *n) {
    ShaderInfo info = {0};
    u32 count, *targets = NULL, ntargets = 0;
    int reads_cc;
    ShaderInsn *ins = shader_decode_program(program, size, &count);
    CfInsn *flow = shader_describe_flow(ins, count, &info, read, user, &targets, &ntargets, &reads_cc);
    CfInsn *out = malloc(sizeof *out * (count + 1) + sizeof *targets * ntargets);
    u32 *copy = (u32 *)(out + count + 1);
    memcpy(out, flow, sizeof *out * (count + 1));
    if (ntargets) memcpy(copy, targets, sizeof *copy * ntargets);
    for (u32 i = 0; i < count; i++)
        if (out[i].table) out[i].table = copy + (out[i].table - targets);
    *n = count;
    free(ins);
    free(flow);
    free(targets);
    return out;
}
