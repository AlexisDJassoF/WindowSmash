/* Maxwell shader programs straight to SPIR-V (gpu_spirv.h), on the instruction decoding the HLSL
 * translator shares (gpu_shader_decode.h). Each instruction does here what gpu_shader.c's emit_insn
 * writes in HLSL, on SPIR-V values instead of text, and notes the same things in ShaderInfo.
 *
 * Registers and predicates are Function variables, loaded and stored around each instruction (the
 * driver's compiler turns them into values); a guarded instruction is an if. Floating point values
 * are the registers' bits (OpBitcast). Values every block may need (the vertex attributes, the
 * pixel inputs) are made at the start of the first block (spv_prologue_*).
 *
 * The floating point operations keep Direct3D's results where Vulkan leaves them undefined: min and
 * max and saturate return the operand that is not NaN (NMin, NMax, NClamp), and conversions to
 * integers clamp (NaN to 0), as Maxwell's do. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_spirv.h"
#include "gpu_spirv_builder.h"
#include "gpu_shader_decode.h"
#include <spirv/unified1/GLSL.std.450.h>

typedef struct Sx {
    Spv b;
    int stage;
    const u8 *prog;
    ShaderInsn *ins;
    u32 n;
    ShaderInfo *info;
    u32 vs_outputs, local[3], shared;
    u64 vin;                                      /* vertex: attributes as vertex inputs (shader_spirv_ex), 0: from the streams */
    CbufReader *read_cbuf;
    void *read_user;
    int branchy;
    /* types */
    u32 t_void, t_bool, t_u32, t_i32, t_f32, t_v2u, t_v3u, t_v4u, t_v4f, t_v3f, t_v2f;
    u32 p_func_u32, p_func_bool, p_func_f32, p_func_v4f, p_sb_u32, p_uni_u32, p_uni_f32, p_in_f32, p_in_v4f, p_out_f32, p_out_v4f;
    u32 c_true, c_false, c_u0, c_f0, c_f1;
    /* variables */
    u32 reg[256], pred[8], cc_c, cc_z, cc_s;
    u32 sb_struct, cbuf_var[18], stream_var[32], params, tex_var[32];
    u32 gload_fn, gstore_fn, smem, lmem;          /* global memory's functions; shared and local memory */
    u32 decode_fn;                                /* a vertex attribute's decoding */
    u32 vid, iid, frag_coord, front, local_id, group_id;
    u32 in_attr[32], out_attr[32], out_pos, out_psize, opos, out_color[8], out_depth;
    u32 attr_value[32][4];          /* vertex: the decoded attributes (in the prologue) */
    u32 out_attr_mask;
    int uses_gmem;                  /* LDG/STG: the storage buffers ShaderInfo notes are used */
    u32 iface[128], niface;
    u32 main_fn;
    int open;                       /* the current block has no terminator yet */
    CfInsn *flow;                   /* (programs with branches) what each instruction does to control flow */
    Cfg *cfg;
    u32 loop_run;                   /* structured: the innermost loop's run flag */
} Sx;

/* ---- small things ------------------------------------------------------------------- */
#define B (&c->b)
static u32 U(Sx *c, u32 v) { return spv_const_u32(B, v); }
static u32 I(Sx *c, i32 v) { return spv_const_i32(B, v); }
static u32 Fc(Sx *c, float v) { return spv_const_f32(B, v); }

static void unsupported(Sx *c, const ShaderInsn *in, const char *why) {
    if (!c->info->unsupported[0])
        snprintf(c->info->unsupported, sizeof c->info->unsupported, "%s at %#x (%016llx): %s", shader_op_name(in->op), in->addr,
                 (unsigned long long)in->w, why);
}

/* The entry point's interface: its Input and Output variables (SPIR-V 1.3 lists only those). */
static void add_interface(Sx *c, u32 var) {
    if (c->niface < 128) c->iface[c->niface++] = var;
}

static u32 bitcast(Sx *c, u32 type, u32 v) { return SPV_VAL(B, SpvOpBitcast, type, v); }
static u32 asfloat(Sx *c, u32 v) { return bitcast(c, c->t_f32, v); }
static u32 asuint(Sx *c, u32 v) { return bitcast(c, c->t_u32, v); }
static u32 asint(Sx *c, u32 v) { return bitcast(c, c->t_i32, v); }
static u32 sel(Sx *c, u32 type, u32 cond, u32 a, u32 b) { return SPV_VAL(B, SpvOpSelect, type, cond, a, b); }
static u32 op2(Sx *c, SpvOp op, u32 type, u32 a, u32 b) { return SPV_VAL(B, op, type, a, b); }
static u32 iadd(Sx *c, u32 a, u32 b) { return op2(c, SpvOpIAdd, c->t_u32, a, b); }
static u32 isub(Sx *c, u32 a, u32 b) { return op2(c, SpvOpISub, c->t_u32, a, b); }
static u32 band(Sx *c, u32 a, u32 b) { return op2(c, SpvOpBitwiseAnd, c->t_u32, a, b); }
static u32 bor(Sx *c, u32 a, u32 b) { return op2(c, SpvOpBitwiseOr, c->t_u32, a, b); }
static u32 bxor(Sx *c, u32 a, u32 b) { return op2(c, SpvOpBitwiseXor, c->t_u32, a, b); }
static u32 bnot(Sx *c, u32 a) { return SPV_VAL(B, SpvOpNot, c->t_u32, a); }
static u32 shl(Sx *c, u32 a, u32 s) { return op2(c, SpvOpShiftLeftLogical, c->t_u32, a, s); }
static u32 shr(Sx *c, u32 a, u32 s) { return op2(c, SpvOpShiftRightLogical, c->t_u32, a, s); }
static u32 sar(Sx *c, u32 a, u32 s) { return asuint(c, op2(c, SpvOpShiftRightArithmetic, c->t_i32, asint(c, a), s)); }
static u32 ieq(Sx *c, u32 a, u32 b) { return op2(c, SpvOpIEqual, c->t_bool, a, b); }
static u32 ult(Sx *c, u32 a, u32 b) { return op2(c, SpvOpULessThan, c->t_bool, a, b); }
static u32 uge(Sx *c, u32 a, u32 b) { return op2(c, SpvOpUGreaterThanEqual, c->t_bool, a, b); }
static u32 lnot(Sx *c, u32 a) { return SPV_VAL(B, SpvOpLogicalNot, c->t_bool, a); }
static u32 land(Sx *c, u32 a, u32 b) { return op2(c, SpvOpLogicalAnd, c->t_bool, a, b); }
static u32 lor(Sx *c, u32 a, u32 b) { return op2(c, SpvOpLogicalOr, c->t_bool, a, b); }
static u32 f_add(Sx *c, u32 a, u32 b) { return op2(c, SpvOpFAdd, c->t_f32, a, b); }
static u32 f_mul(Sx *c, u32 a, u32 b) { return op2(c, SpvOpFMul, c->t_f32, a, b); }
static u32 isnan_(Sx *c, u32 a) { return SPV_VAL(B, SpvOpIsNan, c->t_bool, a); }
static u32 glsl1(Sx *c, u32 type, u32 inst, u32 a) { return SPV_GLSL(B, type, inst, a); }
static u32 glsl2(Sx *c, u32 type, u32 inst, u32 a, u32 b) { return SPV_GLSL(B, type, inst, a, b); }
static u32 glsl3(Sx *c, u32 type, u32 inst, u32 a, u32 b, u32 d) { return SPV_GLSL(B, type, inst, a, b, d); }
static u32 saturate(Sx *c, u32 x) { return glsl3(c, c->t_f32, GLSLstd450NClamp, x, c->c_f0, c->c_f1); }
static u32 f16tof32(Sx *c, u32 x) {   /* the low half's value */
    u32 v2 = glsl1(c, c->t_v2f, GLSLstd450UnpackHalf2x16, x);
    return SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, v2, 0);
}
static u32 f32tof16(Sx *c, u32 x) {   /* (in the low half, the high half 0) */
    u32 v2 = SPV_VAL(B, SpvOpCompositeConstruct, c->t_v2f, x, c->c_f0);
    return glsl1(c, c->t_u32, GLSLstd450PackHalf2x16, v2);
}

/* Blocks: label starts one, the terminators end it. Code after a terminator (after a return, in
 * the tree's unreachable places) goes to a new block: resume. */
static void label(Sx *c, u32 id) { spv_label(B, id); c->open = 1; }
static void branch(Sx *c, u32 to) { if (c->open) SPV_DO(B, SpvOpBranch, to); c->open = 0; }
static void resume(Sx *c) { if (!c->open) label(c, spv_id(B)); }

/* if (cond) { ... }: begin returns the block after it, which end starts */
static u32 if_begin(Sx *c, u32 cond) {
    u32 then = spv_id(B), merge = spv_id(B);
    resume(c);
    SPV_DO(B, SpvOpSelectionMerge, merge, SpvSelectionControlMaskNone);
    SPV_DO(B, SpvOpBranchConditional, cond, then, merge);
    label(c, then);
    return merge;
}
static void if_end(Sx *c, u32 merge) {
    branch(c, merge);
    label(c, merge);
}
/* if (cond) { ... } else { ... }: the else's block, and the merge */
static void if_else_begin(Sx *c, u32 cond, u32 *other, u32 *merge) {
    u32 then = spv_id(B);
    *other = spv_id(B);
    *merge = spv_id(B);
    resume(c);
    SPV_DO(B, SpvOpSelectionMerge, *merge, SpvSelectionControlMaskNone);
    SPV_DO(B, SpvOpBranchConditional, cond, then, *other);
    label(c, then);
}
static void if_else_middle(Sx *c, u32 other, u32 merge) {
    branch(c, merge);
    label(c, other);
}

/* ---- registers and predicates ---------------------------------------------------------- */
/* Registers: RZ reads as zero; writes to it vanish. */
static u32 reg_var(Sx *c, u32 r) {
    if (!c->reg[r]) c->reg[r] = spv_local_init(B, c->p_func_u32, c->c_u0);
    return c->reg[r];
}
static u32 R(Sx *c, u32 r) { return r == RZ ? c->c_u0 : SPV_VAL(B, SpvOpLoad, c->t_u32, reg_var(c, r)); }
static u32 F(Sx *c, u32 r) { return r == RZ ? c->c_f0 : asfloat(c, R(c, r)); }
static void set_u(Sx *c, u32 r, u32 v) {
    if (r != RZ) SPV_DO(B, SpvOpStore, reg_var(c, r), v);
}
static void set_f(Sx *c, u32 r, u32 v) {
    if (r != RZ) set_u(c, r, asuint(c, v));
}
static u32 PRED(Sx *c, u32 p, int neg) {
    if (p == PT) return neg ? c->c_false : c->c_true;
    if (!c->pred[p]) c->pred[p] = spv_local_init(B, c->p_func_bool, c->c_false);
    u32 v = SPV_VAL(B, SpvOpLoad, c->t_bool, c->pred[p]);
    return neg ? lnot(c, v) : v;
}
static void set_p(Sx *c, u32 p, u32 v) {
    if (p == PT) return;
    if (!c->pred[p]) c->pred[p] = spv_local_init(B, c->p_func_bool, c->c_false);
    SPV_DO(B, SpvOpStore, c->pred[p], v);
}
/* the condition codes: carry (0 or 1), zero, sign */
static void make_cc(Sx *c) {
    if (c->cc_c) return;
    c->cc_c = spv_local_init(B, c->p_func_u32, c->c_u0);
    c->cc_z = spv_local_init(B, c->p_func_bool, c->c_false);
    c->cc_s = spv_local_init(B, c->p_func_bool, c->c_false);
}
/* (the variable by its place: it may not exist before make_cc) */
static u32 cc_load(Sx *c, const u32 *var, u32 type) { make_cc(c); return SPV_VAL(B, SpvOpLoad, type, *var); }

/* ---- resources ------------------------------------------------------------------------- */
static void decorate(Sx *c, u32 id, SpvDecoration d, u32 v) { spv_decorate(B, id, d, 1, &v); }
static void decorate0(Sx *c, u32 id, SpvDecoration d) { spv_decorate(B, id, d, 0, NULL); }

/* A storage buffer of words: a struct with a runtime array of uint. */
static u32 word_struct(Sx *c) {
    if (!c->sb_struct) {
        u32 arr = spv_type_runtime_array(B, c->t_u32);
        decorate(c, arr, SpvDecorationArrayStride, 4);
        c->sb_struct = spv_type_struct(B, 1, &arr);
        decorate0(c, c->sb_struct, SpvDecorationBlock);
        spv_member_decorate(B, c->sb_struct, 0, SpvDecorationOffset, 1, (u32[]){0});
    }
    return c->sb_struct;
}

/* A read-only storage buffer of words at (set, binding). */
static u32 word_buffer(Sx *c, u32 set, u32 binding) {
    u32 v = spv_global(B, spv_type_pointer(B, SpvStorageClassStorageBuffer, word_struct(c)), SpvStorageClassStorageBuffer);
    decorate(c, v, SpvDecorationDescriptorSet, set);
    decorate(c, v, SpvDecorationBinding, binding);
    decorate0(c, v, SpvDecorationNonWritable);
    return v;
}
static u32 load_word(Sx *c, u32 buffer, u32 index) {
    u32 p = SPV_VAL(B, SpvOpAccessChain, c->p_sb_u32, buffer, c->c_u0, index);
    return SPV_VAL(B, SpvOpLoad, c->t_u32, p);
}

static u32 cbuf_var(Sx *c, u32 bank) {
    if (!c->cbuf_var[bank]) c->cbuf_var[bank] = word_buffer(c, SPIRV_SET_BUFFERS, (c->stage == SHADER_PIXEL ? SPIRV_CBUF_BINDING_PIXEL : 0) + bank);
    return c->cbuf_var[bank];
}

/* The word of constant buffer bank at a byte offset (as the HLSL translator's cbuf: noted alike). */
static u32 cbuf(Sx *c, u32 bank, u32 offset) {
    c->info->cbuf_mask |= 1u << bank;
    if (c->info->cbuf_extent[bank] < (offset & ~3u) + 4) c->info->cbuf_extent[bank] = (offset & ~3u) + 4;
    if (bank == 0 && offset >= SB_FIRST && offset < SB_END) {   /* a storage buffer descriptor: note it */
        u32 o = offset & ~15u, k = 0;
        while (k < c->info->sb_count && c->info->sb_offset[k] != o) k++;
        if (k == c->info->sb_count && k < 16) c->info->sb_offset[c->info->sb_count++] = o;
    }
    return load_word(c, cbuf_var(c, bank), U(c, offset >> 2));
}
/* (at a byte address computed by the program) */
static u32 cbuf_at(Sx *c, u32 bank, u32 addr) { return load_word(c, cbuf_var(c, bank), shr(c, addr, U(c, 2))); }

/* DrawParams (gpu_shader.h) as Vulkan's std140 lays it out: every member 16-byte aligned. */
enum { P_ATTR, P_FIX, P_FRAG, P_BASE, P_SWZ, P_SB, P_RTSCALE, P_TEXSCALE };
static void make_params(Sx *c) {
    if (c->params) return;
    u32 a32 = spv_type_array(B, c->t_v4u, 32), a16u = spv_type_array(B, c->t_v4u, 16), a16f = spv_type_array(B, c->t_v4f, 16);
    decorate(c, a32, SpvDecorationArrayStride, 16);
    decorate(c, a16u, SpvDecorationArrayStride, 16);
    decorate(c, a16f, SpvDecorationArrayStride, 16);
    u32 m[8] = {a32, c->t_v4f, c->t_v4f, c->t_v4u, a16u, a32, c->t_v4f, a16f};
    static const u32 offsets[8] = {0, 512, 528, 544, 560, 816, 1328, 1344};
    u32 s = spv_type_struct(B, 8, m);
    decorate0(c, s, SpvDecorationBlock);
    for (u32 i = 0; i < 8; i++) spv_member_decorate(B, s, i, SpvDecorationOffset, 1, &offsets[i]);
    c->params = spv_global(B, spv_type_pointer(B, SpvStorageClassUniform, s), SpvStorageClassUniform);
    decorate(c, c->params, SpvDecorationDescriptorSet, SPIRV_SET_PARAMS);
    decorate(c, c->params, SpvDecorationBinding, 0);
}
/* One component of a DrawParams member (index: the array element, or ~0u for none). */
static u32 param(Sx *c, u32 member, u32 index, u32 comp, int is_float) {
    make_params(c);
    u32 p = index == ~0u ? SPV_VAL(B, SpvOpAccessChain, is_float ? c->p_uni_f32 : c->p_uni_u32, c->params, U(c, member), U(c, comp))
                         : SPV_VAL(B, SpvOpAccessChain, is_float ? c->p_uni_f32 : c->p_uni_u32, c->params, U(c, member), index, U(c, comp));
    return SPV_VAL(B, SpvOpLoad, is_float ? c->t_f32 : c->t_u32, p);
}

/* ---- operands ---------------------------------------------------------------------------- */
/* Operand B, as uint. */
static u32 b_u(Sx *c, u64 w, int form) {
    switch (form) {
    case FORM_REG: return R(c, BITS(w, 20, 8));
    case FORM_CBUF: case FORM_CR: return cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4);
    case FORM_RC: return R(c, BITS(w, 39, 8));
    case FORM_IMM: return U(c, shader_imm20(w));
    case FORM_32I: return U(c, BITS(w, 20, 32));
    }
    return c->c_u0;
}
static u32 b_f(Sx *c, u64 w, int form) {
    if (form == FORM_IMM) return spv_const_f32_bits(B, shader_fimm20(w));
    return asfloat(c, b_u(c, w, form));
}
/* Operand C of three-operand instructions, as uint. */
static u32 c_u(Sx *c, u64 w, int form) {
    if (form == FORM_RC) return cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4);
    return R(c, BITS(w, 39, 8));
}
static u32 absneg(Sx *c, u32 x, int abs, int neg) {
    if (abs) x = glsl1(c, c->t_f32, GLSLstd450FAbs, x);
    if (neg) x = SPV_VAL(B, SpvOpFNegate, c->t_f32, x);
    return x;
}

