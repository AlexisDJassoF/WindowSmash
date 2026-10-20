/* AMD FidelityFX Super Resolution 1 for the present (gpu_fsr.c): EASU (the upscale) and RCAS
 * (the sharpening) as pixel shaders, and their constants. */
#pragma once
#include <stdint.h>

/* HLSL: ps_easu (Texture2D t0, sampler s0, cbuffer b0 = the EASU constants) and ps_rcas
 * (Texture2D t0, cbuffer b0 = the RCAS constants), both after the blit's full-screen triangle
 * (struct V { SV_Position; TEXCOORD0 }). */
extern const char FSR_HLSL[];

/* FsrEasuConOffset's 16 words: the vp_w x vp_h picture at (off_x, off_y) of an in_w x in_h
 * texture, to out_w x out_h. */
void fsr_easu_constants(uint32_t con[16], float vp_w, float vp_h, float in_w, float in_h, float out_w, float out_h, float off_x,
                        float off_y);
/* FsrRcasCon's 4 words for this sharpness (stops: 0.0 the most); words 2 and 3 are then this
 * runtime's own: where RCAS's output starts in the back buffer (x, y). */
void fsr_rcas_constants(uint32_t con[4], float sharpness);
