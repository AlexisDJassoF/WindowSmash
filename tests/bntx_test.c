/* runtime/gpu_bntx.c without the game, for tests/test_mod_arc.py: bntx_test <file.bntx> <out>
 * prints "format=<OVERLAY_TEX_*> width=<w> height=<h> size=<bytes>" (or "error=<why>") and writes the
 * level's linear rows (blocks or pixels) to <out>. */
#include <stdio.h>
#include <stdlib.h>

#include "gpu_bntx.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n > 0 ? (size_t)n : 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    BntxImage img;
    char err[200];
    if (!bntx_read(d, (size_t)n, &img, err, sizeof err)) { printf("error=%s\n", err); return 0; }
    printf("format=%d width=%u height=%u size=%zu\n", img.format, img.width, img.height, img.size);
    FILE *o = fopen(argv[2], "wb");
    fwrite(img.data, 1, img.size, o);
    fclose(o);
    free(img.data);
    free(d);
    return 0;
}