/* ---- comparisons --------------------------------------------------------------------------- */
/* (as the HLSL translator's: < <= > >= == ordered, != and the negated forms unordered) */
static u32 fcompare(Sx *c, u32 op, u32 a, u32 b) {
    static const SpvOp ops[16] = {0, SpvOpFOrdLessThan, SpvOpFOrdEqual, SpvOpFOrdLessThanEqual, SpvOpFOrdGreaterThan, SpvOpFOrdNotEqual,
                                  SpvOpFOrdGreaterThanEqual, SpvOpOrdered, SpvOpUnordered, SpvOpFUnordLessThan, SpvOpFUnordEqual,
                                  SpvOpFUnordLessThanEqual, SpvOpFUnordGreaterThan, SpvOpFUnordNotEqual, SpvOpFUnordGreaterThanEqual, 0};
    if (op == 0) return c->c_false;
    if (op >= 15) return c->c_true;
    return op2(c, ops[op], c->t_bool, a, b);
}
static u32 icompare(Sx *c, u32 op, int is_signed, u32 a, u32 b) {
    static const SpvOp u[7] = {0, SpvOpULessThan, SpvOpIEqual, SpvOpULessThanEqual, SpvOpUGreaterThan, SpvOpINotEqual, SpvOpUGreaterThanEqual};
    static const SpvOp s[7] = {0, SpvOpSLessThan, SpvOpIEqual, SpvOpSLessThanEqual, SpvOpSGreaterThan, SpvOpINotEqual, SpvOpSGreaterThanEqual};
    if (op == 0) return c->c_false;
    if (op == 7) return c->c_true;
    return op2(c, (is_signed ? s : u)[op], c->t_bool, a, b);
}
static u32 combine(Sx *c, u32 bop, u32 x, u32 pred) {
    if (pred == c->c_true && bop == 0) return x;
    static const SpvOp ops[3] = {SpvOpLogicalAnd, SpvOpLogicalOr, SpvOpLogicalNotEqual};
    return op2(c, ops[bop < 3 ? bop : 0], c->t_bool, x, pred);
}
/* 0x3F800000 (1.0) or 0xFFFFFFFF for true, 0 for false */
static u32 bool_word(Sx *c, u32 cond, int one) { return sel(c, c->t_u32, cond, U(c, one ? 0x3F800000u : 0xFFFFFFFFu), c->c_u0); }

/* A control instruction's flow test on the condition codes (gpu_shader_decode.h's shader_flow_test):
 * the value, or 0 when it can never pass. */
static u32 flow_cond(Sx *c, u32 test) {
    switch (shader_flow_test(test)) {
    case SHADER_TEST_ALWAYS: return c->c_true;
    case SHADER_TEST_NEVER: return 0;
    case SHADER_TEST_NEVER_CC: make_cc(c); return 0;
    }
    u32 s = cc_load(c, &c->cc_s, c->t_bool), z = cc_load(c, &c->cc_z, c->t_bool), carry = cc_load(c, &c->cc_c, c->t_u32);
    u32 c0 = ieq(c, carry, c->c_u0);
    switch (test) {
    case 1: case 9: return s;                                    /* LT */
    case 2: case 10: return z;                                   /* EQ */
    case 3: case 11: case 30: return lor(c, s, z);               /* LE, RLE */
    case 4: case 12: case 31: return land(c, lnot(c, s), lnot(c, z));   /* GT, RGT */
    case 5: case 13: return lnot(c, z);                          /* NE */
    case 6: case 14: return lnot(c, s);                          /* GE */
    case 17: return c0;                                          /* LO: carry clear */
    case 19: return lor(c, c0, z);                               /* LS */
    case 20: return land(c, lnot(c, c0), lnot(c, z));            /* HI */
    case 22: return lnot(c, c0);                                 /* HS */
    }
    return 0;
}

/* ---- conversions ------------------------------------------------------------------------------- */
/* float to int as Maxwell's (and Direct3D's): clamped to the type's range, NaN 0 */
static u32 f2i(Sx *c, u32 x, int is_signed) {
    if (is_signed) {
        u32 v = asuint(c, SPV_VAL(B, SpvOpConvertFToS, c->t_i32, glsl3(c, c->t_f32, GLSLstd450FClamp, x, Fc(c, -2147483648.0f), Fc(c, 2147483520.0f))));
        v = sel(c, c->t_u32, op2(c, SpvOpFOrdGreaterThanEqual, c->t_bool, x, Fc(c, 2147483648.0f)), U(c, 0x7FFFFFFFu), v);
        return sel(c, c->t_u32, isnan_(c, x), c->c_u0, v);
    }
    u32 v = SPV_VAL(B, SpvOpConvertFToU, c->t_u32, glsl3(c, c->t_f32, GLSLstd450FClamp, x, c->c_f0, Fc(c, 4294967040.0f)));
    v = sel(c, c->t_u32, op2(c, SpvOpFOrdGreaterThanEqual, c->t_bool, x, Fc(c, 4294967296.0f)), U(c, 0xFFFFFFFFu), v);
    return sel(c, c->t_u32, isnan_(c, x), c->c_u0, v);
}
/* the low bits of x sign-extended: ((int)(x << (32 - bits - sh))) >> (32 - bits) */
static u32 sext(Sx *c, u32 x, u32 left, u32 right) { return sar(c, shl(c, x, U(c, left)), U(c, right)); }

/* ---- half precision ------------------------------------------------------------------------- */
static u32 half(Sx *c, u32 x, u32 swizzle, int hi) {   /* one half of a packed pair, as float */
    switch (swizzle) {
    case 0: return f16tof32(c, hi ? shr(c, x, U(c, 16)) : x);   /* H1_H0 */
    case 1: return asfloat(c, x);                                /* F32 */
    case 2: return f16tof32(c, x);                               /* H0_H0 */
    }
    return f16tof32(c, shr(c, x, U(c, 16)));                     /* H1_H1 */
}
static void merge_halves(Sx *c, u32 d, u32 lo, u32 hi, u32 merge) {
    if (d == RZ) return;
    switch (merge) {
    case 0: set_u(c, d, bor(c, f32tof16(c, lo), shl(c, f32tof16(c, hi), U(c, 16)))); break;
    case 1: set_f(c, d, lo); break;
    case 2: set_u(c, d, bor(c, band(c, R(c, d), U(c, 0xFFFF0000u)), f32tof16(c, lo))); break;
    default: set_u(c, d, bor(c, band(c, R(c, d), U(c, 0x0000FFFFu)), shl(c, f32tof16(c, hi), U(c, 16)))); break;
    }
}
static u32 half_imm(Sx *c, u64 w) { return U(c, BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31); }

/* Half-precision pair operations: kind 0 add, 1 mul, 2 fma (as gpu_shader.c's emit_half). */
static void emit_half(Sx *c, const ShaderInsn *in, int kind) {
    u64 w = in->w;
    int form = shader_form_of(in->op);
    u32 d = BITS(w, 0, 8);
    u32 a = R(c, BITS(w, 8, 8)), b, cc = 0;
    u32 swa = BITS(w, 47, 2), swb = 0, swc = 0, merge = BITS(w, 49, 2), sat = 0;
    int abs_a = 0, neg_a = 0, abs_b = 0, neg_b = 0, neg_c = 0;
    if (kind == 0) {   /* HADD2 */
        neg_a = BITS(w, 43, 1); abs_a = BITS(w, 44, 1);
        switch (form) {
        case FORM_REG: b = R(c, BITS(w, 20, 8)); sat = BITS(w, 32, 1); neg_b = BITS(w, 31, 1); abs_b = BITS(w, 30, 1); swb = BITS(w, 28, 2); break;
        case FORM_CBUF: b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); sat = BITS(w, 52, 1); abs_b = BITS(w, 54, 1); neg_b = BITS(w, 56, 1); swb = 1; break;
        case FORM_IMM: b = half_imm(c, w); sat = BITS(w, 52, 1); break;
        default: b = U(c, BITS(w, 20, 32)); sat = BITS(w, 52, 1); neg_a = BITS(w, 56, 1); abs_a = 0; swa = BITS(w, 53, 2); merge = 0; break;
        }
    } else if (kind == 1) {   /* HMUL2 */
        abs_a = BITS(w, 44, 1);
        switch (form) {
        case FORM_REG: b = R(c, BITS(w, 20, 8)); sat = BITS(w, 32, 1); neg_b = BITS(w, 31, 1); abs_b = BITS(w, 30, 1); swb = BITS(w, 28, 2); break;
        case FORM_CBUF: b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); sat = BITS(w, 52, 1); abs_b = BITS(w, 54, 1); neg_a = BITS(w, 43, 1); swb = 1; break;
        case FORM_IMM: b = half_imm(c, w); sat = BITS(w, 52, 1); neg_a = BITS(w, 43, 1); break;
        default: b = U(c, BITS(w, 20, 32)); sat = BITS(w, 52, 1); abs_a = 0; swa = BITS(w, 53, 2); merge = 0; break;
        }
    } else {   /* HFMA2 */
        switch (form) {
        case FORM_REG:
            b = R(c, BITS(w, 20, 8)); cc = R(c, BITS(w, 39, 8)); swb = BITS(w, 28, 2); neg_b = BITS(w, 31, 1);
            neg_c = BITS(w, 30, 1); sat = BITS(w, 32, 1); swc = BITS(w, 35, 2); break;
        case FORM_RC:
            b = R(c, BITS(w, 39, 8)); cc = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); neg_c = BITS(w, 51, 1);
            sat = BITS(w, 52, 1); swb = BITS(w, 53, 2); neg_b = BITS(w, 56, 1); swc = 1; break;
        case FORM_CR:
            b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); cc = R(c, BITS(w, 39, 8)); neg_c = BITS(w, 51, 1);
            sat = BITS(w, 52, 1); swc = BITS(w, 53, 2); neg_b = BITS(w, 56, 1); swb = 1; break;
        case FORM_IMM:
            b = half_imm(c, w); cc = R(c, BITS(w, 39, 8)); neg_c = BITS(w, 51, 1); sat = BITS(w, 52, 1); swc = BITS(w, 53, 2); break;
        default:
            b = U(c, BITS(w, 20, 32)); cc = R(c, BITS(w, 0, 8)); neg_c = BITS(w, 52, 1); swa = BITS(w, 53, 2); merge = 0; break;
        }
    }
    u32 a0 = absneg(c, half(c, a, swa, 0), abs_a, neg_a), a1 = absneg(c, half(c, a, swa, 1), abs_a, neg_a);
    u32 b0 = absneg(c, half(c, b, swb, 0), abs_b, neg_b), b1 = absneg(c, half(c, b, swb, 1), abs_b, neg_b);
    u32 lo, hi;
    if (kind == 0) { lo = f_add(c, a0, b0); hi = f_add(c, a1, b1); }
    else if (kind == 1) { lo = f_mul(c, a0, b0); hi = f_mul(c, a1, b1); }
    else {
        u32 c0 = absneg(c, half(c, cc, swc, 0), 0, neg_c), c1 = absneg(c, half(c, cc, swc, 1), 0, neg_c);
        lo = glsl3(c, c->t_f32, GLSLstd450Fma, a0, b0, c0); hi = glsl3(c, c->t_f32, GLSLstd450Fma, a1, b1, c1);
    }
    if (sat) { lo = saturate(c, lo); hi = saturate(c, hi); }
    merge_halves(c, d, lo, hi, merge);
}

static void emit_xmad(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    int form = shader_form_of(in->op);
    u32 d = BITS(w, 0, 8), sa = BITS(w, 48, 1), sb = BITS(w, 49, 1), ha = BITS(w, 53, 1);
    u32 hb = 0, psl = 0, mrg = 0, x = 0, mode = 0, b, cc;
    switch (form) {
    case FORM_REG: hb = BITS(w, 35, 1); psl = BITS(w, 36, 1); mrg = BITS(w, 37, 1); x = BITS(w, 38, 1); mode = BITS(w, 50, 3);
        b = R(c, BITS(w, 20, 8)); cc = R(c, BITS(w, 39, 8)); break;
    case FORM_RC: mode = BITS(w, 50, 2); hb = BITS(w, 52, 1); x = BITS(w, 54, 1);
        b = R(c, BITS(w, 39, 8)); cc = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); break;
    case FORM_CR: mode = BITS(w, 50, 2); hb = BITS(w, 52, 1); x = BITS(w, 54, 1); psl = BITS(w, 55, 1); mrg = BITS(w, 56, 1);
        b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); cc = R(c, BITS(w, 39, 8)); break;
    default: psl = BITS(w, 36, 1); mrg = BITS(w, 37, 1); x = BITS(w, 38, 1); mode = BITS(w, 50, 3);
        b = U(c, BITS(w, 20, 16)); cc = R(c, BITS(w, 39, 8)); break;
    }
    u32 a = R(c, BITS(w, 8, 8));
    u32 oa = sa ? sext(c, a, ha ? 0 : 16, 16) : ha ? shr(c, a, U(c, 16)) : band(c, a, U(c, 0xFFFF));
    u32 ob = sb ? sext(c, b, hb ? 0 : 16, 16) : hb ? shr(c, b, U(c, 16)) : band(c, b, U(c, 0xFFFF));
    u32 oc;
    switch (mode) {
    case 1: oc = band(c, cc, U(c, 0xFFFF)); break;
    case 2: oc = shr(c, cc, U(c, 16)); break;
    case 4: oc = iadd(c, cc, shl(c, b, U(c, 16))); break;
    default: oc = cc; break;
    }
    u32 prod = op2(c, SpvOpIMul, c->t_u32, oa, ob);
    if (psl) prod = shl(c, prod, U(c, 16));
    u32 sum = iadd(c, prod, oc);
    if (x) sum = iadd(c, sum, cc_load(c, &c->cc_c, c->t_u32));
    if (mrg) sum = bor(c, band(c, sum, U(c, 0xFFFF)), shl(c, b, U(c, 16)));
    set_u(c, d, sum);
}

/* ---- inputs and outputs ------------------------------------------------------------------- */
static u32 builtin_in(Sx *c, u32 type, SpvBuiltIn which, int flat) {
    u32 v = spv_global(B, spv_type_pointer(B, SpvStorageClassInput, type), SpvStorageClassInput);
    decorate(c, v, SpvDecorationBuiltIn, which);
    if (flat) decorate0(c, v, SpvDecorationFlat);
    add_interface(c, v);
    return v;
}
static u32 load_in(Sx *c, u32 type, u32 var) { return SPV_VAL(B, SpvOpLoad, type, var); }

/* The vertex attribute n, decoded from its stream as DrawParams.attr[n] says (gpu_shader.c's
 * VERTEX_FETCH and fetchN, written here as selects: no branches, so that it can be in the prologue). */
static void fetch_attribute(Sx *c, u32 n) {
    spv_prologue_enter(B);
    if (c->vin) {   /* a vertex input the device fetches and converts (its class: float, signed or unsigned) */
        u32 cls = (u32)(c->vin >> (2 * n)) & 3;
        u32 t = cls == 1 ? spv_type_vector(B, c->t_i32, 4) : cls == 2 ? c->t_v4u : c->t_v4f;
        u32 var = spv_global(B, spv_type_pointer(B, SpvStorageClassInput, t), SpvStorageClassInput);
        decorate(c, var, SpvDecorationLocation, n);
        add_interface(c, var);
        u32 v = SPV_VAL(B, SpvOpLoad, t, var);
        if (cls != 2) v = SPV_VAL(B, SpvOpBitcast, c->t_v4u, v);
        for (u32 i = 0; i < 4; i++) c->attr_value[n][i] = SPV_VAL(B, SpvOpCompositeExtract, c->t_u32, v, i);
        spv_prologue_leave(B);
        return;
    }
    if (!c->vid) {
        c->vid = builtin_in(c, c->t_i32, SpvBuiltInVertexIndex, 0);
        c->iid = builtin_in(c, c->t_i32, SpvBuiltInInstanceIndex, 0);
    }
    if (!c->stream_var[n]) c->stream_var[n] = word_buffer(c, SPIRV_SET_BUFFERS, SPIRV_STREAM_BINDING + n);
    u32 ai = U(c, n), fmt = param(c, P_ATTR, ai, 0, 0), stride = param(c, P_ATTR, ai, 1, 0), divisor = param(c, P_ATTR, ai, 2, 0);
    u32 bound = param(c, P_ATTR, ai, 3, 0);
    u32 vid = asuint(c, load_in(c, c->t_i32, c->vid)), iid = asuint(c, load_in(c, c->t_i32, c->iid));
    /* (Vulkan's VertexIndex and InstanceIndex include the bases Direct3D's ids leave out: the
     * renderer passes 0 for them, and DrawParams.base as Direct3D's draw has it) */
    u32 per_instance = op2(c, SpvOpINotEqual, c->t_bool, divisor, c->c_u0);
    u32 safe_div = sel(c, c->t_u32, per_instance, divisor, U(c, 1));
    u32 index = sel(c, c->t_u32, per_instance, iadd(c, param(c, P_BASE, ~0u, 1, 0), op2(c, SpvOpUDiv, c->t_u32, iid, safe_div)), vid);
    /* (bound's bits 8 up: where the stream starts in its buffer, a descriptor's offset being aligned) */
    u32 addr = iadd(c, iadd(c, op2(c, SpvOpIMul, c->t_u32, index, stride), band(c, shr(c, fmt, U(c, 7)), U(c, 0x3FFF))), shr(c, bound, U(c, 8)));
    u32 base = band(c, addr, U(c, ~3u)), word_index = shr(c, base, U(c, 2));
    u32 args[7];
    for (u32 i = 0; i < 5; i++) args[i] = load_word(c, c->stream_var[n], iadd(c, word_index, U(c, i)));
    args[5] = addr;
    args[6] = fmt;
    if (!c->decode_fn) c->decode_fn = spv_id(B);
    u32 v = spv_val(B, SpvOpFunctionCall, c->t_v4u, 8, (u32[]){c->decode_fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6]});
    u32 usable = land(c, ieq(c, band(c, fmt, U(c, 0x40)), c->c_u0), op2(c, SpvOpINotEqual, c->t_bool, bound, c->c_u0));
    for (u32 i = 0; i < 4; i++)
        c->attr_value[n][i] = sel(c, c->t_u32, usable, SPV_VAL(B, SpvOpCompositeExtract, c->t_u32, v, i), i == 3 ? U(c, 0x3F800000u) : c->c_u0);
    spv_prologue_leave(B);
}

