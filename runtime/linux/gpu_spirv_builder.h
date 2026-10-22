/* A SPIR-V module, word by word, for the Maxwell-to-SPIR-V translator (gpu_spirv.c): ids, the
 * module's sections in the order SPIR-V wants them (capabilities, extensions, imports, memory
 * model, entry points, execution modes, names, decorations, types-constants-globals, functions),
 * types and constants made once each, and a function's local variables gathered at the start of
 * its first block, as SPIR-V requires. The module is SPIR-V 1.3 (Vulkan 1.1 and later).
 *
 * Instructions with a result take its type and return its id (spv_val); the others return
 * nothing (spv_do). SPV_VAL / SPV_DO pass the operands as arguments. */
#pragma once
#include <stdint.h>
#include <spirv/unified1/spirv.h>

typedef struct SpvBuf { uint32_t *w; uint32_t n, cap; } SpvBuf;

typedef struct Spv {
    uint32_t bound;                     /* the next id */
    SpvBuf caps, exts, imports, memory, entries, modes, names, decos, globals, functions;
    SpvBuf fn, locals, prologue;        /* the function being written; its local variables; code at its start */
    int in_prologue;
    uint32_t fn_first_block_end;        /* (where in fn its first label ends: the locals go there) */
    struct SpvKey { uint32_t hash, id, at, len; } *keys;   /* types and constants made, by their words */
    uint32_t nkeys, keys_cap;
    SpvBuf key_words;
    uint32_t glsl;                      /* the GLSL.std.450 import, once asked for */
    uint64_t cap_bits[4];               /* the capabilities declared (those under 256) */
} Spv;

void spv_init(Spv *b);
void spv_free(Spv *b);
uint32_t spv_id(Spv *b);

void spv_capability(Spv *b, SpvCapability cap);
void spv_extension(Spv *b, const char *name);
uint32_t spv_glsl(Spv *b);
void spv_entry_point(Spv *b, SpvExecutionModel model, uint32_t fn, const char *name, uint32_t n, const uint32_t *iface);
void spv_execution_mode(Spv *b, uint32_t fn, SpvExecutionMode mode, uint32_t n, const uint32_t *args);
void spv_name(Spv *b, uint32_t id, const char *name);
void spv_decorate(Spv *b, uint32_t id, SpvDecoration d, uint32_t n, const uint32_t *args);
void spv_member_decorate(Spv *b, uint32_t type, uint32_t member, SpvDecoration d, uint32_t n, const uint32_t *args);

/* ---- types (each made once) ------------------------------------------------------------- */
uint32_t spv_type_void(Spv *b);
uint32_t spv_type_bool(Spv *b);
uint32_t spv_type_int(Spv *b, uint32_t width, int is_signed);
uint32_t spv_type_float(Spv *b, uint32_t width);
uint32_t spv_type_vector(Spv *b, uint32_t component, uint32_t count);
uint32_t spv_type_pointer(Spv *b, SpvStorageClass storage, uint32_t type);
uint32_t spv_type_function(Spv *b, uint32_t ret, uint32_t n, const uint32_t *params);
uint32_t spv_type_array(Spv *b, uint32_t element, uint32_t length);          /* (length: a number; its constant is made) */
uint32_t spv_type_runtime_array(Spv *b, uint32_t element);
uint32_t spv_type_image(Spv *b, uint32_t sampled_type, SpvDim dim, uint32_t depth, uint32_t arrayed, uint32_t ms, uint32_t sampled,
                        SpvImageFormat format);
uint32_t spv_type_sampled_image(Spv *b, uint32_t image);
uint32_t spv_type_sampler(Spv *b);
/* A struct: a new one each time (its members are decorated on it). */
uint32_t spv_type_struct(Spv *b, uint32_t n, const uint32_t *members);

/* ---- constants (each made once) ------------------------------------------------------------- */
uint32_t spv_const_u32(Spv *b, uint32_t v);
uint32_t spv_const_i32(Spv *b, int32_t v);
uint32_t spv_const_f32(Spv *b, float v);
uint32_t spv_const_f32_bits(Spv *b, uint32_t bits);
uint32_t spv_const_bool(Spv *b, int v);
uint32_t spv_const_composite(Spv *b, uint32_t type, uint32_t n, const uint32_t *parts);
uint32_t spv_const_null(Spv *b, uint32_t type);

/* ---- variables -------------------------------------------------------------------------- */
uint32_t spv_global(Spv *b, uint32_t pointer_type, SpvStorageClass storage);
uint32_t spv_local(Spv *b, uint32_t pointer_type);   /* in the function being written */
/* The same with an initial value (a constant): (global) init 0 for none. */
uint32_t spv_global_init(Spv *b, uint32_t pointer_type, SpvStorageClass storage, uint32_t init);
uint32_t spv_local_init(Spv *b, uint32_t pointer_type, uint32_t init);

/* ---- functions and code ------------------------------------------------------------------ */
uint32_t spv_function_begin(Spv *b, uint32_t ret, uint32_t fn_type);   /* returns the function's id */
/* The same with an id already given out (a function called before it is written); then its
 * parameters, in order, before its first label. */
uint32_t spv_function_begin_id(Spv *b, uint32_t id, uint32_t ret, uint32_t fn_type);
uint32_t spv_function_param(Spv *b, uint32_t type);
void spv_function_end(Spv *b);
void spv_label(Spv *b, uint32_t id);                                   /* a block starts */
/* Code for the start of the function's first block, after its variables, written from anywhere
 * (values every later block may use: they dominate it). Straight-line code only. */
void spv_prologue_enter(Spv *b);
void spv_prologue_leave(Spv *b);
uint32_t spv_val(Spv *b, SpvOp op, uint32_t type, uint32_t n, const uint32_t *operands);
void spv_do(Spv *b, SpvOp op, uint32_t n, const uint32_t *operands);
#define SPV_ARGS(...) (uint32_t)(sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)), (uint32_t[]){__VA_ARGS__}
#define SPV_VAL(b, op, type, ...) spv_val(b, op, type, SPV_ARGS(__VA_ARGS__))
#define SPV_DO(b, op, ...) spv_do(b, op, SPV_ARGS(__VA_ARGS__))
/* GLSL.std.450's instruction inst on the operands. */
uint32_t spv_glsl_op(Spv *b, uint32_t type, uint32_t inst, uint32_t n, const uint32_t *operands);
#define SPV_GLSL(b, type, inst, ...) spv_glsl_op(b, type, inst, SPV_ARGS(__VA_ARGS__))

/* The whole module (malloc'd), *words long. */
uint32_t *spv_finish(Spv *b, uint32_t *words);
