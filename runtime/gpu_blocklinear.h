/* Maxwell's block-linear layout, on any memory (gpu_texture.c reads the guest's with it, gpu_bntx.c a
 * file's): 64-byte x 8-row GOBs (512 bytes), stacked 2^n high in a block, blocks row after row. */
#pragma once
#include <stdint.h>
#include <string.h>

/* Byte offset of (x bytes, y rows) inside a GOB. */
static inline uint32_t bl_gob_offset(uint32_t x, uint32_t y) {
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

/* The block height (log2 of GOBs) a level of rows block rows really has: halved while it is too tall */
static inline uint32_t bl_level_block_height(uint32_t rows, uint32_t bh) {
    while (bh > 0 && (rows + 7) / 8 <= (1u << (bh - 1))) bh--;
    return bh;
}

/* Block-linear size of one level: width in bytes, height in block rows, block height log2. */
static inline uint64_t bl_level_size(uint32_t wbytes, uint32_t rows, uint32_t bh) {
    return (uint64_t)((wbytes + 63) / 64) * ((rows + (8u << bh) - 1) / (8u << bh)) * (512ull << bh);
}

/* A level of row bytes x rows from src (block height bh, as bl_level_block_height gives it) into linear rows at out */
static inline void bl_read(const uint8_t *src, uint32_t row, uint32_t rows, uint32_t bh, uint8_t *out) {
    uint32_t gobs_x = (row + 63) / 64, block_rows = 8u << bh;
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t by = y / block_rows, yin = y % block_rows;
        uint64_t line = (uint64_t)by * gobs_x * (512ull << bh) + (yin / 8) * 512ull;
        for (uint32_t x = 0; x < row; x += 16) {   /* 16 contiguous bytes within a GOB */
            uint64_t off = line + (uint64_t)(x / 64) * (512ull << bh) + bl_gob_offset(x, yin);
            uint32_t n = row - x < 16 ? row - x : 16;
            memcpy(out + (size_t)y * row + x, src + off, n);
        }
    }
}
