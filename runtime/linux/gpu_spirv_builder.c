/* A SPIR-V module, word by word (gpu_spirv_builder.h). */
#include <stdlib.h>
#include <string.h>

#include "gpu_spirv_builder.h"

static void put(SpvBuf *s, uint32_t w) {
    if (s->n == s->cap) s->w = realloc(s->w, sizeof *s->w * (s->cap = s->cap ? s->cap * 2 : 256));
    s->w[s->n++] = w;
}

static void put_all(SpvBuf *s, uint32_t n, const uint32_t *w) {
    for (uint32_t i = 0; i < n; i++) put(s, w[i]);
}

/* An instruction: its opcode and word count, then the words. */
static void inst(SpvBuf *s, SpvOp op, uint32_t n, const uint32_t *w) {
    put(s, (n + 1) << 16 | op);
    put_all(s, n, w);
}

/* A string's words: its bytes, nul-terminated, padded to a word. */
static uint32_t string_words(const char *text, uint32_t *out) {
    uint32_t len = (uint32_t)strlen(text) + 1, n = (len + 3) / 4;
    memset(out, 0, 4 * n);
    memcpy(out, text, len - 1);
    return n;
}

void spv_init(Spv *b) {
    memset(b, 0, sizeof *b);
    b->bound = 1;
}

void spv_free(Spv *b) {
    SpvBuf *bufs[] = {&b->caps, &b->exts, &b->imports, &b->memory, &b->entries, &b->modes, &b->names, &b->decos, &b->globals,
                      &b->functions, &b->fn, &b->locals, &b->prologue, &b->key_words};
    for (unsigned i = 0; i < sizeof bufs / sizeof *bufs; i++) free(bufs[i]->w);
    free(b->keys);
    memset(b, 0, sizeof *b);
}

uint32_t spv_id(Spv *b) { return b->bound++; }

void spv_capability(Spv *b, SpvCapability cap) {
    if (cap < 256) {
        if (b->cap_bits[cap / 64] >> (cap % 64) & 1) return;
        b->cap_bits[cap / 64] |= 1ull << (cap % 64);
    } else {
        for (uint32_t i = 0; i < b->caps.n; i += 2)
            if (b->caps.w[i + 1] == (uint32_t)cap) return;
    }
    uint32_t w = cap;
    inst(&b->caps, SpvOpCapability, 1, &w);
}

void spv_extension(Spv *b, const char *name) {
    uint32_t w[64], n = string_words(name, w);
    inst(&b->exts, SpvOpExtension, n, w);
}

uint32_t spv_glsl(Spv *b) {
    if (!b->glsl) {
        uint32_t w[8];
        w[0] = b->glsl = spv_id(b);
        uint32_t n = string_words("GLSL.std.450", w + 1);
        inst(&b->imports, SpvOpExtInstImport, n + 1, w);
    }
    return b->glsl;
}

void spv_entry_point(Spv *b, SpvExecutionModel model, uint32_t fn, const char *name, uint32_t n, const uint32_t *iface) {
    uint32_t w[256], k = 0;
    w[k++] = model;
    w[k++] = fn;
    k += string_words(name, w + k);
    for (uint32_t i = 0; i < n && k < 256; i++) w[k++] = iface[i];
    inst(&b->entries, SpvOpEntryPoint, k, w);
}

void spv_execution_mode(Spv *b, uint32_t fn, SpvExecutionMode mode, uint32_t n, const uint32_t *args) {
    uint32_t w[16] = {fn, mode};
    for (uint32_t i = 0; i < n && i < 14; i++) w[2 + i] = args[i];
    inst(&b->modes, SpvOpExecutionMode, 2 + n, w);
}

void spv_name(Spv *b, uint32_t id, const char *name) {
    uint32_t w[64];
    w[0] = id;
    uint32_t n = string_words(name, w + 1);
    inst(&b->names, SpvOpName, n + 1, w);
}

