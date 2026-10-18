/* Ahead-of-time shader compiler: the game's programs found in data.arc (tools/shader_aot.py
 * packs them) into the renderer's disk cache, translated and compiled exactly as the renderer
 * would, so that a program's first use loads its DXBC instead of compiling it.
 *
 * Usage: shaderaot <pack> <cache dir> <part> <parts> [--dry]
 *   Every process translates all vertex programs (their outputs are the paired pixel programs'
 *   inputs) and compiles the records whose index is <part> modulo <parts>. --dry translates only
 *   and prints the cache key of each program ("vs <hash>", "ps <hash> <vs outputs>").
 *
 * Pack: "SAOT", u32 count; then per program u32 stage (0 vertex, 1 pixel), u32 length, u32 pairs,
 * u32 pair[pairs] (the vertex programs found before it in its containers), then its bytes
 * (the header, its code, and possibly bytes after its end).
 *
 * Files are written dated 2001-01-01 (gpu_shader_cache.h): the start's prewarm leaves them out
 * until the game has used them. Programs with indirect branches are left out: their key depends
 * on the constant buffers of a draw. Build: python tools/shader_aot.py builds it. */
#define _CRT_SECURE_NO_WARNINGS
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../runtime/gpu_shader_cache.h"

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

typedef struct Program { int stage; u32 length, npairs; const u32 *pairs; const u8 *bytes; u32 size; u64 hash; int ok; u32 outputs, ntables; } Program;

static char *translate(Program *p, u32 vs_outputs, ShaderInfo *info) {
    if (setjmp(on_fatal)) return NULL;
    return shader_hlsl_ex(p->bytes, p->size, p->stage, vs_outputs, info, zero_cbuf, NULL);
}

static int exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

static struct { u32 compiled, existing, tables, failed, translate_failed, keys; } count;

/* Compile and write one cache file, dated as not yet used. */
static void compile_to(const char *hlsl, const char *name, int stage, const char *path) {
    ID3DBlob *code, *errors;
    HRESULT hr = shader_cache_compile(hlsl, name, stage, &code, &errors);
    if (FAILED(hr)) {
        count.failed++;
        fprintf(stderr, "%s: fxc failed: %.300s\n", name, errors ? (const char *)errors->lpVtbl->GetBufferPointer(errors) : "?");
    } else {
        char tmp[1300];
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, GetCurrentProcessId());
        FILE *f = fopen(tmp, "wb");
        if (f) {
            fwrite(code->lpVtbl->GetBufferPointer(code), 1, code->lpVtbl->GetBufferSize(code), f);
            fclose(f);
            HANDLE h = CreateFileA(tmp, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                FILETIME t = {(DWORD)SHADER_CACHE_UNPLAYED, (DWORD)(SHADER_CACHE_UNPLAYED >> 32)};
                SetFileTime(h, NULL, NULL, &t);
                CloseHandle(h);
            }
            if (!MoveFileExA(tmp, path, 0)) DeleteFileA(tmp);   /* (another process wrote it first) */
            count.compiled++;
        }
    }
    if (code) code->lpVtbl->Release(code);
    if (errors) errors->lpVtbl->Release(errors);
}

/* Keys this process has seen: the same program is often packed more than once. */
static u64 *seen;
static u32 seen_cap = 1u << 17;
static int first_time(u64 key) {
    u64 k = key | 1;
    for (u32 i = (u32)(k * 0x9E3779B97F4A7C15ull >> 47) & (seen_cap - 1);; i = (i + 1) & (seen_cap - 1)) {
        if (seen[i] == k) return 0;
        if (!seen[i]) { seen[i] = k; return 1; }
    }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: shaderaot <pack> <cache dir> <part> <parts> [--dry]\n"); return 2; }
    const char *dir = argv[2];
    u32 part = (u32)atoi(argv[3]), parts = (u32)atoi(argv[4]);
    int dry = argc > 5 && !strcmp(argv[5], "--dry");
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
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
    seen = calloc(seen_cap, sizeof *seen);
    ShaderInfo info;
    char path[1200], name[64];
    /* every vertex program: its key and outputs; compiled by its part */
    for (u32 i = 0; i < total; i++) {
        Program *p = &prog[i];
        if (p->stage != SHADER_VERTEX || p->length <= 0x50) continue;
        if (setjmp(on_fatal)) { count.translate_failed++; continue; }
        p->size = shader_size(p->bytes, p->length);
        if (p->size <= 0x50) continue;
        p->hash = shader_program_hash(p->bytes, p->size);
        char *hlsl = translate(p, 0, &info);
        if (!hlsl) { count.translate_failed++; continue; }
        p->ok = 1;
        p->outputs = info.outputs;
        p->ntables = info.ntables;
        if (i % parts == part && first_time(p->hash * 3)) {
            count.keys++;
            snprintf(name, sizeof name, "vs_%016llx", (unsigned long long)p->hash);
            shader_cache_file(path, sizeof path, dir, SHADER_VERTEX, p->hash, 0, 0);
            if (dry) printf("vs %016llx\n", (unsigned long long)p->hash);
            else if (info.ntables) count.tables++;
            else if (exists(path)) count.existing++;
            else compile_to(hlsl, name, SHADER_VERTEX, path);
        }
        free(hlsl);
    }
    /* the pixel programs of this part, once per distinct output mask of their vertex programs */
    for (u32 i = part; i < total; i += parts) {
        Program *p = &prog[i];
        if (p->stage != SHADER_PIXEL || p->length <= 0x50) continue;
        if (setjmp(on_fatal)) { count.translate_failed++; continue; }
        p->size = shader_size(p->bytes, p->length);
        if (p->size <= 0x50) continue;
        p->hash = shader_program_hash(p->bytes, p->size);
        u32 masks[16], nmasks = 0;
        for (u32 k = 0; k < p->npairs && nmasks < 16; k++) {
            const Program *v = p->pairs[k] < total ? &prog[p->pairs[k]] : NULL;
            if (!v || !v->ok) continue;
            u32 m = 0;
            for (; m < nmasks && masks[m] != v->outputs; m++) {}
            if (m == nmasks) masks[nmasks++] = v->outputs;
        }
        if (!nmasks) {   /* no vertex program known: what it reads */
            char *probe = translate(p, 0xFFFFFFFFu, &info);
            if (!probe) { count.translate_failed++; continue; }
            free(probe);
            masks[nmasks++] = info.inputs;
        }
        for (u32 m = 0; m < nmasks; m++) {
            if (!first_time(p->hash * 5 ^ masks[m])) continue;
            count.keys++;
            snprintf(name, sizeof name, "ps_%016llx", (unsigned long long)p->hash);
            shader_cache_file(path, sizeof path, dir, SHADER_PIXEL, p->hash, masks[m], 0);
            if (dry) { printf("ps %016llx %08x\n", (unsigned long long)p->hash, masks[m]); continue; }
            if (exists(path)) { count.existing++; continue; }
            char *hlsl = translate(p, masks[m], &info);
            if (!hlsl) { count.translate_failed++; continue; }
            if (info.ntables) count.tables++;
            else compile_to(hlsl, name, SHADER_PIXEL, path);
            free(hlsl);
        }
    }
    fprintf(stderr, "part %u/%u: %u keys; %u compiled, %u already cached, %u with jump tables (left out), %u fxc failures, %u not translated\n",
            part, parts, count.keys, count.compiled, count.existing, count.tables, count.failed, count.translate_failed);
    return 0;
}
