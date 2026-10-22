/* Maxwell (SM 5.x) shader programs as instructions, for the translators (gpu_shader.c: HLSL for
 * Direct3D 11; linux/gpu_spirv.c: SPIR-V for Vulkan): the opcode of each 64-bit word, its operand
 * fields, branch targets, where a program ends, and what each instruction does to control flow
 * (gpu_shader_cfg.h's CfInsn), indirect branches' jump tables resolved from constant buffers.
 *
 * Instruction set: opcodes from the top 16 bits (most specific pattern wins); operand fields
 * as decoded by the yuzu project's translator. A program is a 0x50-byte header (SPH), then
 * 64-bit instructions in groups of four whose first is scheduling control. */
#pragma once
#include "gpu_shader.h"
#include "gpu_shader_cfg.h"

#define BITS(w, pos, n) ((u32)(((w) >> (pos)) & ((1ull << (n)) - 1)))
#define RZ 255
#define PT 7

/* ---- opcodes ----------------------------------------------------------------------- */
#define OPCODES(X) \
    X(ALD, "1110111111011---") X(AST, "1110111111110---") X(BAR, "1111000010101---") \
    X(BFE_reg, "0101110000000---") X(BFE_cbuf, "0100110000000---") X(BFE_imm, "0011100-00000---") \
    X(BFI_reg, "0101101111110---") X(BFI_rc, "0101001111110---") X(BFI_cr, "0100101111110---") X(BFI_imm, "0011011-11110---") \
    X(BRA, "111000100100----") X(BRK, "111000110100----") X(BRX, "111000100101----") \
    X(DEPBAR, "1111000011110---") X(EXIT, "111000110000----") \
    X(F2F_reg, "0101110010101---") X(F2F_cbuf, "0100110010101---") X(F2F_imm, "0011100-10101---") \
    X(F2I_reg, "0101110010110---") X(F2I_cbuf, "0100110010110---") X(F2I_imm, "0011100-10110---") \
    X(FADD_reg, "0101110001011---") X(FADD_cbuf, "0100110001011---") X(FADD_imm, "0011100-01011---") X(FADD32I, "000010----------") \
    X(FFMA_reg, "010110011-------") X(FFMA_rc, "010100011-------") X(FFMA_cr, "010010011-------") X(FFMA_imm, "0011001-1-------") \
    X(FFMA32I, "000011----------") \
    X(FMNMX_reg, "0101110001100---") X(FMNMX_cbuf, "0100110001100---") X(FMNMX_imm, "0011100-01100---") \
    X(FMUL_reg, "0101110001101---") X(FMUL_cbuf, "0100110001101---") X(FMUL_imm, "0011100-01101---") X(FMUL32I, "00011110--------") \
    X(CSET, "0101000010011---") X(CSETP, "0101000010100---")     X(FSET_reg, "01011000--------") X(FSET_cbuf, "01001000--------") X(FSET_imm, "0011000---------") \
    X(FSETP_reg, "010110111011----") X(FSETP_cbuf, "010010111011----") X(FSETP_imm, "0011011-1011----") \
    X(HADD2_reg, "0101110100010---") X(HADD2_cbuf, "0111101-1-------") X(HADD2_imm, "0111101-0-------") X(HADD2_32I, "0010110---------") \
    X(HFMA2_reg, "0101110100000---") X(HFMA2_rc, "01100---1-------") X(HFMA2_cr, "01110---1-------") X(HFMA2_imm, "01110---0-------") \
    X(HFMA2_32I, "0010100---------") \
    X(HSET2_reg, "0101110100011---") X(HSET2_cbuf, "0111110-1-------") X(HSET2_imm, "0111110-0-------")     X(HSETP2_reg, "0101110100100---") X(HSETP2_cbuf, "0111111-1-------") X(HSETP2_imm, "0111111-0-------")     X(HMUL2_reg, "0101110100001---") X(HMUL2_cbuf, "0111100-1-------") X(HMUL2_imm, "0111100-0-------") X(HMUL2_32I, "0010101---------") \
    X(I2F_reg, "0101110010111---") X(I2F_cbuf, "0100110010111---") X(I2F_imm, "0011100-10111---") \
    X(I2I_reg, "0101110011100---") X(I2I_cbuf, "0100110011100---") X(I2I_imm, "0011100-11100---") \
    X(IADD_reg, "0101110000010---") X(IADD_cbuf, "0100110000010---") X(IADD_imm, "0011100-00010---") X(IADD32I, "0001110---------") \
    X(IMNMX_reg, "0101110000100---") X(IMNMX_cbuf, "0100110000100---") X(IMNMX_imm, "0011100-00100---") \
    X(IPA, "11100000--------") \
    X(ISCADD_reg, "0101110000011---") X(ISCADD_cbuf, "0100110000011---") X(ISCADD_imm, "0011100-00011---") X(ISCADD32I, "000101----------") \
    X(ISET_reg, "010110110101----") X(ISET_cbuf, "010010110101----") X(ISET_imm, "0011011-0101----") \
    X(ISETP_reg, "010110110110----") X(ISETP_cbuf, "010010110110----") X(ISETP_imm, "0011011-0110----") \
    X(KIL, "111000110011----") X(LDC, "1110111110010---") X(LDG, "1110111011010---") X(STG, "1110111011011---") X(LDS, "1110111101001---") X(STS, "1110111101011---") X(LDL, "1110111101000---") X(STL, "1110111101010---") \
    X(LOP_reg, "0101110001000---") X(LOP_cbuf, "0100110001000---") X(LOP_imm, "0011100-01000---") X(LOP32I, "000001----------") \
    X(LOP3_reg, "0101101111100---") X(LOP3_cbuf, "0000001---------") X(LOP3_imm, "001111----------") \
    X(MEMBAR, "1110111110011---") \
    X(MOV_reg, "0101110010011---") X(MOV_cbuf, "0100110010011---") X(MOV_imm, "0011100-10011---") X(MOV32I, "000000010000----") \
    X(MUFU, "0101000010000---") X(NOP, "0101000010110---") X(PBK, "111000101010----") \
    X(PSET, "0101000010001---") X(PSETP, "0101000010010---") \
    X(R2P_reg, "0101110011110---") X(R2P_cbuf, "0100110011110---") X(R2P_imm, "0011100-11110---") \
    X(IADD3_reg, "010111001100----") X(IADD3_cbuf, "010011001100----") X(IADD3_imm, "0011100-1100----") \
    X(TMML, "1101111101011---") \
    X(RRO_reg, "0101110010010---") X(RRO_cbuf, "0100110010010---") X(RRO_imm, "0011100-10010---") \
    X(S2R, "1111000011001---") \
    X(SEL_reg, "0101110010100---") X(SEL_cbuf, "0100110010100---") X(SEL_imm, "0011100-10100---") \
    X(SHL_reg, "0101110001001---") X(SHL_cbuf, "0100110001001---") X(SHL_imm, "0011100-01001---") \
    X(SHR_reg, "0101110000101---") X(SHR_cbuf, "0100110000101---") X(SHR_imm, "0011100-00101---") \
    X(SSY, "111000101001----") X(SYNC, "1111000011111---") \
    X(TEX, "11000-----------") X(TEXS, "1101-00---------") X(TLD4, "110010----------") X(TLD4S, "11011111-0------") \
    X(TLDS, "1101-01---------") X(TXQ, "1101111101001---") \
    X(VOTE, "0101000011011---") X(VOTE_vtg, "0101000011100---") \
    X(XMAD_reg, "0101101100------") X(XMAD_rc, "010100010-------") X(XMAD_cr, "0100111---------") X(XMAD_imm, "0011011-00------")