void spv_decorate(Spv *b, uint32_t id, SpvDecoration d, uint32_t n, const uint32_t *args) {
    uint32_t w[8] = {id, d};
    for (uint32_t i = 0; i < n && i < 6; i++) w[2 + i] = args[i];
    inst(&b->decos, SpvOpDecorate, 2 + n, w);
}

void spv_member_decorate(Spv *b, uint32_t type, uint32_t member, SpvDecoration d, uint32_t n, const uint32_t *args) {
    uint32_t w[8] = {type, member, d};
    for (uint32_t i = 0; i < n && i < 5; i++) w[3 + i] = args[i];
    inst(&b->decos, SpvOpMemberDecorate, 3 + n, w);
}

/* ---- types and constants, each made once -------------------------------------------------- */
/* The id of the global instruction op with these words around its result id (the result goes at
 * result_at among them; result_at -1: a type, the result first): made the first time. */
static uint32_t once(Spv *b, SpvOp op, int result_at, uint32_t n, const uint32_t *w) {
    uint32_t h = 2166136261u ^ op;
    for (uint32_t i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
    if (b->nkeys * 2 >= b->keys_cap) {   /* grow, and place them again */
        uint32_t cap = b->keys_cap ? b->keys_cap * 2 : 1024;
        struct SpvKey *keys = calloc(cap, sizeof *keys);
        for (uint32_t i = 0; i < b->keys_cap; i++) {
            if (!b->keys[i].id) continue;
            uint32_t s = b->keys[i].hash & (cap - 1);
            while (keys[s].id) s = (s + 1) & (cap - 1);
            keys[s] = b->keys[i];
        }
        free(b->keys);
        b->keys = keys;
        b->keys_cap = cap;
    }
    uint32_t s = h & (b->keys_cap - 1);
    for (; b->keys[s].id; s = (s + 1) & (b->keys_cap - 1)) {
        const struct SpvKey *k = &b->keys[s];
        if (k->hash == h && k->len == n + 1 && b->key_words.w[k->at] == (uint32_t)op && !memcmp(b->key_words.w + k->at + 1, w, 4 * n))
            return k->id;
    }
    uint32_t id = spv_id(b), all[64], k = 0;
    for (uint32_t i = 0; i <= n && k < 64; i++) {
        if ((int)i == (result_at < 0 ? 0 : result_at)) all[k++] = id;
        if (i < n) all[k++] = w[i];
    }
    inst(&b->globals, op, k, all);
    b->keys[s] = (struct SpvKey){h, id, b->key_words.n, n + 1};
    put(&b->key_words, op);
    put_all(&b->key_words, n, w);
    b->nkeys++;
    return id;
}

uint32_t spv_type_void(Spv *b) { return once(b, SpvOpTypeVoid, -1, 0, NULL); }
uint32_t spv_type_bool(Spv *b) { return once(b, SpvOpTypeBool, -1, 0, NULL); }
uint32_t spv_type_int(Spv *b, uint32_t width, int is_signed) {
    uint32_t w[2] = {width, is_signed ? 1u : 0u};
    return once(b, SpvOpTypeInt, -1, 2, w);
}
uint32_t spv_type_float(Spv *b, uint32_t width) { return once(b, SpvOpTypeFloat, -1, 1, &width); }
uint32_t spv_type_vector(Spv *b, uint32_t component, uint32_t count) {
    uint32_t w[2] = {component, count};
    return once(b, SpvOpTypeVector, -1, 2, w);
}
uint32_t spv_type_pointer(Spv *b, SpvStorageClass storage, uint32_t type) {
    uint32_t w[2] = {storage, type};
    return once(b, SpvOpTypePointer, -1, 2, w);
}
uint32_t spv_type_function(Spv *b, uint32_t ret, uint32_t n, const uint32_t *params) {
    uint32_t w[16] = {ret};
    for (uint32_t i = 0; i < n && i < 15; i++) w[1 + i] = params[i];
    return once(b, SpvOpTypeFunction, -1, 1 + n, w);
}
uint32_t spv_type_array(Spv *b, uint32_t element, uint32_t length) {
    uint32_t w[2] = {element, spv_const_u32(b, length)};
    return once(b, SpvOpTypeArray, -1, 2, w);
}
uint32_t spv_type_runtime_array(Spv *b, uint32_t element) { return once(b, SpvOpTypeRuntimeArray, -1, 1, &element); }
uint32_t spv_type_image(Spv *b, uint32_t sampled_type, SpvDim dim, uint32_t depth, uint32_t arrayed, uint32_t ms, uint32_t sampled,
                        SpvImageFormat format) {
    uint32_t w[7] = {sampled_type, dim, depth, arrayed, ms, sampled, format};
    return once(b, SpvOpTypeImage, -1, 7, w);
}
uint32_t spv_type_sampled_image(Spv *b, uint32_t image) { return once(b, SpvOpTypeSampledImage, -1, 1, &image); }
uint32_t spv_type_sampler(Spv *b) { return once(b, SpvOpTypeSampler, -1, 0, NULL); }

uint32_t spv_type_struct(Spv *b, uint32_t n, const uint32_t *members) {
    uint32_t w[64], id = spv_id(b);
    w[0] = id;
    for (uint32_t i = 0; i < n && i < 63; i++) w[1 + i] = members[i];
    inst(&b->globals, SpvOpTypeStruct, 1 + n, w);
    return id;
}

/* (a constant's words: its type, the result, its value) */
uint32_t spv_const_u32(Spv *b, uint32_t v) {
    uint32_t w[2] = {spv_type_int(b, 32, 0), v};
    return once(b, SpvOpConstant, 1, 2, w);
}
uint32_t spv_const_i32(Spv *b, int32_t v) {
    uint32_t w[2] = {spv_type_int(b, 32, 1), (uint32_t)v};
    return once(b, SpvOpConstant, 1, 2, w);
}
uint32_t spv_const_f32_bits(Spv *b, uint32_t bits) {
    uint32_t w[2] = {spv_type_float(b, 32), bits};
    return once(b, SpvOpConstant, 1, 2, w);
}
uint32_t spv_const_f32(Spv *b, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    return spv_const_f32_bits(b, bits);
}
uint32_t spv_const_bool(Spv *b, int v) {
    uint32_t t = spv_type_bool(b);
    return once(b, v ? SpvOpConstantTrue : SpvOpConstantFalse, 1, 1, &t);
}
uint32_t spv_const_composite(Spv *b, uint32_t type, uint32_t n, const uint32_t *parts) {
    uint32_t w[32] = {type};
    for (uint32_t i = 0; i < n && i < 31; i++) w[1 + i] = parts[i];
    return once(b, SpvOpConstantComposite, 1, 1 + n, w);
}
uint32_t spv_const_null(Spv *b, uint32_t type) { return once(b, SpvOpConstantNull, 1, 1, &type); }

/* ---- variables -------------------------------------------------------------------------- */
uint32_t spv_global(Spv *b, uint32_t pointer_type, SpvStorageClass storage) { return spv_global_init(b, pointer_type, storage, 0); }

uint32_t spv_local(Spv *b, uint32_t pointer_type) { return spv_local_init(b, pointer_type, 0); }

uint32_t spv_global_init(Spv *b, uint32_t pointer_type, SpvStorageClass storage, uint32_t init) {
    uint32_t id = spv_id(b), w[4] = {pointer_type, id, storage, init};
    inst(&b->globals, SpvOpVariable, init ? 4 : 3, w);
    return id;
}

uint32_t spv_local_init(Spv *b, uint32_t pointer_type, uint32_t init) {
    uint32_t id = spv_id(b), w[4] = {pointer_type, id, SpvStorageClassFunction, init};
    inst(&b->locals, SpvOpVariable, init ? 4 : 3, w);
    return id;
}

/* ---- functions and code ------------------------------------------------------------------ */
uint32_t spv_function_begin(Spv *b, uint32_t ret, uint32_t fn_type) { return spv_function_begin_id(b, spv_id(b), ret, fn_type); }

uint32_t spv_function_param(Spv *b, uint32_t type) {
    uint32_t id = spv_id(b), w[2] = {type, id};
    inst(&b->fn, SpvOpFunctionParameter, 2, w);
    return id;
}

uint32_t spv_function_begin_id(Spv *b, uint32_t id, uint32_t ret, uint32_t fn_type) {
    uint32_t w[4] = {ret, id, SpvFunctionControlMaskNone, fn_type};
    b->fn.n = b->locals.n = b->prologue.n = 0;
    b->fn_first_block_end = 0;
    inst(&b->fn, SpvOpFunction, 4, w);
    return id;
}

void spv_label(Spv *b, uint32_t id) {
    inst(&b->fn, SpvOpLabel, 1, &id);
    if (!b->fn_first_block_end) b->fn_first_block_end = b->fn.n;
}

static void swap(SpvBuf *a, SpvBuf *c) { SpvBuf t = *a; *a = *c; *c = t; }
void spv_prologue_enter(Spv *b) { if (!b->in_prologue++) swap(&b->fn, &b->prologue); }
void spv_prologue_leave(Spv *b) { if (!--b->in_prologue) swap(&b->fn, &b->prologue); }

void spv_function_end(Spv *b) {
    inst(&b->fn, SpvOpFunctionEnd, 0, NULL);
    uint32_t at = b->fn_first_block_end;
    put_all(&b->functions, at, b->fn.w);
    put_all(&b->functions, b->locals.n, b->locals.w);
    put_all(&b->functions, b->prologue.n, b->prologue.w);
    put_all(&b->functions, b->fn.n - at, b->fn.w + at);
}

uint32_t spv_val(Spv *b, SpvOp op, uint32_t type, uint32_t n, const uint32_t *operands) {
    uint32_t id = spv_id(b);
    put(&b->fn, (n + 3) << 16 | op);
    put(&b->fn, type);
    put(&b->fn, id);
    put_all(&b->fn, n, operands);
    return id;
}

void spv_do(Spv *b, SpvOp op, uint32_t n, const uint32_t *operands) { inst(&b->fn, op, n, operands); }

uint32_t spv_glsl_op(Spv *b, uint32_t type, uint32_t which, uint32_t n, const uint32_t *operands) {
    uint32_t w[16] = {spv_glsl(b), which};
    for (uint32_t i = 0; i < n && i < 14; i++) w[2 + i] = operands[i];
    return spv_val(b, SpvOpExtInst, type, 2 + n, w);
}

uint32_t *spv_finish(Spv *b, uint32_t *words) {
    if (!b->memory.n) {
        uint32_t w[2] = {SpvAddressingModelLogical, SpvMemoryModelGLSL450};
        inst(&b->memory, SpvOpMemoryModel, 2, w);
    }
    const SpvBuf *parts[] = {&b->caps, &b->exts, &b->imports, &b->memory, &b->entries, &b->modes, &b->names, &b->decos, &b->globals,
                             &b->functions};
    uint32_t n = 5;
    for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++) n += parts[i]->n;
    uint32_t *out = malloc(4 * n), k = 0;
    out[k++] = SpvMagicNumber;
    out[k++] = 0x00010300;   /* SPIR-V 1.3 */
    out[k++] = 0;            /* generator */
    out[k++] = b->bound;
    out[k++] = 0;
    for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++) {
        if (parts[i]->n) memcpy(out + k, parts[i]->w, 4 * parts[i]->n);
        k += parts[i]->n;
    }
    *words = n;
    return out;
}