/* An attribute's components from the 5 words at its address (addr: its byte address, fmt: its
 * format word), as the HLSL translator's decode_attr: branches on the size and type, which are the
 * same for every vertex of a draw - only what the format asks for is done */
static void write_decode_function(Sx *c) {
    if (!c->decode_fn) return;
    u32 t4 = c->t_v4u;
    spv_function_begin_id(B, c->decode_fn, t4, spv_type_function(B, t4, 7, (u32[]){c->t_u32, c->t_u32, c->t_u32, c->t_u32, c->t_u32, c->t_u32, c->t_u32}));
    u32 words[5];
    for (u32 i = 0; i < 5; i++) words[i] = spv_function_param(B, c->t_u32);
    u32 addr = spv_function_param(B, c->t_u32), fmt = spv_function_param(B, c->t_u32);
    label(c, spv_id(B));
    u32 o[4], r_var = spv_local_init(B, c->p_func_u32, c->c_u0);
    for (u32 i = 0; i < 4; i++) o[i] = spv_local_init(B, c->p_func_u32, c->c_u0);
    u32 size = band(c, shr(c, fmt, U(c, 21)), U(c, 0x3F)), type = band(c, shr(c, fmt, U(c, 27)), U(c, 7));
    /* the size's components and their bits (0 components: an unknown size, read as (0, 0, 0, 1)) */
    static const struct { u8 size, n, bits; } sizes[] = {
        {0x01, 4, 32}, {0x02, 3, 32}, {0x03, 4, 16}, {0x04, 2, 32}, {0x05, 3, 16}, {0x0A, 4, 8}, {0x0F, 2, 16}, {0x12, 1, 32},
        {0x13, 3, 8},  {0x18, 2, 8},  {0x1B, 1, 16}, {0x1D, 1, 8}, {0x32, 2, 8},  {0x33, 4, 8}, {0x34, 1, 8},  {0x30, 4, 10},
    };
    u32 count = c->c_u0, bits = U(c, 32);
    for (u32 i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        u32 is = ieq(c, size, U(c, sizes[i].size));
        count = sel(c, c->t_u32, is, U(c, sizes[i].n), count);
        bits = sel(c, c->t_u32, is, U(c, sizes[i].bits), bits);
    }
    u32 unknown = ieq(c, count, c->c_u0);
    u32 m = if_begin(c, unknown);
    SPV_DO(B, SpvOpReturnValue, spv_const_composite(B, t4, 4, (u32[]){c->c_u0, c->c_u0, c->c_u0, U(c, 0x3F800000u)}));
    c->open = 0;
    label(c, m);
    u32 packed = ieq(c, size, U(c, 0x30)), sh = shl(c, band(c, addr, U(c, 3)), U(c, 3));
    u32 is_float = lor(c, lor(c, ieq(c, type, U(c, 7)), ieq(c, type, U(c, 1))), lor(c, lor(c, ieq(c, type, U(c, 2)), ieq(c, type, U(c, 5))),
                                                                                       ieq(c, type, U(c, 6))));
    SPV_DO(B, SpvOpStore, o[3], sel(c, c->t_u32, is_float, U(c, 0x3F800000u), U(c, 1)));
    for (u32 i = 0; i < 4; i++) {
        u32 has = if_begin(c, ult(c, U(c, i), count));
        u32 cb = i == 3 ? sel(c, c->t_u32, packed, U(c, 2), bits) : bits;
        u32 bo = iadd(c, sh, sel(c, c->t_u32, packed, U(c, i * 10), op2(c, SpvOpIMul, c->t_u32, U(c, i), bits)));
        u32 wi = shr(c, bo, U(c, 5)), s = band(c, bo, U(c, 31));
        /* the words at wi and wi + 1 (wi is at most 3: 4 components of 32 bits from a byte offset up to 3) */
        u32 lo = words[0], next = words[1];
        for (u32 k = 1; k < 4; k++) {
            u32 is = ieq(c, wi, U(c, k));
            lo = sel(c, c->t_u32, is, words[k], lo);
            next = sel(c, c->t_u32, is, words[k + 1], next);
        }
        u32 v = shr(c, lo, s);
        u32 spill = op2(c, SpvOpUGreaterThan, c->t_bool, iadd(c, s, cb), U(c, 32));
        /* (s is not 0 when it spills: the shift by 32 - s is in range) */
        v = sel(c, c->t_u32, spill, bor(c, v, shl(c, next, isub(c, U(c, 32), sel(c, c->t_u32, spill, s, U(c, 1))))), v);
        u32 narrow = ult(c, cb, U(c, 32)), safe_cb = sel(c, c->t_u32, narrow, cb, U(c, 31));
        v = sel(c, c->t_u32, narrow, band(c, v, isub(c, shl(c, U(c, 1), safe_cb), U(c, 1))), v);
        u32 sv = sel(c, c->t_u32, narrow, sar(c, shl(c, v, isub(c, U(c, 32), safe_cb)), isub(c, U(c, 32), safe_cb)), v);
        SPV_DO(B, SpvOpStore, r_var, v);
        /* switch (type): 1 snorm, 2 unorm, 3 sint, 5 uscaled, 6 sscaled, 7 float; the others as they are */
        u32 merge = spv_id(B), at[6];
        static const u32 types[6] = {1, 2, 3, 5, 6, 7};
        u32 ops[2 + 12];
        ops[0] = type;
        ops[1] = merge;
        for (u32 k = 0; k < 6; k++) { at[k] = spv_id(B); ops[2 + 2 * k] = types[k]; ops[3 + 2 * k] = at[k]; }
        spv_do(B, SpvOpSelectionMerge, 2, (u32[]){merge, SpvSelectionControlMaskNone});
        spv_do(B, SpvOpSwitch, 14, ops);
        c->open = 0;
        for (u32 k = 0; k < 6; k++) {
            label(c, at[k]);
            u32 r;
            if (types[k] == 1 || types[k] == 2) {
                u32 scale = sel(c, c->t_f32, narrow, SPV_VAL(B, SpvOpConvertUToF, c->t_f32, isub(c, shl(c, U(c, 1), safe_cb), U(c, 1))), Fc(c, 4294967295.0f));
                if (types[k] == 1) {
                    u32 fsv = SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, sv));
                    r = asuint(c, glsl2(c, c->t_f32, GLSLstd450NMax, op2(c, SpvOpFDiv, c->t_f32, fsv, SPV_VAL(B, SpvOpFSub, c->t_f32, f_mul(c, scale, Fc(c, 0.5f)), Fc(c, 0.5f))), Fc(c, -1.0f)));
                } else r = asuint(c, op2(c, SpvOpFDiv, c->t_f32, SPV_VAL(B, SpvOpConvertUToF, c->t_f32, v), scale));
            } else if (types[k] == 3) r = sv;
            else if (types[k] == 5) r = asuint(c, SPV_VAL(B, SpvOpConvertUToF, c->t_f32, v));
            else if (types[k] == 6) r = asuint(c, SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, sv)));
            else r = sel(c, c->t_u32, ieq(c, cb, U(c, 16)), asuint(c, f16tof32(c, v)), v);
            SPV_DO(B, SpvOpStore, r_var, r);
            branch(c, merge);
        }
        label(c, merge);
        SPV_DO(B, SpvOpStore, o[i], SPV_VAL(B, SpvOpLoad, c->t_u32, r_var));
        if_end(c, has);
    }
    u32 comp[4];
    for (u32 i = 0; i < 4; i++) comp[i] = SPV_VAL(B, SpvOpLoad, c->t_u32, o[i]);
    u32 reverse = op2(c, SpvOpINotEqual, c->t_bool, shr(c, fmt, U(c, 31)), c->c_u0);
    u32 x = sel(c, c->t_u32, reverse, comp[2], comp[0]), z = sel(c, c->t_u32, reverse, comp[0], comp[2]);
    SPV_DO(B, SpvOpReturnValue, SPV_VAL(B, SpvOpCompositeConstruct, t4, x, comp[1], z, comp[3]));
    spv_function_end(B);
}

/* A vertex shader input attribute (ALD), as uint. */
static u32 vs_input(Sx *c, u32 addr) {
    if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->info->inputs |= 1u << n;
        if (!c->attr_value[n][0]) fetch_attribute(c, n);
        return c->attr_value[n][(addr >> 2) & 3];
    }
    if (addr == 0x2F8 || addr == 0x2FC) {
        if (!c->vid) {
            spv_prologue_enter(B);
            c->vid = builtin_in(c, c->t_i32, SpvBuiltInVertexIndex, 0);
            c->iid = builtin_in(c, c->t_i32, SpvBuiltInInstanceIndex, 0);
            spv_prologue_leave(B);
        }
        if (addr == 0x2FC) return asuint(c, load_in(c, c->t_i32, c->vid));
        if (c->vin) return asuint(c, load_in(c, c->t_i32, c->iid));   /* (the base instance is the draw's firstInstance then: InstanceIndex has it) */
        return iadd(c, asuint(c, load_in(c, c->t_i32, c->iid)), param(c, P_BASE, ~0u, 1, 0));
    }
    return c->c_u0;
}

static u32 out_attr(Sx *c, u32 n) {
    if (!c->out_attr[n]) {
        c->out_attr[n] = spv_global_init(B, c->p_out_v4f, SpvStorageClassOutput, spv_const_null(B, c->t_v4f));
        decorate(c, c->out_attr[n], SpvDecorationLocation, n);
        add_interface(c, c->out_attr[n]);
    }
    return c->out_attr[n];
}

static void vs_output(Sx *c, u32 addr, u32 value) {
    if (addr >= 0x70 && addr < 0x80) {
        u32 p = SPV_VAL(B, SpvOpAccessChain, c->p_func_f32, c->opos, U(c, (addr >> 2) & 3));
        SPV_DO(B, SpvOpStore, p, asfloat(c, value));
    } else if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << n;
        u32 p = SPV_VAL(B, SpvOpAccessChain, c->p_out_f32, out_attr(c, n), U(c, (addr >> 2) & 3));
        SPV_DO(B, SpvOpStore, p, asfloat(c, value));
    }
    /* point size, layer, viewport index, clip distances: not forwarded */
}

/* A pixel shader input (IPA), as float, before the IPA modifiers. */
static u32 ps_input(Sx *c, u32 addr, int *perspective) {
    *perspective = 0;
    if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->info->inputs |= 1u << n;
        u32 comp = (addr >> 2) & 3, mode = (c->prog[0x18 + n] >> (comp * 2)) & 3;
        *perspective = mode == 2;
        if (!(c->vs_outputs >> n & 1) || !c->in_attr[n]) return c->c_f0;
        u32 p = SPV_VAL(B, SpvOpAccessChain, c->p_in_f32, c->in_attr[n], U(c, comp));
        return SPV_VAL(B, SpvOpLoad, c->t_f32, p);
    }
    u32 fc = load_in(c, c->t_v4f, c->frag_coord);
#define FC(i) SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, fc, i)
    switch (addr) {
    /* (the window position in the render target's logical pixels: its physical ones scaled back) */
    case 0x70: return f_mul(c, FC(0), f_add(c, c->c_f1, param(c, P_RTSCALE, ~0u, 2, 1)));
    case 0x74: {
        u32 y = f_mul(c, FC(1), f_add(c, c->c_f1, param(c, P_RTSCALE, ~0u, 3, 1)));
        u32 flip = op2(c, SpvOpFUnordNotEqual, c->t_bool, param(c, P_FRAG, ~0u, 0, 1), c->c_f0);
        return sel(c, c->t_f32, flip, SPV_VAL(B, SpvOpFSub, c->t_f32, param(c, P_FRAG, ~0u, 1, 1), y), y);
    }
    case 0x78: return FC(2);
    case 0x7C: return FC(3);   /* (1 / clip w: Vulkan's FragCoord.w) */
    case 0x3FC: return asfloat(c, sel(c, c->t_u32, load_in(c, c->t_bool, c->front), U(c, 0xFFFFFFFFu), c->c_u0));
    }
#undef FC
    return c->c_f0;
}

static void emit_exit(Sx *c) {
    resume(c);
    if (c->stage == SHADER_VERTEX) {   /* finish(o): the clip-space fix-ups */
        u32 pos = SPV_VAL(B, SpvOpLoad, c->t_v4f, c->opos);
        u32 x = SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, pos, 0), y = SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, pos, 1);
        u32 z = SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, pos, 2), w = SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, pos, 3);
        u32 nx = f_mul(c, x, param(c, P_FIX, ~0u, 0, 1)), ny = f_mul(c, y, param(c, P_FIX, ~0u, 1, 1));
        u32 nz = f_add(c, f_mul(c, z, param(c, P_FIX, ~0u, 2, 1)), f_mul(c, w, param(c, P_FIX, ~0u, 3, 1)));
        SPV_DO(B, SpvOpStore, c->out_pos, SPV_VAL(B, SpvOpCompositeConstruct, c->t_v4f, nx, ny, nz, w));
        SPV_DO(B, SpvOpStore, c->out_psize, c->c_f1);
    } else if (c->stage == SHADER_PIXEL) {
        const u8 *h = c->prog;
        u32 target = *(const u32 *)(h + 0x48), flags = *(const u32 *)(h + 0x4C), reg = 0;
        for (u32 rt = 0; rt < 8; rt++) {
            u32 m = (target >> (rt * 4)) & 15;
            if (!m) continue;
            c->info->color_mask |= 1u << rt;
            u32 v[4];
            for (u32 k = 0; k < 4; k++) v[k] = m >> k & 1 ? F(c, reg++) : c->c_f0;
            if (rt == 0) {   /* the fixed-function alpha test on target 0's alpha: DrawParams.base z its function (D3D's 1-8; 0 off), w its reference */
                u32 func = param(c, P_BASE, ~0u, 2, 0), ref = asfloat(c, param(c, P_BASE, ~0u, 3, 0)), a = v[3];   /* (base is uint4: the reference's bits) */
                static const SpvOp cmp[8] = {0, 0, SpvOpFOrdLessThan, SpvOpFOrdEqual, SpvOpFOrdLessThanEqual, SpvOpFOrdGreaterThan, SpvOpFOrdNotEqual,
                                             SpvOpFOrdGreaterThanEqual};
                u32 pass = lor(c, ieq(c, func, c->c_u0), ieq(c, func, U(c, 8)));
                for (u32 f = 2; f < 8; f++) pass = lor(c, pass, land(c, ieq(c, func, U(c, f)), op2(c, cmp[f], c->t_bool, a, ref)));
                spv_extension(B, "SPV_EXT_demote_to_helper_invocation");
                spv_capability(B, SpvCapabilityDemoteToHelperInvocationEXT);
                u32 merge = if_begin(c, SPV_VAL(B, SpvOpLogicalNot, c->t_bool, pass));
                spv_do(B, SpvOpDemoteToHelperInvocationEXT, 0, NULL);
                if_end(c, merge);
            }
            SPV_DO(B, SpvOpStore, c->out_color[rt], SPV_VAL(B, SpvOpCompositeConstruct, c->t_v4f, v[0], v[1], v[2], v[3]));
        }
        if (flags & 2) { c->info->writes_depth = 1; SPV_DO(B, SpvOpStore, c->out_depth, F(c, reg + 1)); }
    }
    spv_do(B, SpvOpReturn, 0, NULL);
    c->open = 0;
}

/* ---- textures ------------------------------------------------------------------------------ */
static u32 texture_slot(Sx *c, u32 handle_offset, u32 type, int shadow) {
    ShaderInfo *i = c->info;
    for (u32 k = 0; k < i->ntex; k++)
        if (i->tex[k].handle_offset == handle_offset && i->tex[k].type == type && i->tex[k].shadow == shadow) return k;
    if (i->ntex == 32) return 31;
    i->tex[i->ntex] = (ShaderTexture){handle_offset, (u8)type, (u8)shadow};
    return i->ntex++;
}

/* Can this stage compute gradients here? (Implicit-LOD sampling needs them.) */
static int gradients(Sx *c) { return c->stage == SHADER_PIXEL && !c->branchy; }

