/* The render resolution scale's arithmetic (gpu_scale.c), apart from Direct3D so the tests can run it. */
#pragma once

/* A render target's scale: `scale` for one at least the screen's size (sw x sh) in both axes - the
 * game's scene and its full-screen passes - 1 for a smaller one (shadow maps, bloom levels,
 * lookup tables: their pixels are data the game addresses as they are). */
float scale_for(unsigned width, unsigned height, unsigned sw, unsigned sh, float scale);
/* The texture's size for a logical one: rounded, at least 1. */
unsigned scaled_size(unsigned logical, float scale);
/* A scissor rectangle (left, top, right, bottom) in the texture's pixels: outwards, so that it
 * covers the same logical pixels. */
void scale_rect(long r[4], float scale);
/* The translated shaders' arithmetic (gpu_shader.c), in C: a texel the program asks for at logical
 * (x, y) of a texture scaled by 1 + d (tsxy_), and a window position from the physical one (posx). */
void scale_texel(int p[2], float d);
float scale_position(float physical, float inv_d);
