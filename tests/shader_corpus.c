/* The shader translator over every program of the game: the programs tools/shader_aot.py packs
 * from data.arc (dump/aot/programs.pack), each translated as the renderer would - a vertex program
 * on its own, a pixel program for each output mask of the vertex programs packed with it - both
 * as it comes (structured where it can be) and as a dispatch loop (SHADER_DISPATCH).
 *
 * Usage: shader_corpus <pack> [limit] [--spirv <dir>]
 * Prints a line per translation: "<vs|ps> <program hash> <vs outputs> <s|d> <FNV-1a of the HLSL>
 * <ShaderInfo's hash>" or "... failed <why>", sorted by nothing but the pack's order. Run on two
 * builds of the translator, the lines say whether they generate the same (tests/test_shader_corpus.py).
 *
 * --spirv (built with WITH_SPIRV and runtime/linux/gpu_spirv*.c): every translation also through the
 * SPIR-V translator (with the same flags), a line "spirv <key> same|differs|unsupported <why>": its
 * ShaderInfo against the HLSL translator's (unsupported-by-both: neither can), and the module into
 * <dir>/<key>.spv when it is complete,
 * for spirv-val (tests/test_spirv.py). */
#define _CRT_SECURE_NO_WARNINGS
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader.h"
#ifdef WITH_SPIRV
#include "gpu_spirv.h"
#endif

u32 shader_size(const u8 *program, u32 max);

static jmp_buf on_fatal;
static char fatal_text[256];
_Noreturn void rt_fatal(const char *fmt, ...) {   /* the translator gave up on a program */
    va_list a;
    va_start(a, fmt);
    vsnprintf(fatal_text, sizeof fatal_text, fmt, a);
    va_end(a);
    longjmp(on_fatal, 1);
}

static u32 zero_cbuf(void *user, u32 bank, u32 offset) { (void)user; (void)bank; (void)offset; return 0; }

static const char *spirv_dir;   /* --spirv */