/* Texture k's image type (its ShaderTexture's type; a depth image when it is compared) */
static u32 image_type(Sx *c, u32 k) {
    const ShaderTexture *t = &c->info->tex[k];
    static const SpvDim dims[8] = {SpvDim1D, SpvDim1D, SpvDim2D, SpvDim2D, SpvDim3D, SpvDim3D, SpvDimCube, SpvDimCube};
    static const u8 arrayed[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    if (dims[t->type] == SpvDim1D) spv_capability(B, SpvCapabilitySampled1D);
    if (t->type == TEX_ARRAY_CUBE) spv_capability(B, SpvCapabilitySampledCubeArray);
    return spv_type_image(B, c->t_f32, dims[t->type], t->shadow ? 1 : 0, arrayed[t->type], 0, 1, SpvImageFormatUnknown);
}
/* Texture k with its sampler (set 1, the stage's binding k) */
static u32 sampled(Sx *c, u32 k) {
    if (!c->tex_var[k]) {
        u32 st = spv_type_sampled_image(B, image_type(c, k));
        c->tex_var[k] = spv_global(B, spv_type_pointer(B, SpvStorageClassUniformConstant, st), SpvStorageClassUniformConstant);
        decorate(c, c->tex_var[k], SpvDecorationDescriptorSet, SPIRV_SET_TEXTURES);
        decorate(c, c->tex_var[k], SpvDecorationBinding, (c->stage == SHADER_PIXEL ? SPIRV_TEXTURE_BINDING_PIXEL : 0) + k);
    }
    return SPV_VAL(B, SpvOpLoad, spv_type_sampled_image(B, image_type(c, k)), c->tex_var[k]);
}
static u32 image_of(Sx *c, u32 k) { return SPV_VAL(B, SpvOpImage, image_type(c, k), sampled(c, k)); }

static u32 vec(Sx *c, u32 type, u32 n, const u32 *v) { return n == 1 ? v[0] : spv_val(B, SpvOpCompositeConstruct, type, n, v); }
static u32 extract(Sx *c, u32 type, u32 v, u32 i) { return SPV_VAL(B, SpvOpCompositeExtract, type, v, i); }

/* A coordinate vector for the type (layer: a float, or 0) */
static u32 coord_of(Sx *c, u32 type, u32 x, u32 y, u32 z, u32 layer) {
    switch (type) {
    case TEX_1D: return x;
    case TEX_ARRAY_1D: return vec(c, c->t_v2f, 2, (u32[]){x, layer});
    case TEX_2D: return vec(c, c->t_v2f, 2, (u32[]){x, y});
    case TEX_ARRAY_2D: return vec(c, c->t_v3f, 3, (u32[]){x, y, layer});
    case TEX_3D: case TEX_CUBE: return vec(c, c->t_v3f, 3, (u32[]){x, y, z});
    case TEX_ARRAY_CUBE: return vec(c, c->t_v4f, 4, (u32[]){x, y, z, layer});
    }
    return x;
}
static u32 layer_of(Sx *c, u32 r) { return SPV_VAL(B, SpvOpConvertUToF, c->t_f32, band(c, R(c, r), U(c, 0xFFFF))); }

/* A sample (a float4). lod_mode: 0 implicit, 1 zero, 2 bias, 3 explicit. A compared one is (r, r, r, 1). */
static u32 sample(Sx *c, u32 k, u32 coord, int lod_mode, u32 lod, u32 dref) {
    if ((lod_mode == 0 || lod_mode == 2) && !gradients(c)) lod_mode = 1;
    u32 si = sampled(c, k);
    if (dref) {
        u32 r = lod_mode == 0 || lod_mode == 2 ? SPV_VAL(B, SpvOpImageSampleDrefImplicitLod, c->t_f32, si, coord, dref)
                                               : SPV_VAL(B, SpvOpImageSampleDrefExplicitLod, c->t_f32, si, coord, dref, SpvImageOperandsLodMask, c->c_f0);
        return vec(c, c->t_v4f, 4, (u32[]){r, r, r, c->c_f1});
    }
    switch (lod_mode) {
    case 0: return SPV_VAL(B, SpvOpImageSampleImplicitLod, c->t_v4f, si, coord);
    case 2: return SPV_VAL(B, SpvOpImageSampleImplicitLod, c->t_v4f, si, coord, SpvImageOperandsBiasMask, lod);
    case 3: return SPV_VAL(B, SpvOpImageSampleExplicitLod, c->t_v4f, si, coord, SpvImageOperandsLodMask, lod);
    }
    return SPV_VAL(B, SpvOpImageSampleExplicitLod, c->t_v4f, si, coord, SpvImageOperandsLodMask, c->c_f0);
}

/* The texture's component swizzle (DrawParams.swz: 3 bits a component; 2-5 x-w, 6 and 7 one, else zero) */
static u32 swz(Sx *c, u32 v, u32 k) {
    u32 s = param(c, P_SWZ, U(c, (c->stage == SHADER_VERTEX ? 0 : 8) + (k >> 2)), k & 3, 0);
    u32 comp[4] = {extract(c, c->t_f32, v, 0), extract(c, c->t_f32, v, 1), extract(c, c->t_f32, v, 2), extract(c, c->t_f32, v, 3)}, out[4];
    for (u32 i = 0; i < 4; i++) {
        u32 si = band(c, shr(c, s, U(c, 3 * i)), U(c, 7)), r = sel(c, c->t_f32, uge(c, si, U(c, 6)), c->c_f1, c->c_f0);
        for (u32 j = 0; j < 4; j++) r = sel(c, c->t_f32, ieq(c, si, U(c, 2 + j)), comp[j], r);
        out[i] = r;
    }
    return vec(c, c->t_v4f, 4, out);
}

/* The components selected by mask of float4 v to the TEXS-style destinations: slots 0, 1 to a,
 * a+1; slots 2, 3 to b, b+1 (or packed halves for F16). */
static void store_split(Sx *c, u32 v, u32 mask, u32 da, u32 db, int f16) {
    u32 sel_[4], n = 0;
    for (u32 i = 0; i < 4; i++) if (mask >> i & 1) sel_[n++] = extract(c, c->t_f32, v, i);
    if (f16) {
        if (n >= 1) set_u(c, da, bor(c, f32tof16(c, sel_[0]), n >= 2 ? shl(c, f32tof16(c, sel_[1]), U(c, 16)) : c->c_u0));
        if (n >= 3) set_u(c, db, bor(c, f32tof16(c, sel_[2]), n >= 4 ? shl(c, f32tof16(c, sel_[3]), U(c, 16)) : c->c_u0));
        return;
    }
    u32 dst[4] = {da, da == RZ ? RZ : da + 1, db, db == RZ ? RZ : db + 1};
    for (u32 i = 0; i < n; i++) set_f(c, dst[i], sel_[i]);
}
/* Consecutive destinations for the components selected by mask (TEX, TLD4). */
static void store_seq(Sx *c, u32 v, u32 mask, u32 d) {
    for (u32 i = 0; i < 4; i++)
        if (mask >> i & 1) { set_f(c, d, extract(c, c->t_f32, v, i)); if (d != RZ) d++; }
}

static void emit_texs(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8);
    u32 handle = BITS(w, 36, 13) * 4, swz_ = BITS(w, 50, 3), enc = BITS(w, 53, 4);
    int f16 = !BITS(w, 59, 1);
    static const u8 RG[8] = {1, 2, 4, 8, 3, 9, 10, 12}, RGBA[8] = {7, 11, 13, 14, 15, 15, 15, 15};
    u32 mask = db == RZ ? RG[swz_] : RGBA[swz_];
    u32 type = TEX_2D, coord, lodx = 0, dref = 0;
    int lod = 0;
    switch (enc) {
    case 0: type = TEX_1D; lod = 1; coord = F(c, a); break;
    case 1: coord = coord_of(c, TEX_2D, F(c, a), F(c, b), 0, 0); break;
    case 2: lod = 1; coord = coord_of(c, TEX_2D, F(c, a), F(c, b), 0, 0); break;
    case 3: lod = 3; lodx = F(c, b); coord = coord_of(c, TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 4: dref = F(c, b); coord = coord_of(c, TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 5: lod = 3; lodx = F(c, b); dref = F(c, b + 1); coord = coord_of(c, TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 6: lod = 1; dref = F(c, b); coord = coord_of(c, TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 7: type = TEX_ARRAY_2D; coord = coord_of(c, type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 8: type = TEX_ARRAY_2D; lod = 1; coord = coord_of(c, type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 9: type = TEX_ARRAY_2D; lod = 1; dref = F(c, b + 1); coord = coord_of(c, type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 10: type = TEX_3D; coord = coord_of(c, type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 11: type = TEX_3D; lod = 1; coord = coord_of(c, type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 12: type = TEX_CUBE; coord = coord_of(c, type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 13: type = TEX_CUBE; lod = 3; lodx = F(c, b + 1); coord = coord_of(c, type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    default: unsupported(c, in, "TEXS encoding"); return;
    }
    u32 k = texture_slot(c, handle, type, dref != 0);
    u32 v = sample(c, k, coord, lod, lodx, dref);
    if (!dref) v = swz(c, v, k);
    store_split(c, v, mask, da, db, f16);
}

/* The size of texture k's level 0, as floats (width, height) */
static void texture_size(Sx *c, u32 k, u32 type, u32 *wf, u32 *hf) {
    spv_capability(B, SpvCapabilityImageQuery);
    static const u8 dims[8] = {1, 2, 2, 3, 3, 3, 2, 3};
    u32 n = dims[type], t = n == 1 ? c->t_i32 : spv_type_vector(B, c->t_i32, n);
    u32 size = SPV_VAL(B, SpvOpImageQuerySizeLod, t, image_of(c, k), I(c, 0));
    *wf = SPV_VAL(B, SpvOpConvertSToF, c->t_f32, n == 1 ? size : extract(c, c->t_i32, size, 0));
    *hf = n == 1 ? c->c_f1 : SPV_VAL(B, SpvOpConvertSToF, c->t_f32, extract(c, c->t_i32, size, 1));
}

static void emit_tex(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), m = BITS(w, 20, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4);
    u32 handle = BITS(w, 36, 13) * 4, dc = BITS(w, 50, 1), aoffi = BITS(w, 54, 1), blod = BITS(w, 55, 3);
    u32 x, y = 0, z = 0, layer = 0;
    switch (type) {
    case TEX_1D: x = F(c, a); break;
    case TEX_ARRAY_1D: x = F(c, a + 1); layer = layer_of(c, a); break;
    case TEX_2D: x = F(c, a); y = F(c, a + 1); break;
    case TEX_ARRAY_2D: x = F(c, a + 1); y = F(c, a + 2); layer = layer_of(c, a); break;
    case TEX_3D: case TEX_CUBE: x = F(c, a); y = F(c, a + 1); z = F(c, a + 2); break;
    case TEX_ARRAY_CUBE: x = F(c, a + 1); y = F(c, a + 2); z = F(c, a + 3); layer = layer_of(c, a); break;
    default: unsupported(c, in, "TEX type"); return;
    }
    int lod = blod == 1 ? 1 : blod == 2 || blod == 6 ? 2 : blod == 3 || blod == 7 ? 3 : 0;
    u32 lodx = 0, dref = 0, off = 0;
    if (lod >= 2) lodx = F(c, m++);
    if (aoffi) off = R(c, m++);
    if (dc) dref = F(c, m++);
    u32 k = texture_slot(c, handle, type, dc);
    if (off) {   /* (sampling takes constant offsets only: the coordinates shifted instead, 4-bit signed texels) */
        u32 tw, th;
        texture_size(c, k, type, &tw, &th);
        u32 ox = SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, sext(c, off, 28, 28)));
        x = f_add(c, x, op2(c, SpvOpFDiv, c->t_f32, ox, tw));
        if (y) {
            u32 oy = SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, sext(c, off, 24, 28)));
            y = f_add(c, y, op2(c, SpvOpFDiv, c->t_f32, oy, th));
        }
    }
    u32 v = sample(c, k, coord_of(c, type, x, y, z, layer), lod, lodx, dref);
    if (!dref) v = swz(c, v, k);
    store_seq(c, v, mask, d);
}

/* A gather of component comp (or compared), with an offset (an int2, or 0) */
static u32 gather(Sx *c, u32 k, u32 comp, u32 coord, u32 dref, u32 offset) {
    u32 si = sampled(c, k);
    if (offset) spv_capability(B, SpvCapabilityImageGatherExtended);
    if (dref) return offset ? SPV_VAL(B, SpvOpImageDrefGather, c->t_v4f, si, coord, dref, SpvImageOperandsOffsetMask, offset)
                            : SPV_VAL(B, SpvOpImageDrefGather, c->t_v4f, si, coord, dref);
    return offset ? SPV_VAL(B, SpvOpImageGather, c->t_v4f, si, coord, U(c, comp & 3), SpvImageOperandsOffsetMask, offset)
                  : SPV_VAL(B, SpvOpImageGather, c->t_v4f, si, coord, U(c, comp & 3));
}
/* a 6-bit signed texel offset pair from a register: x from bits [lx, lx + 6), y from [ly, ly + 6) */
static u32 offset6(Sx *c, u32 r, u32 x_left, u32 y_left) {
    u32 v2i = spv_type_vector(B, c->t_i32, 2);
    return vec(c, v2i, 2, (u32[]){asint(c, sext(c, r, x_left, 26)), asint(c, sext(c, r, y_left, 26))});
}

static void emit_tld4(Sx *c, const ShaderInsn *in, int scalar) {
    u64 w = in->w;
    if (scalar) {   /* TLD4S: 2D only */
        u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8), handle = BITS(w, 36, 13) * 4;
        u32 dc = BITS(w, 50, 1), aoffi = BITS(w, 51, 1), comp = BITS(w, 52, 2), f16 = BITS(w, 55, 1);
        u32 coord, dref = 0, offset = 0;
        if (!aoffi && !dc) coord = coord_of(c, TEX_2D, F(c, a), F(c, b), 0, 0);
        else {
            coord = coord_of(c, TEX_2D, F(c, a), F(c, a + 1), 0, 0);
            if (aoffi) {
                offset = offset6(c, R(c, b), 26, 18);
                if (dc) dref = F(c, b + 1);
            } else dref = F(c, b);
        }
        u32 k = texture_slot(c, handle, TEX_2D, dc);
        store_split(c, gather(c, k, comp, coord, dref, offset), 15, da, db, f16);
        return;
    }
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), m = BITS(w, 20, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4);
    u32 handle = BITS(w, 36, 13) * 4, dc = BITS(w, 50, 1), offmode = BITS(w, 54, 2), comp = BITS(w, 56, 2);
    u32 x, y = 0, z = 0, layer = 0, offset = 0, dref = 0, o0 = 0, o1 = 0;
    switch (type) {
    case TEX_2D: x = F(c, a); y = F(c, a + 1); break;
    case TEX_ARRAY_2D: x = F(c, a + 1); y = F(c, a + 2); layer = layer_of(c, a); break;
    case TEX_CUBE: x = F(c, a); y = F(c, a + 1); z = F(c, a + 2); break;
    case TEX_ARRAY_CUBE: x = F(c, a + 1); y = F(c, a + 2); z = F(c, a + 3); layer = layer_of(c, a); break;
    default: unsupported(c, in, "TLD4 type"); return;
    }
    if (offmode == 1) offset = offset6(c, R(c, m++), 26, 18);
    else if (offmode == 2) { o0 = R(c, m); o1 = R(c, m + 1); m += 2; }   /* a separate offset per texel: two pairs in each register */
    if (dc) dref = F(c, m);
    u32 k = texture_slot(c, handle, type, dc), coord = coord_of(c, type, x, y, z, layer), v;
    if (offmode == 2) {   /* four gathers, each with its own offset, one component of each */
        static const u8 shifts[4][2] = {{26, 18}, {10, 2}, {26, 18}, {10, 2}};
        u32 part[4];
        for (u32 i = 0; i < 4; i++) part[i] = extract(c, c->t_f32, gather(c, k, comp, coord, dref, offset6(c, i < 2 ? o0 : o1, shifts[i][0], shifts[i][1])), i);
        v = vec(c, c->t_v4f, 4, part);
    } else v = gather(c, k, comp, coord, dref, offset);
    store_seq(c, v, mask, d);
}

/* A texel of texture k at the logical (x, y) the program asks for: the physical one of a scaled
 * render target read as a texture (render-resolution-scale); exact when unscaled. */
static void tsxy(Sx *c, u32 k, u32 x, u32 y, u32 *ox, u32 *oy) {
    u32 dscale = param(c, P_TEXSCALE, U(c, (c->stage == SHADER_VERTEX ? 0 : 8) + (k >> 2)), k & 3, 1);
    u32 f = f_add(c, c->c_f1, dscale), half = Fc(c, 0.5f);
    u32 sx = asuint(c, SPV_VAL(B, SpvOpConvertFToS, c->t_i32, glsl1(c, c->t_f32, GLSLstd450Floor, f_mul(c, f_add(c, SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, x)), half), f))));
    u32 sy = asuint(c, SPV_VAL(B, SpvOpConvertFToS, c->t_i32, glsl1(c, c->t_f32, GLSLstd450Floor, f_mul(c, f_add(c, SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, y)), half), f))));
    u32 exact = op2(c, SpvOpFOrdEqual, c->t_bool, dscale, c->c_f0);
    *ox = sel(c, c->t_u32, exact, x, sx);
    *oy = sel(c, c->t_u32, exact, y, sy);
}

static void emit_tlds(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8);
    u32 handle = BITS(w, 36, 13) * 4, swz_ = BITS(w, 50, 3), enc = BITS(w, 53, 4);
    int f16 = !BITS(w, 59, 1);
    static const u8 RG[8] = {1, 2, 4, 8, 3, 9, 10, 12}, RGBA[8] = {7, 11, 13, 14, 15, 15, 15, 15};
    u32 mask = db == RZ ? RG[swz_] : RGBA[swz_];
    u32 type = enc <= 1 ? TEX_1D : enc == 7 ? TEX_3D : enc == 8 ? TEX_ARRAY_2D : TEX_2D;
    if (enc == 3 || enc == 6 || (enc > 8 && enc != 12)) { unsupported(c, in, "TLDS encoding"); return; }
    u32 k = texture_slot(c, handle, type, 0);
    u32 v2i = spv_type_vector(B, c->t_i32, 2), v3i = spv_type_vector(B, c->t_i32, 3);
    u32 coord, lod = c->c_u0, ia = R(c, a), tx, ty;
#define XY(x, y) (tsxy(c, k, x, y, &tx, &ty), vec(c, v2i, 2, (u32[]){asint(c, tx), asint(c, ty)}))
    switch (enc) {   /* (the coordinates and the level; 2D texel coordinates go through tsxy) */
    case 0: coord = asint(c, ia); break;
    case 1: coord = asint(c, ia); lod = R(c, b); break;
    case 2: coord = XY(ia, R(c, b)); break;
    case 4: coord = XY(ia, R(c, a + 1)); break;   /* offset ignored */
    case 5: coord = XY(ia, R(c, a + 1)); lod = R(c, b); break;
    case 7: coord = vec(c, v3i, 3, (u32[]){asint(c, ia), asint(c, R(c, a + 1)), asint(c, R(c, b))}); break;
    case 8:
        tsxy(c, k, R(c, b), R(c, b + 1), &tx, &ty);
        coord = vec(c, v3i, 3, (u32[]){asint(c, tx), asint(c, ty), asint(c, band(c, R(c, a), U(c, 0xFFFF)))});
        break;
    default: coord = XY(ia, R(c, a + 1)); lod = R(c, b); break;   /* 12 */
    }
