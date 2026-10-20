/* The render resolution scale's arithmetic (render-resolution-scale): which render targets are
 * scaled, their sizes, and the shaders' coordinate fix-ups as C (gpu_shader.c emits the same as
 * HLSL; tests/test_gpu_scale.py checks that unscaled they change nothing). */
#include <math.h>

#include "gpu_scale.h"

float scale_for(unsigned width, unsigned height, unsigned sw, unsigned sh, float scale) {
    return width >= sw && height >= sh ? scale : 1.0f;
}

unsigned scaled_size(unsigned logical, float scale) {
    if (scale == 1.0f) return logical;
    unsigned n = (unsigned)(logical * scale + 0.5f);
    return n ? n : 1;
}

void scale_rect(long r[4], float scale) {
    if (scale == 1.0f) return;
    r[0] = (long)floorf(r[0] * scale);
    r[1] = (long)floorf(r[1] * scale);
    r[2] = (long)ceilf(r[2] * scale);
    r[3] = (long)ceilf(r[3] * scale);
}

/* tsxy_: d == 0 ? p : int2(floor((float2(p) + 0.5f) * (1.0f + d))) */
void scale_texel(int p[2], float d) {
    if (d == 0.0f) return;
    p[0] = (int)floorf(((float)p[0] + 0.5f) * (1.0f + d));
    p[1] = (int)floorf(((float)p[1] + 0.5f) * (1.0f + d));
}

/* the window position: i.pos * (1.0f + params.rtscale.z) */
float scale_position(float physical, float inv_d) { return physical * (1.0f + inv_d); }
