/* The mods' data.arc (runtime/mod_arc.c) without the game, for tests/test_mod_arc.py:
 *
 *   mod_arc_test <mods dir> <data.arc size> <cache file or -> <out file>
 *
 * reads the mods folder as the runtime does (mod_scan, mod_load_list), builds the archive and
 * writes what the game would see past data.arc into <out file>: "SSBUMODT", u64 data.arc's size,
 * u64 the archive's, the 0x38-byte header, then bytes [size, total). data.arc's bytes come from
 * the test over the pipes: this writes 'R', u64 offset, u64 size to stdout and reads the bytes
 * from stdin; at the end 'E', i32: 1 the archive was made (and "cache=1" on stderr if it came from
 * the cache), 0 nothing to serve.
 *
 *   mod_arc_test --path <out file> <data.arc path>...
 *
 * reads each path from data.arc with mod_arc_read_path (the same pipes) and writes them to <out file>
 * one after the other, each as u64 its size (all ones: not found) and its bytes; 'E', 1 at the end. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_arc.h"
#include "mod_scan.h"

const char *rt_data_dir = ".";

static int arc_read(void *ctx, uint64_t at, void *buf, uint64_t size) {
    (void)ctx;
    unsigned char q[17] = {'R'};
    memcpy(q + 1, &at, 8);
    memcpy(q + 9, &size, 8);
    if (fwrite(q, 1, 17, stdout) != 17 || fflush(stdout)) return 0;
    return fread(buf, 1, size, stdin) == size;
}

static ModArcFile *files;

static int mod_read(void *ctx, int f, uint64_t at, void *buf, uint64_t size) {
    (void)ctx;
    FILE *fp = fopen(files[f].src, "rb");
    if (!fp) return 0;
    int ok = !fseeko(fp, (off_t)at, SEEK_SET) && fread(buf, 1, size, fp) == size;
    fclose(fp);
    return ok;
}

/* TEST_GAME_HOOKS: the game's files and bodies as the runtime gives them (hle_fs.c), from this data.arc */
static void *game_file(const char *path, uint64_t *size) {
    ModArcIO io = {NULL, arc_read, NULL};
    return mod_arc_read_path(&io, path, size);
}
static uint64_t game_body(const char *fighter, int color) {
    char path[96];
    uint64_t size, h = 0;
    snprintf(path, sizeof path, "fighter/%s/model/body/c%02d/model.nusktb", fighter, color);
    unsigned char *d = game_file(path, &size);
    if (!d) return 0;
    h = 1469598103934665603ull;
    for (uint64_t i = 0; i < size; i++) h = (h ^ d[i]) * 1099511628211ull;
    free(d);
    return h;
}

static void end(int status) {
    unsigned char q[5] = {'E'};
    memcpy(q + 1, &status, 4);
    fwrite(q, 1, 5, stdout);
    fflush(stdout);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--path")) {
        ModArcIO io = {NULL, arc_read, NULL};
        FILE *out = fopen(argv[2], "wb");
        if (!out) end(0);
        for (int i = 3; i < argc; i++) {
            uint64_t size;
            void *data = mod_arc_read_path(&io, argv[i], &size);
            uint64_t n = data ? size : ~0ull;
            fwrite(&n, 8, 1, out);
            if (data) fwrite(data, 1, size, out);
            free(data);
        }
        fclose(out);
        mod_arc_read_done();
        end(1);
    }
    if (argc != 5) { fprintf(stderr, "usage: mod_arc_test <mods dir> <data.arc size> <cache|-> <out>\n"); return 2; }
    static ModList l;
    if (getenv("TEST_GAME_HOOKS")) mod_body_hook = game_body, mod_game_file_hook = game_file;
    mod_scan(&l, argv[1]);
    int n = mod_load_list(&l, &files);
    mod_body_hook = NULL, mod_game_file_hook = NULL;
    mod_arc_read_done();
    mod_list_free(&l);
    ModArcIO io = {NULL, arc_read, mod_read};
    ModArc a;
    uint64_t size = strtoull(argv[2], NULL, 10);
    if (!mod_arc_setup(&a, size, &io, files, n, strcmp(argv[3], "-") ? argv[3] : NULL)) end(0);
    fprintf(stderr, "cache=%d\n", mod_arc_from_cache);
    FILE *out = fopen(argv[4], "wb");
    if (!out) end(0);
    fwrite("SSBUMODT", 8, 1, out);
    fwrite(&a.n, 8, 1, out);
    fwrite(&a.total, 8, 1, out);
    fwrite(a.header, 0x38, 1, out);
    static unsigned char buf[1 << 20];
    for (uint64_t at = a.n; at < a.total;) {
        uint64_t k = a.total - at < sizeof buf ? a.total - at : sizeof buf;
        if (!mod_arc_read(&a, at, buf, k, &io)) { fprintf(stderr, "mod_arc_read failed at %#llx\n", (unsigned long long)at); end(0); }
        fwrite(buf, 1, k, out);
        at += k;
    }
    fclose(out);
    mod_arc_free(&a);
    mod_load_free(files, n);
    end(1);
}