#undef XY
    u32 v = SPV_VAL(B, SpvOpImageFetch, c->t_v4f, image_of(c, k), coord, SpvImageOperandsLodMask, asint(c, lod));
    store_split(c, swz(c, v, k), mask, da, db, f16);
}

/* TMML: the level of detail, computed and unclamped, as 8.8 fixed point */
static void emit_tmml(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4), handle = BITS(w, 36, 13) * 4, coord;
    switch (type) {
    case TEX_1D: coord = F(c, a); break;
    case TEX_2D: coord = coord_of(c, TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case TEX_ARRAY_2D: coord = vec(c, c->t_v2f, 2, (u32[]){F(c, a + 1), F(c, a + 2)}); break;
    case TEX_3D: case TEX_CUBE: coord = vec(c, c->t_v3f, 3, (u32[]){F(c, a), F(c, a + 1), F(c, a + 2)}); break;
    default: unsupported(c, in, "TMML type"); return;
    }
    u32 k = texture_slot(c, handle, type, 0), v[4] = {c->c_f0, c->c_f0, c->c_f0, c->c_f0};
    if (gradients(c)) {
        spv_capability(B, SpvCapabilityImageQuery);
        u32 lod = SPV_VAL(B, SpvOpImageQueryLod, c->t_v2f, sampled(c, k), coord);
        v[0] = extract(c, c->t_f32, lod, 0);
        v[1] = extract(c, c->t_f32, lod, 1);
    }
    for (u32 i = 0; i < 4; i++)
        if (mask >> i & 1) { set_u(c, d, f2i(c, f_mul(c, v[i], Fc(c, 256.0f)), 0)); if (d != RZ) d++; }
}

/* ---- memory ---------------------------------------------------------------------------------- */
/* Global memory is in the storage buffers ShaderInfo notes (their descriptors' words read from
 * constant buffer 0): gload/gstore look an address up in each in turn (DrawParams.sb: its GPU
 * address and size); outside them a load is 0 and a store is dropped. They are functions written
 * after the program, when every storage buffer is known. */
static u32 gload(Sx *c, u32 lo, u32 hi) {
    if (!c->gload_fn) c->gload_fn = spv_id(B);
    return SPV_VAL(B, SpvOpFunctionCall, c->t_u32, c->gload_fn, lo, hi);
}
static void gstore(Sx *c, u32 lo, u32 hi, u32 v) {
    if (!c->gstore_fn) c->gstore_fn = spv_id(B);
    SPV_VAL(B, SpvOpFunctionCall, c->t_void, c->gstore_fn, lo, hi, v);
}
static u32 hi_carry(Sx *c, u32 lo, u32 hi, u32 add) {   /* hi of (hi:lo) + add, add a small constant */
    return iadd(c, hi, sel(c, c->t_u32, ult(c, iadd(c, lo, U(c, add)), lo), U(c, 1), c->c_u0));
}

static void write_memory_functions(Sx *c) {
    if (!c->gload_fn && !c->gstore_fn) return;
    u32 first = c->stage == SHADER_PIXEL ? 16 : 0, n = c->info->sb_count;
    u32 buf[16];
    for (u32 k = 0; k < n; k++) {
        buf[k] = spv_global(B, spv_type_pointer(B, SpvStorageClassStorageBuffer, word_struct(c)), SpvStorageClassStorageBuffer);
        decorate(c, buf[k], SpvDecorationDescriptorSet, SPIRV_SET_STORAGE);
        decorate(c, buf[k], SpvDecorationBinding, (c->stage == SHADER_PIXEL ? SPIRV_STORAGE_BINDING_PIXEL : 0) + k);
        if (c->stage != SHADER_COMPUTE) decorate0(c, buf[k], SpvDecorationNonWritable);
    }
    /* (hit k: the address is in buffer k; d its byte offset there - from the descriptor's start, which
 * is sb[k][3] bytes before the buffer's, a descriptor's offset being aligned) */
#define HIT(k, lo, hi, d_out) do { \
        u32 s_x = param(c, P_SB, U(c, first + (k)), 0, 0), s_y = param(c, P_SB, U(c, first + (k)), 1, 0), s_z = param(c, P_SB, U(c, first + (k)), 2, 0); \
        u32 s_w = param(c, P_SB, U(c, first + (k)), 3, 0); \
        u32 d_in = isub(c, lo, s_x); \
        u32 hdiff = isub(c, isub(c, hi, s_y), sel(c, c->t_u32, ult(c, lo, s_x), U(c, 1), c->c_u0)); \
        hit = land(c, ieq(c, hdiff, c->c_u0), ult(c, d_in, s_z)); \
        d_out = iadd(c, d_in, s_w); \
    } while (0)
    if (c->gload_fn) {
        spv_function_begin_id(B, c->gload_fn, c->t_u32, spv_type_function(B, c->t_u32, 2, (u32[]){c->t_u32, c->t_u32}));
        u32 lo = spv_function_param(B, c->t_u32), hi = spv_function_param(B, c->t_u32);
        label(c, spv_id(B));
        u32 hit, d;
        for (u32 k = 0; k < n; k++) {   /* (the first that holds it wins, as in HLSL; only that one is read) */
            HIT(k, lo, hi, d);
            u32 merge = if_begin(c, hit);
            SPV_DO(B, SpvOpReturnValue, load_word(c, buf[k], shr(c, d, U(c, 2))));
            c->open = 0;
            label(c, merge);
        }
        SPV_DO(B, SpvOpReturnValue, c->c_u0);
        spv_function_end(B);
    }
    if (c->gstore_fn) {
        spv_function_begin_id(B, c->gstore_fn, c->t_void, spv_type_function(B, c->t_void, 3, (u32[]){c->t_u32, c->t_u32, c->t_u32}));
        u32 lo = spv_function_param(B, c->t_u32), hi = spv_function_param(B, c->t_u32), v = spv_function_param(B, c->t_u32);
        label(c, spv_id(B));
        u32 hit, d;
        for (u32 k = 0; k < n; k++) {
            HIT(k, lo, hi, d);
            u32 merge = if_begin(c, hit);
            SPV_DO(B, SpvOpStore, SPV_VAL(B, SpvOpAccessChain, c->p_sb_u32, buf[k], c->c_u0, shr(c, d, U(c, 2))), v);
            spv_do(B, SpvOpReturn, 0, NULL);
            c->open = 0;
            label(c, merge);
        }
        spv_do(B, SpvOpReturn, 0, NULL);
        spv_function_end(B);
    }
#undef HIT
}

/* shared (compute) and local memory, as words: index modulo their size */
static u32 smem(Sx *c, int local) {
    u32 *var = local ? &c->lmem : &c->smem;
    if (!*var) {
        u32 words = local ? 256 : (c->shared / 4 ? c->shared / 4 : 1);
        u32 t = spv_type_array(B, c->t_u32, words);
        if (local) *var = spv_local(B, spv_type_pointer(B, SpvStorageClassFunction, t));
        else *var = spv_global(B, spv_type_pointer(B, SpvStorageClassWorkgroup, t), SpvStorageClassWorkgroup);
    }
    return *var;
}
static u32 mem_ptr(Sx *c, int local, u32 word) {
    u32 words = local ? 256 : (c->shared / 4 ? c->shared / 4 : 1);
    u32 i = op2(c, SpvOpUMod, c->t_u32, word, U(c, words));
    return SPV_VAL(B, SpvOpAccessChain, spv_type_pointer(B, local ? SpvStorageClassFunction : SpvStorageClassWorkgroup, c->t_u32), smem(c, local), i);
}

static void emit_memory(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    u32 d = BITS(w, 0, 8), ra = BITS(w, 8, 8);
    switch (in->op) {
    case OP_STG: case OP_LDG: {   /* global memory: found in one of the storage buffers */
        int store = in->op == OP_STG;
        if (store && c->stage != SHADER_COMPUTE) { unsupported(c, in, "STG outside a compute shader"); break; }
        u32 e = BITS(w, 45, 1), size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        c->uses_gmem = 1;
        if (store) c->info->stores = 1;
        u32 base = R(c, ra), lo = iadd(c, base, U(c, (u32)off)), hi = e && ra != RZ ? R(c, ra + 1) : c->c_u0;
        hi = off >= 0 ? iadd(c, hi, sel(c, c->t_u32, ult(c, lo, base), U(c, 1), c->c_u0))
                      : isub(c, hi, sel(c, c->t_u32, op2(c, SpvOpUGreaterThan, c->t_bool, lo, base), U(c, 1), c->c_u0));
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16, sh = shl(c, band(c, lo, U(c, 3)), U(c, 3)), lo4 = band(c, lo, U(c, ~3u));
            if (store) {   /* the word, with these bits replaced */
                u32 m = U(c, (1u << bits) - 1), word = gload(c, lo4, hi);
                gstore(c, lo4, hi, bor(c, band(c, word, bnot(c, shl(c, m, sh))), shl(c, band(c, R(c, d), m), sh)));
            } else {
                u32 word = shr(c, gload(c, lo4, hi), sh), sg = size & 1;
                set_u(c, d, sg ? sext(c, word, 32 - bits, 32 - bits) : band(c, word, U(c, (1u << bits) - 1)));
            }
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words; i++) {
                u32 li = i ? iadd(c, lo, U(c, 4 * i)) : lo, hii = i ? hi_carry(c, lo, hi, 4 * i) : hi;
                if (store) gstore(c, li, hii, d == RZ ? c->c_u0 : R(c, d + i));
                else if (d != RZ && d + i < RZ) set_u(c, d + i, gload(c, li, hii));
            }
        }
        break;
    }
    default: {   /* LDS, STS, LDL, STL */
        int local = in->op == OP_LDL || in->op == OP_STL, load = in->op == OP_LDS || in->op == OP_LDL;
        if (!local && c->stage != SHADER_COMPUTE) { unsupported(c, in, "shared memory outside a compute shader"); break; }
        u32 size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        u32 a = iadd(c, R(c, ra), U(c, (u32)off)), word = shr(c, a, U(c, 2));
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1, m = U(c, (1u << bits) - 1), sh = shl(c, band(c, a, U(c, 3)), U(c, 3));
            u32 p = mem_ptr(c, local, word), cur = SPV_VAL(B, SpvOpLoad, c->t_u32, p);
            if (load) {
                u32 v = shr(c, cur, sh);
                set_u(c, d, sg ? sext(c, v, 32 - bits, 32 - bits) : band(c, v, m));
            } else SPV_DO(B, SpvOpStore, p, bor(c, band(c, cur, bnot(c, shl(c, m, sh))), shl(c, band(c, R(c, d), m), sh)));
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words && d + i < RZ; i++) {
                u32 p = mem_ptr(c, local, iadd(c, word, U(c, i)));
                if (load) set_u(c, d + i, SPV_VAL(B, SpvOpLoad, c->t_u32, p));
                else SPV_DO(B, SpvOpStore, p, d == RZ ? c->c_u0 : R(c, d + i));
            }
        }
        break;
    }
    }
}

