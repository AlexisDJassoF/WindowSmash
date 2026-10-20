/* Offline check of the shader translator: translate every dumped program in a directory
 * (<pipeline stage>_<hash>.bin, as SSBU_DUMP_SHADERS writes them) and compile the HLSL as the
 * renderer does - with the system's D3DCompiler on Windows, with vkd3d-shader on Linux
 * (runtime/linux/d3dcompile_vkd3d.c; tools/linux/build_shadertest.py builds it) - reporting
 * failures. A program with branches is compiled twice: as translated (structured when its graph
 * allows, and as the renderer falls back, a dispatch loop when that does not compile) and as the
 * dispatch loop alone; the report counts how each program's control flow came out, why the
 * dispatch loop stayed, and the DXBC instructions of both forms.
 *
 * Usage: shadertest <dir> [-v] [-d] [-q] [name filter]
 *   -v prints the HLSL of each program as translated (-d: its dispatch-loop form instead);
 *   -q only the summary. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <d3dcompiler.h>

#include "gpu_shader.h"
#include "gpu_shader_cache.h"

_Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

/* The file names in a directory ending in .bin, sorted (malloc'd, *n of them). */
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char **list_programs(const char *dir, int *n) {
    char **names = NULL;
    int cap = 0;
    *n = 0;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return NULL;
    for (struct dirent *e; (e = readdir(d));) {
        const char *name = e->d_name;
        size_t len = strlen(name);
        if (len < 4 || strcmp(name + len - 4, ".bin")) continue;
#endif
        if (*n == cap) names = realloc(names, sizeof *names * (cap = cap * 2 + 256));
        names[(*n)++] = strdup(name);
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    qsort(names, (size_t)*n, sizeof *names, by_name);
    return names;
}

/* DXBC: instructions in its shader chunk (declarations left out), and indexable temporaries declared. */
static int dxbc_count(const unsigned char *p, size_t n, int *indexable) {
    *indexable = 0;
    if (n < 32 || memcmp(p, "DXBC", 4)) return -1;
    u32 chunks;
    memcpy(&chunks, p + 28, 4);
    for (u32 k = 0; k < chunks && 32 + 4 * k + 4 <= n; k++) {
        u32 off, size;
        memcpy(&off, p + 32 + 4 * k, 4);
        if (off + 8 > n || (memcmp(p + off, "SHDR", 4) && memcmp(p + off, "SHEX", 4))) continue;
        memcpy(&size, p + off + 4, 4);
        const u32 *t = (const u32 *)(p + off + 8);
        u32 len = size / 4 < t[1] ? size / 4 : t[1], count = 0;
        for (u32 i = 2; i < len;) {
            u32 op = t[i] & 0x7FF, words = op == 0x35 ? (i + 1 < len ? t[i + 1] : 0) : (t[i] >> 24) & 0x7F;   /* (0x35: custom data) */
            if (!words) break;
            int decl = (op >= 88 && op <= 106) || (op >= 143 && op <= 162) || op == 206;
            if (op == 105) ++*indexable;   /* dcl_indexableTemp */
            if (!decl && op != 0x35) count++;
            i += words;
        }
        return (int)count;
    }
    return -1;
}

typedef struct Compiled { int ok, insns, indexable; char *errors; } Compiled;

static Compiled compile(const char *hlsl, const char *name, int stage) {
    Compiled r = {0, 0, 0, NULL};
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = shader_cache_compile(hlsl, name, stage, &code, &errors);
    r.ok = SUCCEEDED(hr);
    if (r.ok) r.insns = dxbc_count(code->lpVtbl->GetBufferPointer(code), code->lpVtbl->GetBufferSize(code), &r.indexable);
    if (errors) r.errors = strdup((const char *)errors->lpVtbl->GetBufferPointer(errors));
    if (code) code->lpVtbl->Release(code);
    if (errors) errors->lpVtbl->Release(errors);
    return r;
}

static char *translate(const unsigned char *buf, u32 n, int stage, u32 vs_out, const u32 local[3], u32 shared, ShaderInfo *info, int flags) {
    return shader_translate(buf, n, stage, vs_out, stage == SHADER_COMPUTE ? local : NULL, shared, info, NULL, NULL, flags);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: shadertest <dir> [-v] [-d] [-q] [name filter]\n");
        return 2;
    }
    int verbose = 0, show_dispatch = 0, quiet = 0;
    const char *filter = NULL;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-d")) show_dispatch = 1;
        else if (!strcmp(argv[a], "-q")) quiet = 1;
        else filter = argv[a];
    }
    int count;
    char **names = list_programs(argv[1], &count);
    if (!names) { fprintf(stderr, "cannot list %s\n", argv[1]); return 1; }
    int total = 0, failed = 0, unsupported = 0, linear = 0, structured = 0, dispatch = 0, fallback = 0, regressions = 0;
    int indexable_now = 0, indexable_before = 0;
    long long insns_now = 0, insns_before = 0;
    struct { char why[24]; int count; } reasons[32];
    int nreasons = 0;
    for (int i = 0; i < count; i++) {
        const char *name = names[i];
        if (filter && !strstr(name, filter)) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", argv[1], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        static unsigned char buf[0x40000];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        int stage = name[0] == '5' ? SHADER_PIXEL : name[0] == 'C' ? SHADER_COMPUTE : SHADER_VERTEX;
        u32 local[3] = {1, 1, 1}, shared = 0;
        if (stage == SHADER_COMPUTE) {   /* C_<hash>_<x>x<y>x<z>_<shared>.bin */
            const char *u = strchr(name + 2, '_');
            if (u) sscanf(u + 1, "%ux%ux%u_%u", &local[0], &local[1], &local[2], &shared);
        }
        ShaderInfo info, dinfo;
        u32 vs_out = 0;
        if (stage == SHADER_PIXEL) {   /* first learn what it reads */
            free(translate(buf, (u32)n, stage, 0xFFFFFFFFu, local, shared, &info, 0));
            vs_out = info.inputs;
        }
        char *hlsl = translate(buf, (u32)n, stage, vs_out, local, shared, &info, 0);
        total++;
        if (info.unsupported[0]) { unsupported++; if (!quiet) printf("%s: UNSUPPORTED %s\n", name, info.unsupported); }
        Compiled now = compile(hlsl, name, stage), before = now;
        char *dhlsl = NULL;
        if (info.flow != SHADER_FLOW_LINEAR) {
            dhlsl = translate(buf, (u32)n, stage, vs_out, local, shared, &dinfo, SHADER_DISPATCH);
            before = compile(dhlsl, name, stage);
        }
        if (info.flow == SHADER_FLOW_LINEAR) linear++;
        else if (info.flow == SHADER_FLOW_STRUCTURED) structured++;
        else {
            dispatch++;
            int r = 0;
            while (r < nreasons && strcmp(reasons[r].why, info.flow_reason)) r++;
            if (r == nreasons && nreasons < 32) { snprintf(reasons[r].why, sizeof reasons[r].why, "%s", info.flow_reason); reasons[nreasons++].count = 0; }
            if (r < nreasons) reasons[r].count++;
            if (verbose) printf("%s: dispatch loop (%s)\n", name, info.flow_reason);
        }
        if (!now.ok && info.flow == SHADER_FLOW_STRUCTURED && before.ok) {   /* the renderer's fallback */
            fallback++;
            if (!quiet) printf("%s: structured HLSL failed to compile, the dispatch loop instead\n%.2000s\n", name, now.errors ? now.errors : "?");
            free(now.errors);
            now = before;
            now.errors = NULL;
        }
        if (verbose) printf("==== %s\n%s\n", name, show_dispatch && dhlsl ? dhlsl : hlsl);
        if (!now.ok) {
            failed++;
            if (!quiet) printf("%s: COMPILE FAILED\n%.2000s\n", name, now.errors ? now.errors : "?");
        } else {
            insns_now += now.insns;
            indexable_now += now.indexable > 0;
            if (verbose) printf("%s: %d DXBC instructions (%d as a dispatch loop)%s\n", name, now.insns, before.insns,
                                now.indexable ? ", indexable temporaries" : "");
        }
        if (before.ok) { insns_before += before.insns; indexable_before += before.indexable > 0; }
        if (before.ok && !now.ok) regressions++;
        if (before.errors != now.errors) free(before.errors);
        free(now.errors);
        free(hlsl);
        free(dhlsl);
    }
    printf("%d shaders: %d failed to compile, %d with unsupported instructions\n", total, failed, unsupported);
    printf("control flow: %d linear, %d structured, %d dispatch loops", linear, structured, dispatch);
    for (int r = 0; r < nreasons; r++) printf("%s%s %d", r ? ", " : " (", reasons[r].why, reasons[r].count);
    printf("%s; %d structured ones compiled as dispatch loops (fallback)\n", nreasons ? ")" : "", fallback);
    printf("DXBC instructions: %lld as translated, %lld with every branchy program a dispatch loop; "
           "indexable temporaries in %d (%d as dispatch loops); %d that compile only as dispatch loops\n",
           insns_now, insns_before, indexable_now, indexable_before, regressions);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return failed != 0 || regressions != 0;
}
