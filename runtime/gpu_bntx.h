/* A BNTX file's first texture (gpu_bntx.c), its level 0 as linear rows of pixels or blocks - the
 * portraits and icons of ui/replace[_patch]/chara/ (BC7, some BC1, BC3 or RGBA8), for the menu's
 * pictures (gpu_overlay_quads.h's textures). */
#pragma once
#include <stddef.h>

typedef struct BntxImage {
    int format;                /* OVERLAY_TEX_* (RGBA8, BC1, BC3, BC7) */
    unsigned width, height;
    unsigned char *data;       /* malloc'd: rows of 4x4 blocks (BC), or of RGBA pixels */
    size_t size;
} BntxImage;

/* 1 read (img->data to free), 0 not, with why into err */
int bntx_read(const void *file, size_t size, BntxImage *img, char *err, size_t n);