static u64 fnv(const void *data, size_t n, u64 h) {
    const u8 *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

typedef struct Program { int stage; u32 length, npairs, size, outputs; const u32 *pairs; const u8 *bytes; u64 hash; int ok; } Program;

/* One translation's line; the vertex program's outputs into *outputs (for its pixel programs). */
static int translate(Program *p, u32 vs_outputs, int flags, u32 *outputs) {
    ShaderInfo info;
    memset(&info, 0, sizeof info);
    const char *stage = p->stage == SHADER_VERTEX ? "vs" : "ps";
    char *text = NULL;
    fatal_text[0] = 0;
    if (!setjmp(on_fatal)) text = shader_translate(p->bytes, p->size, p->stage, vs_outputs, NULL, 0, &info, zero_cbuf, NULL, flags);
    if (!text) {
        printf("%s %016llx %08x %c failed %s\n", stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's', fatal_text);
        return 0;
    }
    if (outputs) *outputs = info.outputs;
#ifdef WITH_SPIRV
    if (spirv_dir) {
        ShaderInfo si;
        u32 n = 0, *code = NULL;
        fatal_text[0] = 0;
        if (!setjmp(on_fatal)) code = shader_spirv(p->bytes, p->size, p->stage, vs_outputs, NULL, 0, &si, zero_cbuf, NULL, flags, &n);
        if (!code) printf("spirv %s_%016llx_%08x_%c failed %s\n", stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's', fatal_text);
        else if (si.unsupported[0] && info.unsupported[0])   /* (neither translates it: not the SPIR-V translator's gap) */
            printf("spirv %s_%016llx_%08x_%c unsupported-by-both %s\n", stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's', si.unsupported);
        else if (si.unsupported[0]) printf("spirv %s_%016llx_%08x_%c unsupported %s\n", stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's', si.unsupported);
        else {
            int same = !memcmp(&si, &info, sizeof si);
            printf("spirv %s_%016llx_%08x_%c %s\n", stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's', same ? "same" : "differs");
            char path[1024];
            snprintf(path, sizeof path, "%s/%s_%016llx_%08x_%c.spv", spirv_dir, stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's');
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(code, 4, n, f); fclose(f); }
        }
        free(code);
    }
#endif
    printf("%s %016llx %08x %c %016llx %016llx\n", stage, (unsigned long long)p->hash, vs_outputs, flags ? 'd' : 's',
           (unsigned long long)fnv(text, strlen(text), 1469598103934665603ull),
           (unsigned long long)fnv(&info, sizeof info, 1469598103934665603ull));
    free(text);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: shader_corpus <pack> [limit] [--spirv <dir>]\n"); return 2; }
    u32 limit = 0xFFFFFFFFu;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "--spirv") && a + 1 < argc) spirv_dir = argv[++a];
        else limit = (u32)strtoul(argv[a], NULL, 0);
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *pack = malloc((size_t)n);
    if (!pack || fread(pack, 1, (size_t)n, f) != (size_t)n || memcmp(pack, "SAOT", 4)) { fprintf(stderr, "bad pack\n"); return 1; }
    fclose(f);
    u32 total;
    memcpy(&total, pack + 4, 4);
    Program *prog = calloc(total, sizeof *prog);
    const u8 *q = pack + 8;
    for (u32 i = 0; i < total; i++) {
        u32 w[3];
        memcpy(w, q, 12);
        prog[i].stage = w[0] ? SHADER_PIXEL : SHADER_VERTEX;
        prog[i].length = w[1];
        prog[i].npairs = w[2];
        prog[i].pairs = (const u32 *)(q + 12);
        prog[i].bytes = q + 12 + 4ull * w[2];
        q = prog[i].bytes + w[1];
    }
    /* each distinct program once (the same one is often packed many times) */
    u64 *seen = calloc(1u << 18, sizeof *seen);
    u32 *first = calloc(1u << 18, sizeof *first);   /* (the pack index of each one's first copy) */
    u32 done = 0;
    for (int pass = 0; pass < 2; pass++)   /* vertex programs first: their outputs are the pixel programs' inputs */
        for (u32 i = 0; i < total && done < limit; i++) {
            Program *p = &prog[i];
            if ((p->stage == SHADER_PIXEL) != pass || p->length <= 0x50) continue;
            p->size = shader_size(p->bytes, p->length);
            if (p->size <= 0x50) continue;
            p->hash = fnv(p->bytes, p->size, 1469598103934665603ull);
            u64 k = (p->hash * 2 + (u64)pass) | 1;
            u32 s = (u32)(k * 0x9E3779B97F4A7C15ull >> 46) & ((1u << 18) - 1);
            int dup = 0;
            for (; seen[s]; s = (s + 1) & ((1u << 18) - 1))
                if (seen[s] == k) { dup = 1; break; }
            if (dup) {   /* (a copy of a vertex program: the first one's outputs, for its pixel programs) */
                if (pass == 0) { p->ok = prog[first[s]].ok; p->outputs = prog[first[s]].outputs; }
                continue;
            }
            seen[s] = k;
            first[s] = i;
            done++;
            if (pass == 0) {
                p->ok = translate(p, 0, 0, &p->outputs);
                translate(p, 0, SHADER_DISPATCH, NULL);
                continue;
            }
            u32 masks[16], nmasks = 0;
            for (u32 j = 0; j < p->npairs && nmasks < 16; j++) {
                const Program *v = p->pairs[j] < total ? &prog[p->pairs[j]] : NULL;
                if (!v || !v->ok) continue;
                u32 m = 0;
                for (; m < nmasks && masks[m] != v->outputs; m++) {}
                if (m == nmasks) masks[nmasks++] = v->outputs;
            }
            if (!nmasks) masks[nmasks++] = 0xFFFFFFFFu;
            for (u32 m = 0; m < nmasks; m++) {
                translate(p, masks[m], 0, NULL);
                translate(p, masks[m], SHADER_DISPATCH, NULL);
            }
        }
    return 0;
}