/* ---- instructions ------------------------------------------------------------------------- */
static void emit_insn(Sx *c, const ShaderInsn *in) {
    u64 w = in->w;
    int form = shader_form_of(in->op);
    u32 d = BITS(w, 0, 8), ra = BITS(w, 8, 8);
    switch (in->op) {
    /* -- floating point -- */
    case OP_FADD_reg: case OP_FADD_cbuf: case OP_FADD_imm: {
        u32 a = absneg(c, F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), b = absneg(c, b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1));
        u32 r = f_add(c, a, b);
        set_f(c, d, BITS(w, 50, 1) ? saturate(c, r) : r);
        break;
    }
    case OP_FADD32I: {
        u32 a = absneg(c, F(c, ra), BITS(w, 54, 1), BITS(w, 56, 1)), b = absneg(c, b_f(c, w, FORM_32I), BITS(w, 57, 1), BITS(w, 53, 1));
        set_f(c, d, f_add(c, a, b));
        break;
    }
    case OP_FMUL_reg: case OP_FMUL_cbuf: case OP_FMUL_imm: case OP_FMUL32I: {
        int is32 = in->op == OP_FMUL32I;
        u32 a = F(c, ra), b = absneg(c, b_f(c, w, form), 0, is32 ? 0 : BITS(w, 48, 1));
        u32 fmz = is32 ? BITS(w, 53, 2) : BITS(w, 44, 2), sat = is32 ? BITS(w, 55, 1) : BITS(w, 50, 1);
        static const float scale[8] = {1, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f, 1};
        u32 sa = is32 ? 0 : BITS(w, 41, 3);
        u32 r = f_mul(c, sa == 0 || sa == 7 ? a : f_mul(c, a, Fc(c, scale[sa])), b);
        if (fmz == 2 && !sat) r = sel(c, c->t_f32, lor(c, op2(c, SpvOpFOrdEqual, c->t_bool, a, c->c_f0), op2(c, SpvOpFOrdEqual, c->t_bool, b, c->c_f0)), c->c_f0, r);
        set_f(c, d, sat ? saturate(c, r) : r);
        break;
    }
    case OP_FFMA_reg: case OP_FFMA_rc: case OP_FFMA_cr: case OP_FFMA_imm: case OP_FFMA32I: {
        int is32 = in->op == OP_FFMA32I;
        u32 a = absneg(c, F(c, ra), 0, is32 ? BITS(w, 56, 1) : 0);
        u32 b = is32 ? b_f(c, w, FORM_32I) : absneg(c, b_f(c, w, form), 0, BITS(w, 48, 1));
        u32 cc = absneg(c, is32 ? F(c, d) : asfloat(c, c_u(c, w, form)), 0, is32 ? BITS(w, 57, 1) : BITS(w, 49, 1));
        u32 fmz = BITS(w, 53, 2), sat = is32 ? BITS(w, 55, 1) : BITS(w, 50, 1);
        u32 r = glsl3(c, c->t_f32, GLSLstd450Fma, a, b, cc);
        if (fmz == 2 && !sat) r = sel(c, c->t_f32, lor(c, op2(c, SpvOpFOrdEqual, c->t_bool, a, c->c_f0), op2(c, SpvOpFOrdEqual, c->t_bool, b, c->c_f0)), cc, r);
        set_f(c, d, sat ? saturate(c, r) : r);
        break;
    }
    case OP_MUFU: {
        u32 x = absneg(c, F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), r;
        switch (BITS(w, 20, 4)) {
        case 0: r = glsl1(c, c->t_f32, GLSLstd450Cos, x); break;
        case 1: r = glsl1(c, c->t_f32, GLSLstd450Sin, x); break;
        case 2: r = glsl1(c, c->t_f32, GLSLstd450Exp2, x); break;
        case 3: r = glsl1(c, c->t_f32, GLSLstd450Log2, x); break;
        case 4: r = op2(c, SpvOpFDiv, c->t_f32, c->c_f1, x); break;
        case 5: r = glsl1(c, c->t_f32, GLSLstd450InverseSqrt, x); break;
        case 8: r = glsl1(c, c->t_f32, GLSLstd450Sqrt, x); break;
        default: unsupported(c, in, "MUFU operation"); return;
        }
        set_f(c, d, BITS(w, 50, 1) ? saturate(c, r) : r);
        break;
    }
    case OP_RRO_reg: case OP_RRO_cbuf: case OP_RRO_imm:
        set_f(c, d, absneg(c, b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1)));
        break;
    case OP_FMNMX_reg: case OP_FMNMX_cbuf: case OP_FMNMX_imm: {
        u32 a = absneg(c, F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), b = absneg(c, b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1));
        u32 p = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        set_f(c, d, sel(c, c->t_f32, p, glsl2(c, c->t_f32, GLSLstd450NMin, a, b), glsl2(c, c->t_f32, GLSLstd450NMax, a, b)));
        break;
    }
    case OP_FSETP_reg: case OP_FSETP_cbuf: case OP_FSETP_imm: {
        u32 a = absneg(c, F(c, ra), BITS(w, 7, 1), BITS(w, 43, 1)), b = absneg(c, b_f(c, w, form), BITS(w, 44, 1), BITS(w, 6, 1));
        u32 cmp = fcompare(c, BITS(w, 48, 4), a, b), bp = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)), bop = BITS(w, 45, 2);
        set_p(c, BITS(w, 3, 3), combine(c, bop, cmp, bp));
        set_p(c, BITS(w, 0, 3), combine(c, bop, lnot(c, cmp), bp));
        break;
    }
    case OP_FSET_reg: case OP_FSET_cbuf: case OP_FSET_imm: {
        u32 a = absneg(c, F(c, ra), BITS(w, 54, 1), BITS(w, 43, 1)), b = absneg(c, b_f(c, w, form), BITS(w, 44, 1), BITS(w, 53, 1));
        u32 r = combine(c, BITS(w, 45, 2), fcompare(c, BITS(w, 48, 4), a, b), PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)));
        set_u(c, d, bool_word(c, r, BITS(w, 52, 1)));
        break;
    }
    case OP_CSET: case OP_CSETP: {   /* a condition code test, combined with a predicate */
        u32 cond = flow_cond(c, BITS(w, 8, 5));
        if (!cond) cond = c->c_false;
        u32 pred = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        if (in->op == OP_CSET) set_u(c, d, bool_word(c, combine(c, BITS(w, 45, 2), cond, pred), BITS(w, 44, 1)));
        else {
            set_p(c, BITS(w, 3, 3), combine(c, BITS(w, 45, 2), cond, pred));
            set_p(c, BITS(w, 0, 3), combine(c, BITS(w, 45, 2), lnot(c, cond), pred));
        }
        break;
    }
    case OP_HSET2_reg: case OP_HSET2_cbuf: case OP_HSET2_imm:
    case OP_HSETP2_reg: case OP_HSETP2_cbuf: case OP_HSETP2_imm: {   /* compare two half pairs */
        int setp = in->op == OP_HSETP2_reg || in->op == OP_HSETP2_cbuf || in->op == OP_HSETP2_imm;
        u32 b, swb, abs_b = 0, neg_b = 0, cmp, flag;   /* flag: HSET2 BF (1.0 for true), HSETP2 H_AND */
        if (form == FORM_REG) {
            b = R(c, BITS(w, 20, 8)); swb = BITS(w, 28, 2); abs_b = BITS(w, 30, 1); neg_b = BITS(w, 31, 1);
            cmp = BITS(w, 35, 4); flag = BITS(w, 49, 1);
        } else if (form == FORM_CBUF) {
            b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); swb = 1; cmp = BITS(w, 49, 4); flag = BITS(w, 53, 1);
            neg_b = BITS(w, 56, 1); abs_b = setp ? BITS(w, 54, 1) : 0;
        } else {
            b = half_imm(c, w); swb = 0; cmp = BITS(w, 49, 4); flag = BITS(w, 53, 1);
        }
        u32 a = R(c, ra), swa = BITS(w, 47, 2), abs_a = BITS(w, 44, 1), neg_a = BITS(w, 43, 1);
        u32 pred = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 lo = combine(c, BITS(w, 45, 2), fcompare(c, cmp, absneg(c, half(c, a, swa, 0), abs_a, neg_a), absneg(c, half(c, b, swb, 0), abs_b, neg_b)), pred);
        u32 hi = combine(c, BITS(w, 45, 2), fcompare(c, cmp, absneg(c, half(c, a, swa, 1), abs_a, neg_a), absneg(c, half(c, b, swb, 1), abs_b, neg_b)), pred);
        if (!setp) {
            u32 t = U(c, flag ? 0x3C00u : 0xFFFFu);
            set_u(c, d, bor(c, sel(c, c->t_u32, lo, t, c->c_u0), shl(c, sel(c, c->t_u32, hi, t, c->c_u0), U(c, 16))));
        } else if (flag) {
            u32 both = land(c, lo, hi);
            set_p(c, BITS(w, 3, 3), both);
            set_p(c, BITS(w, 0, 3), lnot(c, both));
        } else {
            set_p(c, BITS(w, 3, 3), lo);
            set_p(c, BITS(w, 0, 3), hi);
        }
        break;
    }
    case OP_F2F_reg: case OP_F2F_cbuf: case OP_F2F_imm: {
        u32 dst = BITS(w, 8, 2), src = BITS(w, 10, 2), rop = BITS(w, 39, 4) & 0xB;
        u32 x = src == 1 ? f16tof32(c, BITS(w, 41, 1) ? shr(c, b_u(c, w, form), U(c, 16)) : b_u(c, w, form)) : b_f(c, w, form);
        x = absneg(c, x, BITS(w, 49, 1), BITS(w, 45, 1));
        if (src == dst) {
            switch (rop) {
            case 8: x = glsl1(c, c->t_f32, GLSLstd450RoundEven, x); break;
            case 9: x = glsl1(c, c->t_f32, GLSLstd450Floor, x); break;
            case 10: x = glsl1(c, c->t_f32, GLSLstd450Ceil, x); break;
            case 11: x = glsl1(c, c->t_f32, GLSLstd450Trunc, x); break;
            }
        }
        if (BITS(w, 50, 1)) x = saturate(c, x);
        if (dst == 1) set_u(c, d, f32tof16(c, x));
        else set_f(c, d, x);
        break;
    }
    case OP_F2I_reg: case OP_F2I_cbuf: case OP_F2I_imm: {
        u32 dst = BITS(w, 8, 2), src = BITS(w, 10, 2), is_signed = BITS(w, 12, 1), rnd = BITS(w, 39, 2);
        u32 x = src == 1 ? f16tof32(c, BITS(w, 41, 1) ? shr(c, b_u(c, w, form), U(c, 16)) : b_u(c, w, form)) : b_f(c, w, form);
        x = absneg(c, x, BITS(w, 45, 1), BITS(w, 49, 1));
        static const u32 rf[4] = {GLSLstd450RoundEven, GLSLstd450Floor, GLSLstd450Ceil, GLSLstd450Trunc};
        x = glsl1(c, c->t_f32, rf[rnd], x);
        if (dst == 1) x = is_signed ? glsl3(c, c->t_f32, GLSLstd450NClamp, x, Fc(c, -32768.0f), Fc(c, 32767.0f))
                                    : glsl3(c, c->t_f32, GLSLstd450NClamp, x, c->c_f0, Fc(c, 65535.0f));
        set_u(c, d, f2i(c, x, is_signed));
        break;
    }
    case OP_I2F_reg: case OP_I2F_cbuf: case OP_I2F_imm: {
        u32 ff = BITS(w, 8, 2), fmt = BITS(w, 10, 2), is_signed = BITS(w, 13, 1), sl = BITS(w, 41, 2);
        u32 x = b_u(c, w, form);
        if (fmt == 0) x = is_signed ? sext(c, x, 24 - sl * 8, 24) : band(c, shr(c, x, U(c, sl * 8)), U(c, 0xFF));
        else if (fmt == 1) x = is_signed ? sext(c, x, 16 - sl * 8, 16) : band(c, shr(c, x, U(c, sl * 8)), U(c, 0xFFFF));
        if (BITS(w, 49, 1) && is_signed) x = asuint(c, glsl1(c, c->t_i32, GLSLstd450SAbs, asint(c, x)));
        u32 f = is_signed ? SPV_VAL(B, SpvOpConvertSToF, c->t_f32, asint(c, x)) : SPV_VAL(B, SpvOpConvertUToF, c->t_f32, x);
        if (BITS(w, 45, 1)) f = SPV_VAL(B, SpvOpFNegate, c->t_f32, f);
        if (ff == 1) set_u(c, d, f32tof16(c, f));
        else set_f(c, d, f);
        break;
    }
    case OP_I2I_reg: case OP_I2I_cbuf: case OP_I2I_imm: {
        u32 dfmt = BITS(w, 8, 2), sfmt = BITS(w, 10, 2), dsig = BITS(w, 12, 1), ssig = BITS(w, 13, 1), sl = BITS(w, 41, 3);
        u32 x = b_u(c, w, form);
        u32 bits = sfmt == 0 ? 8 : sfmt == 1 ? 16 : 32;
        if (bits < 32) x = ssig ? sext(c, x, 32 - bits - sl * 8, 32 - bits) : band(c, shr(c, x, U(c, sl * 8)), U(c, (1u << bits) - 1));
        if (BITS(w, 49, 1)) x = asuint(c, glsl1(c, c->t_i32, GLSLstd450SAbs, asint(c, x)));
        if (BITS(w, 45, 1)) x = isub(c, c->c_u0, x);
        u32 dbits = dfmt == 0 ? 8 : dfmt == 1 ? 16 : 32;
        if (BITS(w, 50, 1) && dbits < 32)
            x = dsig ? asuint(c, glsl3(c, c->t_i32, GLSLstd450SClamp, asint(c, x), I(c, -(1 << (dbits - 1))), I(c, (1 << (dbits - 1)) - 1)))
                     : asuint(c, glsl3(c, c->t_i32, GLSLstd450SClamp, asint(c, x), I(c, 0), I(c, (1 << dbits) - 1)));
        else if (dbits < 32) x = band(c, x, U(c, (1u << dbits) - 1));
        set_u(c, d, x);
        break;
    }
    /* -- half precision -- */
    case OP_HADD2_reg: case OP_HADD2_cbuf: case OP_HADD2_imm: case OP_HADD2_32I: emit_half(c, in, 0); break;
    case OP_HMUL2_reg: case OP_HMUL2_cbuf: case OP_HMUL2_imm: case OP_HMUL2_32I: emit_half(c, in, 1); break;
    case OP_HFMA2_reg: case OP_HFMA2_rc: case OP_HFMA2_cr: case OP_HFMA2_imm: case OP_HFMA2_32I: emit_half(c, in, 2); break;
    /* -- integer -- */
    case OP_IADD_reg: case OP_IADD_cbuf: case OP_IADD_imm: case OP_IADD32I: {
        int is32 = in->op == OP_IADD32I;
        u32 po = is32 ? BITS(w, 55, 2) == 3 : BITS(w, 48, 2) == 3;
        u32 neg_a = is32 ? (BITS(w, 56, 1) && !po) : BITS(w, 49, 1), neg_b = is32 ? 0 : (BITS(w, 48, 1) && !po);
        u32 x = is32 ? BITS(w, 53, 1) : BITS(w, 43, 1), cc = is32 ? BITS(w, 52, 1) : BITS(w, 47, 1);
        u32 a = R(c, ra), b = b_u(c, w, form);
        if (neg_a) a = isub(c, c->c_u0, a);
        if (neg_b) b = isub(c, c->c_u0, b);
        if (x || cc) {
            make_cc(c);
            u32 s = iadd(c, a, b), t = iadd(c, s, x ? cc_load(c, &c->cc_c, c->t_u32) : c->c_u0);
            if (cc) {
                SPV_DO(B, SpvOpStore, c->cc_c, sel(c, c->t_u32, lor(c, ult(c, s, a), ult(c, t, s)), U(c, 1), c->c_u0));
                SPV_DO(B, SpvOpStore, c->cc_z, ieq(c, t, c->c_u0));
                SPV_DO(B, SpvOpStore, c->cc_s, op2(c, SpvOpSLessThan, c->t_bool, asint(c, t), I(c, 0)));
            }
            set_u(c, d, po ? iadd(c, t, U(c, 1)) : t);
        } else set_u(c, d, po ? iadd(c, iadd(c, a, b), U(c, 1)) : iadd(c, a, b));
        break;
    }
    case OP_ISCADD_reg: case OP_ISCADD_cbuf: case OP_ISCADD_imm: case OP_ISCADD32I: {
        int is32 = in->op == OP_ISCADD32I;
        u32 scale = is32 ? BITS(w, 53, 5) : BITS(w, 39, 5);
        u32 neg_b = is32 ? 0 : BITS(w, 48, 1), neg_a = is32 ? 0 : BITS(w, 49, 1), po = neg_a && neg_b;
        u32 a = R(c, ra), b = b_u(c, w, form);
        if (po) b = iadd(c, b, U(c, 1));
        else {
            if (neg_a) a = isub(c, c->c_u0, a);
            if (neg_b) b = isub(c, c->c_u0, b);
        }
        set_u(c, d, iadd(c, shl(c, a, U(c, scale)), b));
        break;
    }
    case OP_IMNMX_reg: case OP_IMNMX_cbuf: case OP_IMNMX_imm: {
        u32 a = R(c, ra), b = b_u(c, w, form), p = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        if (BITS(w, 48, 1))
            set_u(c, d, asuint(c, sel(c, c->t_i32, p, glsl2(c, c->t_i32, GLSLstd450SMin, asint(c, a), asint(c, b)), glsl2(c, c->t_i32, GLSLstd450SMax, asint(c, a), asint(c, b)))));
        else set_u(c, d, sel(c, c->t_u32, p, glsl2(c, c->t_u32, GLSLstd450UMin, a, b), glsl2(c, c->t_u32, GLSLstd450UMax, a, b)));
        break;
    }
    case OP_ISETP_reg: case OP_ISETP_cbuf: case OP_ISETP_imm: {
        u32 cmp = icompare(c, BITS(w, 49, 3), BITS(w, 48, 1), R(c, ra), b_u(c, w, form)), bp = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 bop = BITS(w, 45, 2);   /* (ISETP.X: a plain compare, as in HLSL) */
        set_p(c, BITS(w, 3, 3), combine(c, bop, cmp, bp));
        set_p(c, BITS(w, 0, 3), combine(c, bop, lnot(c, cmp), bp));
        break;
    }
    case OP_ISET_reg: case OP_ISET_cbuf: case OP_ISET_imm: {
        u32 r = combine(c, BITS(w, 45, 2), icompare(c, BITS(w, 49, 3), BITS(w, 48, 1), R(c, ra), b_u(c, w, form)), PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)));
        set_u(c, d, bool_word(c, r, BITS(w, 44, 1)));
        break;
    }
    case OP_LOP_reg: case OP_LOP_cbuf: case OP_LOP_imm: case OP_LOP32I: {
        int is32 = in->op == OP_LOP32I;
        u32 op = is32 ? BITS(w, 53, 2) : BITS(w, 41, 2);
        u32 a = R(c, ra), b = b_u(c, w, form);
        if (is32 ? BITS(w, 55, 1) : BITS(w, 39, 1)) a = bnot(c, a);
        if (is32 ? BITS(w, 56, 1) : BITS(w, 40, 1)) b = bnot(c, b);
        u32 r = op == 3 ? b : op == 0 ? band(c, a, b) : op == 1 ? bor(c, a, b) : bxor(c, a, b);
        u32 pop = is32 ? 0 : BITS(w, 44, 2), dp = is32 ? PT : BITS(w, 48, 3);
        set_u(c, d, r);
        if (pop || dp != PT)
            set_p(c, dp, pop == 0 ? c->c_false : pop == 1 ? c->c_true : pop == 2 ? ieq(c, r, c->c_u0) : op2(c, SpvOpINotEqual, c->t_bool, r, c->c_u0));
        break;
    }
    case OP_LOP3_reg: case OP_LOP3_cbuf: case OP_LOP3_imm: {   /* any function of three inputs: the lut's minterms */
        u32 lut = in->op == OP_LOP3_reg ? BITS(w, 28, 8) : BITS(w, 48, 8);
        u32 a = R(c, ra), b = b_u(c, w, in->op == OP_LOP3_imm ? FORM_IMM : form), cc = R(c, BITS(w, 39, 8));
        u32 na = bnot(c, a), nb = bnot(c, b), nc = bnot(c, cc), r = c->c_u0;
        for (u32 k = 0; k < 8; k++)
            if (lut >> k & 1) r = bor(c, r, band(c, band(c, k & 4 ? a : na, k & 2 ? b : nb), k & 1 ? cc : nc));
        set_u(c, d, r);
        break;
    }
    case OP_SHL_reg: case OP_SHL_cbuf: case OP_SHL_imm: {
        u32 a = R(c, ra), s = b_u(c, w, form);
        if (BITS(w, 39, 1)) set_u(c, d, shl(c, a, band(c, s, U(c, 31))));
        else set_u(c, d, sel(c, c->t_u32, ult(c, s, U(c, 32)), shl(c, a, band(c, s, U(c, 31))), c->c_u0));
        break;
    }
    case OP_SHR_reg: case OP_SHR_cbuf: case OP_SHR_imm: {
        u32 a = R(c, ra), s = b_u(c, w, form);
        if (BITS(w, 40, 1)) a = SPV_VAL(B, SpvOpBitReverse, c->t_u32, a);
        u32 v;
        if (BITS(w, 48, 1)) v = sar(c, a, BITS(w, 39, 1) ? band(c, s, U(c, 31)) : glsl2(c, c->t_u32, GLSLstd450UMin, s, U(c, 31)));
        else if (BITS(w, 39, 1)) v = shr(c, a, band(c, s, U(c, 31)));
        else v = sel(c, c->t_u32, ult(c, s, U(c, 32)), shr(c, a, band(c, s, U(c, 31))), c->c_u0);
        set_u(c, d, v);
        break;
    }
    case OP_BFE_reg: case OP_BFE_cbuf: case OP_BFE_imm: {
        u32 base = R(c, ra), b = b_u(c, w, form);
        if (BITS(w, 40, 1)) base = SPV_VAL(B, SpvOpBitReverse, c->t_u32, base);
        u32 o = band(c, b, U(c, 0xFF)), nb = band(c, shr(c, b, U(c, 8)), U(c, 0xFF));
        u32 none = ieq(c, nb, c->c_u0), past = uge(c, o, U(c, 32));
        if (BITS(w, 48, 1)) {
            /* ((int)(base << (32 - min(o + n, 32)))) >> (32 - min(n, 32 - o)) */
            u32 top = glsl2(c, c->t_u32, GLSLstd450UMin, iadd(c, o, nb), U(c, 32));
            u32 left = band(c, isub(c, U(c, 32), top), U(c, 31));
            u32 keep = glsl2(c, c->t_u32, GLSLstd450UMin, nb, isub(c, U(c, 32), glsl2(c, c->t_u32, GLSLstd450UMin, o, U(c, 32))));
            u32 right = band(c, isub(c, U(c, 32), keep), U(c, 31));
            u32 field = sar(c, shl(c, base, left), right);
            u32 sign = sel(c, c->t_u32, op2(c, SpvOpSLessThan, c->t_bool, asint(c, base), I(c, 0)), U(c, 0xFFFFFFFFu), c->c_u0);
            set_u(c, d, sel(c, c->t_u32, none, c->c_u0, sel(c, c->t_u32, past, sign, field)));
        } else {
            u32 mask = sel(c, c->t_u32, uge(c, nb, U(c, 32)), U(c, 0xFFFFFFFFu), isub(c, shl(c, U(c, 1), band(c, nb, U(c, 31))), U(c, 1)));
            u32 field = band(c, shr(c, base, band(c, o, U(c, 31))), mask);
            set_u(c, d, sel(c, c->t_u32, lor(c, none, past), c->c_u0, field));
        }
        break;
    }
    case OP_BFI_reg: case OP_BFI_rc: case OP_BFI_cr: case OP_BFI_imm: {
        u32 ins = R(c, ra), src, base;
        switch (form) {
        case FORM_REG: src = R(c, BITS(w, 20, 8)); base = R(c, BITS(w, 39, 8)); break;
        case FORM_RC: src = R(c, BITS(w, 39, 8)); base = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); break;
        case FORM_CR: src = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); base = R(c, BITS(w, 39, 8)); break;
        default: src = U(c, shader_imm20(w)); base = R(c, BITS(w, 39, 8)); break;
        }
        u32 o = band(c, src, U(c, 0xFF));
        u32 nb = glsl2(c, c->t_u32, GLSLstd450UMin, band(c, shr(c, src, U(c, 8)), U(c, 0xFF)), isub(c, U(c, 32), glsl2(c, c->t_u32, GLSLstd450UMin, o, U(c, 32))));
        u32 mask = sel(c, c->t_u32, uge(c, nb, U(c, 32)), U(c, 0xFFFFFFFFu), isub(c, shl(c, U(c, 1), band(c, nb, U(c, 31))), U(c, 1)));
        u32 so = band(c, o, U(c, 31));
        u32 merged = bor(c, band(c, base, bnot(c, shl(c, mask, so))), shl(c, band(c, ins, mask), so));
        set_u(c, d, sel(c, c->t_u32, uge(c, o, U(c, 32)), base, merged));
        break;
    }
    case OP_XMAD_reg: case OP_XMAD_rc: case OP_XMAD_cr: case OP_XMAD_imm: emit_xmad(c, in); break;
    case OP_MOV_reg: case OP_MOV_cbuf: case OP_MOV_imm: set_u(c, d, b_u(c, w, form)); break;
    case OP_MOV32I: set_u(c, d, b_u(c, w, FORM_32I)); break;
    case OP_SEL_reg: case OP_SEL_cbuf: case OP_SEL_imm:
        set_u(c, d, sel(c, c->t_u32, PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)), R(c, ra), b_u(c, w, form)));
        break;
    case OP_R2P_reg: case OP_R2P_cbuf: case OP_R2P_imm: {   /* predicates from register bits, under a mask */
        if (BITS(w, 40, 1)) { unsupported(c, in, "R2P.CC"); break; }
        u32 sh = BITS(w, 41, 2) * 8, mask = b_u(c, w, form), src = R(c, ra);
        for (u32 p = 0; p < 7; p++) {
            u32 bit = U(c, p + sh), on = op2(c, SpvOpINotEqual, c->t_bool, band(c, shr(c, mask, bit), U(c, 1)), c->c_u0);
            u32 v = op2(c, SpvOpINotEqual, c->t_bool, band(c, shr(c, src, bit), U(c, 1)), c->c_u0);
            set_p(c, p, sel(c, c->t_bool, on, v, PRED(c, p, 0)));
        }
        break;
    }
    case OP_IADD3_reg: case OP_IADD3_cbuf: case OP_IADD3_imm: {
        u32 a = R(c, ra), b = b_u(c, w, form), cc = R(c, BITS(w, 39, 8));
        if (form == FORM_REG) {   /* halves, and a 16-bit shift of the partial sum */
            u32 ha = BITS(w, 35, 2), hb = BITS(w, 33, 2), hc = BITS(w, 31, 2);
            if (ha == 1) a = band(c, a, U(c, 0xFFFF)); else if (ha == 2) a = shr(c, a, U(c, 16));
            if (hb == 1) b = band(c, b, U(c, 0xFFFF)); else if (hb == 2) b = shr(c, b, U(c, 16));
            if (hc == 1) cc = band(c, cc, U(c, 0xFFFF)); else if (hc == 2) cc = shr(c, cc, U(c, 16));
        }
        if (BITS(w, 51, 1)) a = isub(c, c->c_u0, a);
        if (BITS(w, 50, 1)) b = isub(c, c->c_u0, b);
        if (BITS(w, 49, 1)) cc = isub(c, c->c_u0, cc);
        u32 ab = iadd(c, a, b), shift = form == FORM_REG ? BITS(w, 37, 2) : 0;
        if (shift == 1) ab = shr(c, ab, U(c, 16));
        else if (shift == 2) ab = shl(c, ab, U(c, 16));
        set_u(c, d, iadd(c, ab, cc));
        break;
    }
    case OP_PSET: {   /* predicates combined into a register */
        u32 pa = PRED(c, BITS(w, 12, 3), BITS(w, 15, 1)), pb = PRED(c, BITS(w, 29, 3), BITS(w, 32, 1)), pc = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 r = combine(c, BITS(w, 45, 2), combine(c, BITS(w, 24, 2), pa, pb), pc);
        set_u(c, d, bool_word(c, r, BITS(w, 44, 1)));
        break;
    }
    case OP_PSETP: {
        u32 pa = PRED(c, BITS(w, 12, 3), BITS(w, 15, 1)), pb = PRED(c, BITS(w, 29, 3), BITS(w, 32, 1)), pc = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 l = combine(c, BITS(w, 24, 2), pa, pb);
        set_p(c, BITS(w, 3, 3), combine(c, BITS(w, 45, 2), l, pc));
        set_p(c, BITS(w, 0, 3), combine(c, BITS(w, 45, 2), lnot(c, l), pc));
        break;
    }
    /* -- constant buffers and attributes -- */
    case OP_LDC: {
        u32 idx = BITS(w, 36, 5), size = BITS(w, 48, 3), mode = BITS(w, 44, 2);
        i32 off = (i32)(BITS(w, 20, 16) << 16) >> 16;
        if (mode != 0) { unsupported(c, in, "LDC addressing mode"); break; }
        c->info->cbuf_mask |= 1u << idx;
        if (ra != RZ) {   /* indexed: all of the buffer may be read - unless this is a jump table's load (found first) */
            u32 end = ~0u;
            for (u32 t = 0; t < c->info->ntables; t++)
                if (c->info->table[t].bank == idx && (i32)c->info->table[t].offset == off) end = (u32)off + 4 * c->info->table[t].count;
            if (end == ~0u) c->info->cbuf_extent[idx] = ~0u;
            else if (c->info->cbuf_extent[idx] < end) c->info->cbuf_extent[idx] = end;
        } else if (off >= 0 && c->info->cbuf_extent[idx] < ((u32)off & ~7u) + 8) c->info->cbuf_extent[idx] = ((u32)off & ~7u) + 8;
        u32 addr = iadd(c, R(c, ra), U(c, (u32)off));
        if (size == 4) set_u(c, d, cbuf_at(c, idx, band(c, addr, U(c, ~3u))));
        else if (size == 5) {
            u32 a8 = band(c, addr, U(c, ~7u)), lo = cbuf_at(c, idx, a8), hi = cbuf_at(c, idx, iadd(c, a8, U(c, 4)));
            set_u(c, d, lo);
            set_u(c, d == RZ ? RZ : d + 1, hi);
        } else {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1;
            u32 word = shr(c, cbuf_at(c, idx, band(c, addr, U(c, ~3u))), shl(c, band(c, addr, U(c, 3)), U(c, 3)));
            set_u(c, d, sg ? sext(c, word, 32 - bits, 32 - bits) : band(c, word, U(c, (1u << bits) - 1)));
        }
        break;
    }
    case OP_ALD: {
        if (c->stage != SHADER_VERTEX) { unsupported(c, in, "ALD outside a vertex shader"); break; }
        if (BITS(w, 8, 8) != RZ) { unsupported(c, in, "indexed ALD"); break; }
        u32 off = BITS(w, 20, 10), n = BITS(w, 47, 2) + 1;
        for (u32 e = 0; e < n; e++) set_u(c, d == RZ ? RZ : d + e, vs_input(c, off + 4 * e));
        break;
    }
    case OP_AST: {
        if (c->stage != SHADER_VERTEX) { unsupported(c, in, "AST outside a vertex shader"); break; }
        u32 off = BITS(w, 20, 10), n = BITS(w, 47, 2) + 1;
        for (u32 e = 0; e < n; e++) vs_output(c, off + 4 * e, R(c, d == RZ ? RZ : d + e));
        break;
    }
    case OP_IPA: {
        if (c->stage != SHADER_PIXEL) { unsupported(c, in, "IPA outside a pixel shader"); break; }
        u32 attr = BITS(w, 30, 8) * 4, mode = BITS(w, 54, 2);
        int persp;
        u32 v = ps_input(c, attr, &persp);
        if (persp) {   /* (Vulkan has interpolated it with perspective already: the program multiplies by w again) */
            u32 fc = load_in(c, c->t_v4f, c->frag_coord);
            v = f_mul(c, v, SPV_VAL(B, SpvOpCompositeExtract, c->t_f32, fc, 3));
        }
        if (mode == 1) v = f_mul(c, v, F(c, BITS(w, 20, 8)));
        if (BITS(w, 51, 1)) v = saturate(c, v);
        if (attr == 0x3FC) set_u(c, d, sel(c, c->t_u32, load_in(c, c->t_bool, c->front), U(c, 0xFFFFFFFFu), c->c_u0));
        else set_f(c, d, v);
        break;
    }
    case OP_S2R: {
        u32 sr = BITS(w, 20, 8);
        if (c->stage == SHADER_COMPUTE && !c->local_id) {
            spv_prologue_enter(B);
            c->local_id = builtin_in(c, c->t_v3u, SpvBuiltInLocalInvocationId, 0);
            c->group_id = builtin_in(c, c->t_v3u, SpvBuiltInWorkgroupId, 0);
            spv_prologue_leave(B);
        }
#define TID(i) (c->local_id ? SPV_VAL(B, SpvOpCompositeExtract, c->t_u32, load_in(c, c->t_v3u, c->local_id), i) : c->c_u0)
#define CTA(i) (c->group_id ? SPV_VAL(B, SpvOpCompositeExtract, c->t_u32, load_in(c, c->t_v3u, c->group_id), i) : c->c_u0)
        switch (sr) {
        case 0: set_u(c, d, c->stage == SHADER_COMPUTE ? band(c, TID(0), U(c, 31)) : c->c_u0); break;   /* lane */
        case 0x20: set_u(c, d, bor(c, bor(c, TID(0), shl(c, TID(1), U(c, 16))), shl(c, TID(2), U(c, 26)))); break;  /* thread id, packed */
        case 0x21: case 0x22: case 0x23: set_u(c, d, TID(sr - 0x21)); break;
        case 0x25: case 0x26: case 0x27: set_u(c, d, CTA(sr - 0x25)); break;
        case 0x28: set_u(c, d, U(c, c->local[0] | c->local[1] << 16 | c->local[2] << 26)); break;
        case 18: set_u(c, d, U(c, 0x3F800000u)); break;                         /* y direction: +1 */
        case 30: case 31: set_u(c, d, U(c, 0x3F800000u)); break;                /* w scale factors: 1.0 */
        default: set_u(c, d, c->c_u0); break;                                   /* thread kill, invocation id/info, ... */
        }
#undef TID
#undef CTA
        break;
    }
    case OP_KIL: {
        u32 cond = flow_cond(c, BITS(w, 0, 5));
        if (!cond) break;
        c->info->kills = 1;
        if (c->stage != SHADER_PIXEL) break;
        spv_extension(B, "SPV_EXT_demote_to_helper_invocation");
        spv_capability(B, SpvCapabilityDemoteToHelperInvocationEXT);
        if (cond == c->c_true) spv_do(B, SpvOpDemoteToHelperInvocationEXT, 0, NULL);
        else {
            u32 merge = if_begin(c, cond);
            spv_do(B, SpvOpDemoteToHelperInvocationEXT, 0, NULL);
            if_end(c, merge);
        }
        break;
    }
    case OP_DEPBAR: case OP_NOP: case OP_BAR: case OP_MEMBAR: case OP_VOTE_vtg: break;
    case OP_VOTE: set_u(c, d, U(c, 0xFFFFFFFFu)); break;
    case OP_TEXS: emit_texs(c, in); break;
    case OP_TEX: emit_tex(c, in); break;
    case OP_TLD4: emit_tld4(c, in, 0); break;
    case OP_TLD4S: emit_tld4(c, in, 1); break;
    case OP_TLDS: emit_tlds(c, in); break;
    case OP_TMML: emit_tmml(c, in); break;
    case OP_LDG: case OP_STG: case OP_LDS: case OP_STS: case OP_LDL: case OP_STL: emit_memory(c, in); break;
    default: unsupported(c, in, "instruction"); break;
    }
}