enum {
    OP_UNKNOWN,
#define ENUM(name, pat) OP_##name,
    OPCODES(ENUM)
#undef ENUM
    OP_COUNT
};

/* The tables are made on first use; a translator calls this before it decodes on several threads. */
void shader_decode_init(void);
u32 shader_op_decode(u64 w);         /* OP_*, OP_UNKNOWN for none */
const char *shader_op_name(u32 op);  /* "FADD_reg", "?" */

typedef struct ShaderInsn { u32 addr; u64 w; u32 op; } ShaderInsn;

/* ---- operands ---------------------------------------------------------------------- */
/* Operand forms. */
enum { FORM_REG, FORM_CBUF, FORM_IMM, FORM_RC, FORM_CR, FORM_32I };
int shader_form_of(u32 op);          /* FORM_*, from the opcode's name */
u32 shader_imm20(u64 w);             /* a 20-bit immediate, sign-extended */
u32 shader_fimm20(u64 w);            /* a 20-bit float immediate: the top bits of a float */

/* ---- control flow ------------------------------------------------------------------- */
i32 shader_branch_offset(u64 w);
/* Every fourth word, from the code's start after the 0x50-byte header, holds scheduling
 * controls for the next three instructions; a branch to one goes to the first of them. */
u32 shader_align_target(u32 t);
u32 shader_branch_target(const ShaderInsn *in);
int shader_is_branchy(u32 op);       /* BRA, BRX, SSY, PBK, SYNC, BRK */

/* A control instruction's flow test (its low 5 bits): it always passes, never does, or tests
 * the condition codes (TEST_CC) - or reads them and never passes (NUM, NaN: TEST_NEVER_CC). The
 * culling tests (CSM/FCSM, fed by VOTE.VTG) never pass, as nothing is culled early. */
enum { SHADER_TEST_ALWAYS, SHADER_TEST_NEVER, SHADER_TEST_CC, SHADER_TEST_NEVER_CC };
int shader_flow_test(u32 test);

/* The instructions up to the end of the program (malloc'd, *n of them): the 'BRA to self'
 * padding, or an unconditional EXIT that no branch target lies beyond. */
ShaderInsn *shader_decode_program(const u8 *program, u32 size, u32 *n);

/* What each instruction does to control flow (malloc'd, n + 1 of them). Indirect branches' jump
 * tables are resolved with read (when given) and go to info's tables; their targets to *targets
 * (malloc'd, *ntargets), which CfInsn.table points into. *reads_cc: some control instruction
 * tests the condition codes. */
CfInsn *shader_describe_flow(const ShaderInsn *ins, u32 n, ShaderInfo *info, CbufReader *read, void *user, u32 **targets, u32 *ntargets,
                             int *reads_cc);
