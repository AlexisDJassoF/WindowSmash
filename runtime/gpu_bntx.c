/* BNTX: see bntx.h. The layout (Switch Toolbox's names): "BNTX" at 0, "NX  " at 0x20 with the
 * texture count at +4 and the offset of its table of BRTI offsets at +8; a BRTI's format at +0x1C
 * (high byte the kind, low byte the type: 1 UNORM, 6 sRGB), width +0x24, height +0x28, its layout
 * +0x34 (the low 3 bits: the block height, log2 of GOBs), and at +0x70 the offset of its table of
 * mip level offsets. Level 0 is block linear (gpu_blocklinear.h). */
#include "gpu_bntx.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_blocklinear.h"
#include "gpu_overlay_quads.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int bntx_read(const void *file, size_t size, BntxImage *img, char *err, size_t n) {
    const uint8_t *d = file;
    memset(img, 0, sizeof *img);
    if (size < 0x30 || memcmp(d, "BNTX", 4) || memcmp(d + 0x20, "NX  ", 4)) { snprintf(err, n, "not a BNTX file"); return 0; }
    uint64_t infos = rd64(d + 0x28);
    if (!rd32(d + 0x24) || infos + 8 > size) { snprintf(err, n, "no texture in it"); return 0; }
    uint64_t b = rd64(d + infos);
    if (b + 0x78 > size || memcmp(d + b, "BRTI", 4)) { snprintf(err, n, "its texture's header is not there"); return 0; }
    uint32_t format = rd32(d + b + 0x1C), w = rd32(d + b + 0x24), h = rd32(d + b + 0x28), bh = rd32(d + b + 0x34) & 7;
    uint64_t mips = rd64(d + b + 0x70);
    if (mips + 8 > size) { snprintf(err, n, "no level table"); return 0; }
    uint64_t at = rd64(d + mips);
    unsigned bytes, block;   /* per block (or pixel), texels across a block */
    switch (format >> 8) {
    case 0x0B: img->format = OVERLAY_TEX_RGBA8, bytes = 4, block = 1; break;   /* R8G8B8A8 */
    case 0x0C: img->format = OVERLAY_TEX_RGBA8, bytes = 4, block = 1; break;   /* B8G8R8A8: swapped below */
    case 0x1A: img->format = OVERLAY_TEX_BC1, bytes = 8, block = 4; break;
    case 0x1C: img->format = OVERLAY_TEX_BC3, bytes = 16, block = 4; break;
    case 0x20: img->format = OVERLAY_TEX_BC7, bytes = 16, block = 4; break;
    default: snprintf(err, n, "format 0x%04x not supported", format); return 0;
    }
    if (!w || !h || w > 8192 || h > 8192) { snprintf(err, n, "size %ux%u", w, h); return 0; }
    uint32_t row = (w + block - 1) / block * bytes, rows = (h + block - 1) / block;
    bh = bl_level_block_height(rows, bh);
    if (at > size || bl_level_size(row, rows, bh) > size - at) { snprintf(err, n, "cut short (%zu bytes)", size); return 0; }
    img->size = (size_t)row * rows;
    if (!(img->data = malloc(img->size))) { snprintf(err, n, "out of memory"); return 0; }
    bl_read(d + at, row, rows, bh, img->data);
    if (format >> 8 == 0x0C)
        for (size_t i = 0; i + 3 < img->size; i += 4) { uint8_t t = img->data[i]; img->data[i] = img->data[i + 2]; img->data[i + 2] = t; }
    img->width = w, img->height = h;
    return 1;
}