static void emit_guarded(Sx *c, const ShaderInsn *in) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
    if (p == PT && neg) return;   /* never executes */
    resume(c);
    if (p == PT) { emit_insn(c, in); return; }
    u32 merge = if_begin(c, PRED(c, p, neg));
    emit_insn(c, in);
    if_end(c, merge);
}

/* ---- whole programs ------------------------------------------------------------------------ */
static void make_types(Sx *c) {
    c->t_void = spv_type_void(B);
    c->t_bool = spv_type_bool(B);
    c->t_u32 = spv_type_int(B, 32, 0);
    c->t_i32 = spv_type_int(B, 32, 1);
    c->t_f32 = spv_type_float(B, 32);
    c->t_v2u = spv_type_vector(B, c->t_u32, 2);
    c->t_v3u = spv_type_vector(B, c->t_u32, 3);
    c->t_v4u = spv_type_vector(B, c->t_u32, 4);
    c->t_v2f = spv_type_vector(B, c->t_f32, 2);
    c->t_v3f = spv_type_vector(B, c->t_f32, 3);
    c->t_v4f = spv_type_vector(B, c->t_f32, 4);
    c->p_func_u32 = spv_type_pointer(B, SpvStorageClassFunction, c->t_u32);
    c->p_func_bool = spv_type_pointer(B, SpvStorageClassFunction, c->t_bool);
    c->p_func_f32 = spv_type_pointer(B, SpvStorageClassFunction, c->t_f32);
    c->p_func_v4f = spv_type_pointer(B, SpvStorageClassFunction, c->t_v4f);
    c->p_sb_u32 = spv_type_pointer(B, SpvStorageClassStorageBuffer, c->t_u32);
    c->p_uni_u32 = spv_type_pointer(B, SpvStorageClassUniform, c->t_u32);
    c->p_uni_f32 = spv_type_pointer(B, SpvStorageClassUniform, c->t_f32);
    c->p_in_f32 = spv_type_pointer(B, SpvStorageClassInput, c->t_f32);
    c->p_in_v4f = spv_type_pointer(B, SpvStorageClassInput, c->t_v4f);
    c->p_out_f32 = spv_type_pointer(B, SpvStorageClassOutput, c->t_f32);
    c->p_out_v4f = spv_type_pointer(B, SpvStorageClassOutput, c->t_v4f);
    c->c_true = spv_const_bool(B, 1);
    c->c_false = spv_const_bool(B, 0);
    c->c_u0 = U(c, 0);
    c->c_f0 = Fc(c, 0);
    c->c_f1 = Fc(c, 1);
}

/* The stage's fixed inputs and outputs, before the body (the pixel program's interface is its
 * vertex program's outputs, as the HLSL translator makes it). */
static void make_interface(Sx *c) {
    if (c->stage == SHADER_VERTEX) {
        c->out_pos = spv_global(B, c->p_out_v4f, SpvStorageClassOutput);
        decorate(c, c->out_pos, SpvDecorationBuiltIn, SpvBuiltInPosition);
        add_interface(c, c->out_pos);
        c->out_psize = spv_global(B, spv_type_pointer(B, SpvStorageClassOutput, c->t_f32), SpvStorageClassOutput);   /* (points: 1 pixel, as Direct3D draws them) */
        decorate(c, c->out_psize, SpvDecorationBuiltIn, SpvBuiltInPointSize);
        add_interface(c, c->out_psize);
        c->opos = spv_local_init(B, c->p_func_v4f, spv_const_composite(B, c->t_v4f, 4, (u32[]){c->c_f0, c->c_f0, c->c_f0, c->c_f1}));
    } else if (c->stage == SHADER_PIXEL) {
        c->frag_coord = builtin_in(c, c->t_v4f, SpvBuiltInFragCoord, 0);
        c->front = builtin_in(c, c->t_bool, SpvBuiltInFrontFacing, 0);
        for (u32 n = 0; n < 32; n++) {
            if (!(c->vs_outputs >> n & 1)) continue;
            u8 m = c->prog[0x18 + n];
            u32 mode = 0;
            for (u32 k = 0; k < 4 && !mode; k++) mode = (m >> (k * 2)) & 3;
            c->info->interp[n] = (u8)mode;
            c->in_attr[n] = spv_global(B, c->p_in_v4f, SpvStorageClassInput);
            decorate(c, c->in_attr[n], SpvDecorationLocation, n);
            if (mode == 1) decorate0(c, c->in_attr[n], SpvDecorationFlat);
            else if (mode == 3) decorate0(c, c->in_attr[n], SpvDecorationNoPerspective);
            add_interface(c, c->in_attr[n]);
        }
        u32 target = *(const u32 *)(c->prog + 0x48), flags = *(const u32 *)(c->prog + 0x4C);
        for (u32 rt = 0; rt < 8; rt++) {
            if (!((target >> (rt * 4)) & 15)) continue;
            c->out_color[rt] = spv_global(B, c->p_out_v4f, SpvStorageClassOutput);
            decorate(c, c->out_color[rt], SpvDecorationLocation, rt);
            add_interface(c, c->out_color[rt]);
        }
        if (flags & 2) {
            c->out_depth = spv_global(B, c->p_out_f32, SpvStorageClassOutput);
            decorate(c, c->out_depth, SpvDecorationBuiltIn, SpvBuiltInFragDepth);
            add_interface(c, c->out_depth);
        }
    }
}

/* ---- control flow ------------------------------------------------------------------------------ */
/* A control instruction's predicate and flow test together, for the tree: when it acts. */
static u32 tree_guard(Sx *c, const ShaderInsn *in) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1), cond = flow_cond(c, BITS(in->w, 0, 5));
    if (!cond) return c->c_false;
    if (p == PT) return neg ? c->c_false : cond;
    u32 pr = PRED(c, p, neg);
    return cond == c->c_true ? pr : land(c, pr, cond);
}

static u32 load(Sx *c, u32 type, u32 var) { return SPV_VAL(B, SpvOpLoad, type, var); }
static void store(Sx *c, u32 var, u32 v) { SPV_DO(B, SpvOpStore, var, v); }
static u32 elem(Sx *c, u32 array, u32 index) { return SPV_VAL(B, SpvOpAccessChain, c->p_func_u32, array, band(c, index, U(c, 15))); }

/* The dispatch loop: every block a case of a switch, in a loop driven by pc (as gpu_shader.c's
 * emit_dispatch), the SSY/PBK stacks per thread. A block's taken branch sets npc and breaks. */
typedef struct Dispatch { u32 pc, npc, ssy, pbk, ssp, psp, merge; } Dispatch;

static void push(Sx *c, u32 stack, u32 sp, u32 value) {
    u32 at = load(c, c->t_u32, sp);
    store(c, elem(c, stack, at), value);
    store(c, sp, iadd(c, at, U(c, 1)));
}

/* One control instruction in the dispatch loop; 1 if it ends the block. */
static int emit_flow(Sx *c, const Dispatch *dp, const ShaderInsn *in, u32 next) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
    if (p == PT && neg) return 0;
    resume(c);
    u32 cond = flow_cond(c, BITS(in->w, 0, 5)), taken = cond != 0;
    u32 guard = p == PT ? 0 : PRED(c, p, neg);
    if (cond && cond != c->c_true) guard = guard ? land(c, guard, cond) : cond;   /* predicate and flow test */
    if (!next) next = 0xFFFFFFFFu;
    switch (in->op) {
    case OP_SSY: push(c, dp->ssy, dp->ssp, U(c, shader_branch_target(in))); return 0;
    case OP_PBK: push(c, dp->pbk, dp->psp, U(c, shader_branch_target(in))); return 0;
    case OP_BRA:
        if (!taken) return 0;
        store(c, dp->npc, guard ? sel(c, c->t_u32, guard, U(c, shader_branch_target(in)), U(c, next)) : U(c, shader_branch_target(in)));
        branch(c, dp->merge);
        return 1;
    case OP_SYNC: case OP_BRK: {
        if (!taken) return 0;
        u32 sp = in->op == OP_SYNC ? dp->ssp : dp->psp, stack = in->op == OP_SYNC ? dp->ssy : dp->pbk;
        u32 t = isub(c, load(c, c->t_u32, sp), U(c, 1)), to = load(c, c->t_u32, elem(c, stack, t));
        if (!guard) { store(c, sp, t); store(c, dp->npc, to); }
        else { store(c, sp, sel(c, c->t_u32, guard, t, load(c, c->t_u32, sp))); store(c, dp->npc, sel(c, c->t_u32, guard, to, U(c, next))); }
        branch(c, dp->merge);
        return 1;
    }
    case OP_BRX: {
        u32 t = iadd(c, R(c, BITS(in->w, 8, 8)), U(c, in->addr + 8 + (u32)shader_branch_offset(in->w)));
        store(c, dp->npc, sel(c, c->t_u32, ieq(c, band(c, t, U(c, 31)), U(c, 16)), iadd(c, t, U(c, 8)), t));
        branch(c, dp->merge);
        return 1;
    }
    case OP_EXIT:
        if (!taken) return 0;
        if (!guard) { emit_exit(c); return 1; }
        u32 m = if_begin(c, guard);
        emit_exit(c);
        if_end(c, m);
        return 0;
    }
    return 0;
}

/* for (guard = 0; guard < CFG_LIMIT [&& run]; guard++) { body }: begin opens the body, end closes
 * the loop (its merge block starts) */
typedef struct Loop { u32 header, cont, merge, guard; } Loop;
static Loop loop_begin(Sx *c, u32 run) {
    Loop l = {spv_id(B), spv_id(B), spv_id(B), spv_local(B, c->p_func_u32)};
    u32 check = spv_id(B), body = spv_id(B);
    resume(c);
    store(c, l.guard, c->c_u0);
    branch(c, l.header);
    label(c, l.header);
    SPV_DO(B, SpvOpLoopMerge, l.merge, l.cont, SpvLoopControlMaskNone);
    SPV_DO(B, SpvOpBranch, check);
    label(c, check);
    u32 more = ult(c, load(c, c->t_u32, l.guard), U(c, CFG_LIMIT));
    if (run) more = land(c, load(c, c->t_bool, run), more);
    SPV_DO(B, SpvOpBranchConditional, more, body, l.merge);
    label(c, body);
    return l;
}
static void loop_end(Sx *c, const Loop *l) {
    branch(c, l->cont);
    label(c, l->cont);
    store(c, l->guard, iadd(c, load(c, c->t_u32, l->guard), U(c, 1)));
    SPV_DO(B, SpvOpBranch, l->header);
    c->open = 0;
    label(c, l->merge);
}

static void emit_dispatch(Sx *c) {
    const Cfg *g = c->cfg;
    u32 arr = spv_type_pointer(B, SpvStorageClassFunction, spv_type_array(B, c->t_u32, 16));
    Dispatch dp = {spv_local_init(B, c->p_func_u32, U(c, c->ins[0].addr)), spv_local(B, c->p_func_u32),
                   spv_local_init(B, arr, spv_const_null(B, spv_type_array(B, c->t_u32, 16))),
                   spv_local_init(B, arr, spv_const_null(B, spv_type_array(B, c->t_u32, 16))),
                   spv_local_init(B, c->p_func_u32, c->c_u0), spv_local_init(B, c->p_func_u32, c->c_u0), spv_id(B)};
    Loop l = loop_begin(c, 0);
    u32 pc = load(c, c->t_u32, dp.pc);
    store(c, dp.npc, pc);
    u32 dflt = spv_id(B), *labels = calloc(c->n, sizeof *labels), *ops = malloc(sizeof *ops * (2 + 2 * c->n)), nops = 0;
    ops[nops++] = pc;
    ops[nops++] = dflt;
    for (u32 i = 0; i < c->n; i++)
        if (g->leader[i]) { labels[i] = spv_id(B); ops[nops++] = c->ins[i].addr; ops[nops++] = labels[i]; }
    SPV_DO(B, SpvOpSelectionMerge, dp.merge, SpvSelectionControlMaskNone);
    spv_do(B, SpvOpSwitch, nops, ops);
    c->open = 0;
    int ended = 0;
    for (u32 i = 0; i < c->n; i++) {
        const ShaderInsn *in = &c->ins[i];
        if (g->leader[i]) {
            if (c->open && !ended) { store(c, dp.npc, U(c, in->addr)); branch(c, dp.merge); }   /* (into the next block) */
            branch(c, dp.merge);
            label(c, labels[i]);
            ended = 0;
            u32 addr = U(c, in->addr);
            if (g->ssy_target[i] || g->pbk_target[i])
                for (int k = 0; k < 2; k++) {   /* (the target of the stack's top: popped on arrival) */
                    if (!(k ? g->pbk_target[i] : g->ssy_target[i])) continue;
                    u32 sp = k ? dp.psp : dp.ssp, stack = k ? dp.pbk : dp.ssy, at = load(c, c->t_u32, sp);
                    u32 top = load(c, c->t_u32, elem(c, stack, isub(c, at, U(c, 1))));
                    u32 pop = land(c, op2(c, SpvOpUGreaterThan, c->t_bool, at, c->c_u0), ieq(c, top, addr));
                    store(c, sp, sel(c, c->t_u32, pop, isub(c, at, U(c, 1)), at));
                }
        }
        if (ended) continue;   /* unreachable within the block */
        switch (in->op) {
        case OP_BRA: case OP_BRX: case OP_SSY: case OP_PBK: case OP_SYNC: case OP_BRK: case OP_EXIT:
            ended = emit_flow(c, &dp, in, i + 1 < c->n ? c->ins[i + 1].addr : 0);
            break;
        default: emit_guarded(c, in); break;
        }
    }
    if (c->open && !ended) emit_exit(c);
    branch(c, dp.merge);
    label(c, dflt);
    emit_exit(c);
    label(c, dp.merge);
    store(c, dp.pc, load(c, c->t_u32, dp.npc));
    loop_end(c, &l);
    emit_exit(c);
    free(labels);
    free(ops);
}

/* ---- structured control flow --------------------------------------------------------------------- */
static int arm_empty(const SNode *s) { return !s->n || (s->n == 1 && s->kid[0]->kind == SN_CONTINUE); }
static int arm_breaks(const SNode *s) { return s->n == 1 && s->kid[0]->kind == SN_BREAK; }

/* The tree of gpu_shader_cfg.c, as gpu_shader.c's emit_tree: a loop is left through its run flag,
 * set where a BREAK is (no code follows a BREAK or CONTINUE in its iteration, so the flag only
 * stops the next one); each loop keeps the dispatch loop's guard. */
static void emit_tree(Sx *c, const SNode *s) {
    const Cfg *g = c->cfg;
    switch (s->kind) {
    case SN_SEQ:
        for (u32 k = 0; k < s->n; k++) emit_tree(c, s->kid[k]);
        break;
    case SN_BLOCK: {
        const CfgBlock *b = &g->block[s->block];
        for (u32 i = b->first; i < b->end; i++)
            if (c->flow[i].kind == CF_PLAIN) emit_guarded(c, &c->ins[i]);
        break;
    }
    case SN_IF: {
        const SNode *taken = s->kid[0], *not_taken = s->kid[1];
        resume(c);
        u32 cond = tree_guard(c, &c->ins[g->block[s->block].term]);
        if (arm_empty(taken) && arm_empty(not_taken)) break;
        if (arm_breaks(taken) || arm_breaks(not_taken)) {   /* the run flag, as a value */
            int t = arm_breaks(taken);
            store(c, c->loop_run, t ? lnot(c, cond) : cond);
            const SNode *other = t ? not_taken : taken;
            if (!arm_empty(other)) {
                u32 m = if_begin(c, load(c, c->t_bool, c->loop_run));
                emit_tree(c, other);
                if_end(c, m);
            }
        } else if (arm_empty(not_taken) || arm_empty(taken)) {
            int which = arm_empty(not_taken);
            u32 m = if_begin(c, which ? cond : lnot(c, cond));
            emit_tree(c, which ? taken : not_taken);
            if_end(c, m);
        } else {
            u32 other, m;
            if_else_begin(c, cond, &other, &m);
            emit_tree(c, taken);
            if_else_middle(c, other, m);
            emit_tree(c, not_taken);
            if_end(c, m);
        }
        break;
    }
    case SN_SWITCH: {   /* an indirect branch: its table's targets, in turn; anything else ends the program */
        const CfgBlock *b = &g->block[s->block];
        const ShaderInsn *in = &c->ins[b->term];
        const CfInsn *f = &c->flow[b->term];
        resume(c);
        u32 bx = iadd(c, R(c, BITS(in->w, 8, 8)), U(c, in->addr + 8 + (u32)shader_branch_offset(in->w)));
        bx = sel(c, c->t_u32, ieq(c, band(c, bx, U(c, 31)), U(c, 16)), iadd(c, bx, U(c, 8)), bx);
        u32 merges[64], nm = 0;
        for (u32 k = 0; k < b->nsucc && nm < 64; k++) {
            u32 test = 0;
            for (u32 e = 0; e < f->ntable; e++) {
                int dup = 0;
                for (u32 d = 0; d < e; d++) dup |= f->table[d] == f->table[e];
                if (dup || b->succ[k] == CFG_EXIT || g->block[b->succ[k]].addr != f->table[e]) continue;
                u32 hit = ieq(c, bx, U(c, f->table[e]));
                test = test ? lor(c, test, hit) : hit;
            }
            u32 other;
            if_else_begin(c, test ? test : c->c_false, &other, &merges[nm]);
            emit_tree(c, s->kid[k]);
            if_else_middle(c, other, merges[nm++]);
        }
        emit_exit(c);
        while (nm) if_end(c, merges[--nm]);
        break;
    }
    case SN_LOOP: {
        u32 outer = c->loop_run, run = 0;
        if (s->breaks) {
            run = spv_local(B, c->p_func_bool);
            resume(c);
            store(c, run, c->c_true);
        }
        c->loop_run = run;
        Loop l = loop_begin(c, run);
        emit_tree(c, s->kid[0]);
        loop_end(c, &l);
        c->loop_run = outer;
        if (s->breaks) {   /* still running: the guard ran out, and the program ends, as in the dispatch loop */
            u32 m = if_begin(c, load(c, c->t_bool, run));
            emit_exit(c);
            if_end(c, m);
        } else emit_exit(c);
        break;
    }
    case SN_CONTINUE: break;
    case SN_BREAK: resume(c); store(c, c->loop_run, c->c_false); break;
    case SN_RETURN: emit_exit(c); break;
    }
}

/* ---- whole programs ------------------------------------------------------------------------------ */
enum { MODE_LINEAR, MODE_DISPATCH, MODE_STRUCTURED };

/* One module of the program, in a mode; info, and what the passes share, in base. */
static u32 *build(const Sx *base, int mode, u32 *words, u32 *out_attr_mask, int *uses_gmem) {
    Sx *c = calloc(1, sizeof *c);
    memcpy(c, base, sizeof *c);
    ShaderInfo *info = c->info;
    spv_init(B);
    spv_capability(B, SpvCapabilityShader);
    make_types(c);
    c->main_fn = spv_function_begin(B, c->t_void, spv_type_function(B, c->t_void, 0, NULL));
    label(c, spv_id(B));
    make_interface(c);
    if (mode == MODE_LINEAR) {
        for (u32 i = 0; i < c->n; i++) {
            const ShaderInsn *in = &c->ins[i];
            if (in->op != OP_EXIT) { emit_guarded(c, in); continue; }
            /* EXIT: an unconditional one ends the program; a predicated one returns early */
            u32 cond = tree_guard(c, in);
            if (cond == c->c_false) continue;
            if (cond == c->c_true) { emit_exit(c); continue; }
            u32 merge = if_begin(c, cond);
            emit_exit(c);
            if_end(c, merge);
        }
        if (c->open) emit_exit(c);
    } else if (mode == MODE_DISPATCH) emit_dispatch(c);
    else {
        emit_tree(c, c->cfg->root);
        const SNode *last = c->cfg->root->n ? c->cfg->root->kid[c->cfg->root->n - 1] : NULL;
        if (!last || !(last->kind == SN_RETURN || (last->kind == SN_LOOP && !last->breaks))) emit_exit(c);   /* (ends otherwise) */
    }
    if (c->open) { spv_do(B, SpvOpReturn, 0, NULL); c->open = 0; }   /* (an unreachable block after the last return) */
    spv_function_end(B);
    write_memory_functions(c);
    write_decode_function(c);
    *out_attr_mask |= c->out_attr_mask;
    *uses_gmem |= c->uses_gmem;

    int stage = c->stage;
    SpvExecutionModel model = stage == SHADER_VERTEX ? SpvExecutionModelVertex : stage == SHADER_PIXEL ? SpvExecutionModelFragment : SpvExecutionModelGLCompute;
    spv_entry_point(B, model, c->main_fn, "main", c->niface, c->iface);
    if (stage == SHADER_PIXEL) {
        spv_execution_mode(B, c->main_fn, SpvExecutionModeOriginUpperLeft, 0, NULL);
        if (info->writes_depth) spv_execution_mode(B, c->main_fn, SpvExecutionModeDepthReplacing, 0, NULL);
    } else if (stage == SHADER_COMPUTE) {
        u32 size3[3] = {c->local[0] ? c->local[0] : 1, c->local[1] ? c->local[1] : 1, c->local[2] ? c->local[2] : 1};
        spv_execution_mode(B, c->main_fn, SpvExecutionModeLocalSize, 3, size3);
    }
    u32 *out = spv_finish(B, words);
    spv_free(B);
    free(c);
    return out;
}

u32 *shader_spirv(const u8 *program, u32 size, int stage, u32 vs_outputs, const u32 local[3], u32 shared, ShaderInfo *info,
                  CbufReader *read, void *user, int flags, u32 *words) {
    return shader_spirv_ex(program, size, stage, vs_outputs, local, shared, info, read, user, flags, 0, words);
}

u32 *shader_spirv_ex(const u8 *program, u32 size, int stage, u32 vs_outputs, const u32 local[3], u32 shared, ShaderInfo *info,
                     CbufReader *read, void *user, int flags, u64 vin, u32 *words) {
    shader_decode_init();
    memset(info, 0, sizeof *info);
    info->stage = stage;
    Sx base;
    memset(&base, 0, sizeof base);
    if (local) { base.local[0] = local[0]; base.local[1] = local[1]; base.local[2] = local[2]; }
    base.shared = shared;
    base.read_cbuf = read;
    base.read_user = user;
    base.stage = stage;
    base.prog = program;
    base.info = info;
    base.vs_outputs = vs_outputs;
    base.vin = stage == SHADER_VERTEX ? vin : 0;
    base.ins = shader_decode_program(program, size, &base.n);
    for (u32 i = 0; i < base.n; i++) base.branchy |= shader_is_branchy(base.ins[i].op);
    u32 out_attr_mask = 0, *out, *targets = NULL, ntargets = 0;
    int uses_gmem = 0, reads_cc;
    Cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    if (!base.branchy) {
        info->flow = SHADER_FLOW_LINEAR;
        out = build(&base, MODE_LINEAR, words, &out_attr_mask, &uses_gmem);
    } else {
        /* the dispatch loop first, structured or not, as the HLSL translator does: what it notes in
         * ShaderInfo (textures and their order, attributes) is then the same in both forms */
        base.flow = shader_describe_flow(base.ins, base.n, info, read, user, &targets, &ntargets, &reads_cc);
        cfg_blocks(&cfg, base.flow, base.n);
        base.cfg = &cfg;
        out = build(&base, MODE_DISPATCH, words, &out_attr_mask, &uses_gmem);
        info->flow = SHADER_FLOW_DISPATCH;
        const char *why = (flags & SHADER_DISPATCH) || shader_dispatch_forced() ? "forced"
                        : !cfg_resolve(&cfg) || !cfg_structure(&cfg) ? cfg.reject : NULL;
        if (why) snprintf(info->flow_reason, sizeof info->flow_reason, "%s", why);
        else {
            free(out);
            out = build(&base, MODE_STRUCTURED, words, &out_attr_mask, &uses_gmem);
            info->flow = SHADER_FLOW_STRUCTURED;
        }
    }
    info->outputs = stage == SHADER_VERTEX ? out_attr_mask : 0;
    if (!uses_gmem) info->sb_count = 0;
    cfg_free(&cfg);
    free(base.flow);
    free(targets);
    free(base.ins);
    return out;
}
