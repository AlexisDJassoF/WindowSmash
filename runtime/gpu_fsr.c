/* AMD FidelityFX Super Resolution 1 (FSR 1.0.2) in the present: EASU, the edge-adaptive spatial
 * upscale, then RCAS, the robust contrast-adaptive sharpening (gpu_d3d11.c gfx_present, with
 * display.ini's upscale = fsr).
 *
 * Ported from AMD's ffx_fsr1.h and ffx_a.h (third_party/fidelityfx-fsr1/, verbatim: the tests
 * compile them to check these constants bit for bit): the non-packed 32-bit versions of
 * FsrEasuF and FsrRcasF, with ffx_a.h's macros spelled out and the 16-bit paths left out, so the
 * same HLSL compiles with fxc (Windows) and vkd3d-shader (Linux). The reciprocals ffx_a.h takes
 * from HLSL's rcp() are 1.0 / x here (its GLSL path's), as vkd3d-shader has no rcp(). EASU is a
 * pixel shader over the blit's full-screen triangle rather than a compute one: the present is a
 * chain of pixel shaders into render targets, and the swap chain's buffer is no UAV.
 *
 * Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
#include <math.h>
#include <string.h>

#include "gpu_fsr.h"

#pragma STDC FP_CONTRACT OFF   /* (each operation rounded as ffx_a.h's C: no fused multiply-adds) */

const char FSR_HLSL[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "cbuffer Fsr : register(b0) { uint4 con0, con1, con2, con3; };\n"
    "Texture2D<float4> src : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float APrxLoRcpF1(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }\n"
    "float APrxMedRcpF1(float a) { float b = asfloat(0x7ef19fffu - asuint(a)); return b * (-b * a + 2.0); }\n"
    "float APrxLoRsqF1(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }\n"
    "float ARcpF1(float a) { return 1.0 / a; }\n"
    "float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }\n"
    "float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }\n"
    "float3 AMin3F3(float3 x, float3 y, float3 z) { return min(x, min(y, z)); }\n"
    "float3 AMax3F3(float3 x, float3 y, float3 z) { return max(x, max(y, z)); }\n"
    /* ---- EASU (ffx_fsr1.h, FSR_EASU_F) */
    "float4 FsrEasuRF(float2 p) { return src.GatherRed(samp, p); }\n"
    "float4 FsrEasuGF(float2 p) { return src.GatherGreen(samp, p); }\n"
    "float4 FsrEasuBF(float2 p) { return src.GatherBlue(samp, p); }\n"
    "void FsrEasuTapF(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {\n"
    "  float2 v;\n"
    "  v.x = (off.x * ( dir.x)) + (off.y * dir.y);\n"
    "  v.y = (off.x * (-dir.y)) + (off.y * dir.x);\n"
    "  v *= len;\n"
    "  float d2 = v.x * v.x + v.y * v.y;\n"
    "  d2 = min(d2, clp);\n"
    "  float wB = (2.0 / 5.0) * d2 + (-1.0);\n"
    "  float wA = lob * d2 + (-1.0);\n"
    "  wB *= wB;\n"
    "  wA *= wA;\n"
    "  wB = (25.0 / 16.0) * wB + (-(25.0 / 16.0 - 1.0));\n"
    "  float w = wB * wA;\n"
    "  aC += c * w; aW += w;\n"
    "}\n"
    "void FsrEasuSetF(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,\n"
    "                 float lA, float lB, float lC, float lD, float lE) {\n"
    "  float w = 0.0;\n"
    "  if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);\n"
    "  if (biT) w =        pp.x  * (1.0 - pp.y);\n"
    "  if (biU) w = (1.0 - pp.x) *        pp.y ;\n"
    "  if (biV) w =        pp.x  *        pp.y ;\n"
    "  float dc = lD - lC;\n"
    "  float cb = lC - lB;\n"
    "  float lenX = max(abs(dc), abs(cb));\n"
    "  lenX = APrxLoRcpF1(lenX);\n"
    "  float dirX = lD - lB;\n"
    "  dir.x += dirX * w;\n"
    "  lenX = saturate(abs(dirX) * lenX);\n"
    "  lenX *= lenX;\n"
    "  len += lenX * w;\n"
    "  float ec = lE - lC;\n"
    "  float ca = lC - lA;\n"
    "  float lenY = max(abs(ec), abs(ca));\n"
    "  lenY = APrxLoRcpF1(lenY);\n"
    "  float dirY = lE - lA;\n"
    "  dir.y += dirY * w;\n"
    "  lenY = saturate(abs(dirY) * lenY);\n"
    "  lenY *= lenY;\n"
    "  len += lenY * w;\n"
    "}\n"
    "void FsrEasuF(out float3 pix, uint2 ip, uint4 con0, uint4 con1, uint4 con2, uint4 con3) {\n"
    "  float2 pp = float2(ip) * asfloat(con0.xy) + asfloat(con0.zw);\n"
    "  float2 fp = floor(pp);\n"
    "  pp -= fp;\n"
    "  float2 p0 = fp * asfloat(con1.xy) + asfloat(con1.zw);\n"
    "  float2 p1 = p0 + asfloat(con2.xy);\n"
    "  float2 p2 = p0 + asfloat(con2.zw);\n"
    "  float2 p3 = p0 + asfloat(con3.xy);\n"
    "  float4 bczzR = FsrEasuRF(p0);\n"
    "  float4 bczzG = FsrEasuGF(p0);\n"
    "  float4 bczzB = FsrEasuBF(p0);\n"
    "  float4 ijfeR = FsrEasuRF(p1);\n"
    "  float4 ijfeG = FsrEasuGF(p1);\n"
    "  float4 ijfeB = FsrEasuBF(p1);\n"
    "  float4 klhgR = FsrEasuRF(p2);\n"
    "  float4 klhgG = FsrEasuGF(p2);\n"
    "  float4 klhgB = FsrEasuBF(p2);\n"
    "  float4 zzonR = FsrEasuRF(p3);\n"
    "  float4 zzonG = FsrEasuGF(p3);\n"
    "  float4 zzonB = FsrEasuBF(p3);\n"
    "  float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);\n"
    "  float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);\n"
    "  float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);\n"
    "  float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);\n"
    "  float bL = bczzL.x;\n"
    "  float cL = bczzL.y;\n"
    "  float iL = ijfeL.x;\n"
    "  float jL = ijfeL.y;\n"
    "  float fL = ijfeL.z;\n"
    "  float eL = ijfeL.w;\n"
    "  float kL = klhgL.x;\n"
    "  float lL = klhgL.y;\n"
    "  float hL = klhgL.z;\n"
    "  float gL = klhgL.w;\n"
    "  float oL = zzonL.z;\n"
    "  float nL = zzonL.w;\n"
    "  float2 dir = 0.0;\n"
    "  float len = 0.0;\n"
    "  FsrEasuSetF(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);\n"
    "  FsrEasuSetF(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);\n"
    "  FsrEasuSetF(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);\n"
    "  FsrEasuSetF(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);\n"
    "  float2 dir2 = dir * dir;\n"
    "  float dirR = dir2.x + dir2.y;\n"
    "  bool zro = dirR < (1.0 / 32768.0);\n"
    "  dirR = APrxLoRsqF1(dirR);\n"
    "  dirR = zro ? 1.0 : dirR;\n"
    "  dir.x = zro ? 1.0 : dir.x;\n"
    "  dir *= dirR;\n"
    "  len = len * 0.5;\n"
    "  len *= len;\n"
    "  float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));\n"
    "  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);\n"
    "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
    "  float clp = APrxLoRcpF1(lob);\n"
    "  float3 min4 = min(AMin3F3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w), float3(ijfeR.y, ijfeG.y, ijfeB.y)),\n"
    "                    float3(klhgR.x, klhgG.x, klhgB.x));\n"
    "  float3 max4 = max(AMax3F3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w), float3(ijfeR.y, ijfeG.y, ijfeB.y)),\n"
    "                    float3(klhgR.x, klhgG.x, klhgB.x));\n"
    "  float3 aC = 0.0;\n"
    "  float aW = 0.0;\n"
    "  FsrEasuTapF(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));\n"   /* b */
    "  FsrEasuTapF(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));\n"   /* c */
    "  FsrEasuTapF(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));\n"   /* i */
    "  FsrEasuTapF(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));\n"   /* j */
    "  FsrEasuTapF(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));\n"   /* f */
    "  FsrEasuTapF(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));\n"   /* e */
    "  FsrEasuTapF(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));\n"   /* k */
    "  FsrEasuTapF(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));\n"   /* l */
    "  FsrEasuTapF(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));\n"   /* h */
    "  FsrEasuTapF(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));\n"   /* g */
    "  FsrEasuTapF(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));\n"   /* o */
    "  FsrEasuTapF(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));\n"   /* n */
    "  pix = min(max4, max(min4, aC * ARcpF1(aW)));\n"
    "}\n"
    "float4 ps_easu(V i) : SV_Target { float3 c; FsrEasuF(c, uint2(i.pos.xy), con0, con1, con2, con3); return float4(c, 1); }\n"
    /* ---- RCAS (ffx_fsr1.h, FSR_RCAS_F; without FSR_RCAS_DENOISE, as AMD's default) */
    "#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))\n"
    "float4 FsrRcasLoadF(int2 p) {   /* (this runtime: clamped to the picture, which starts at con0.zw in the back buffer) */\n"
    "  uint w, h; src.GetDimensions(w, h);\n"
    "  return src.Load(int3(clamp(p, int2(0, 0), int2(w, h) - 1), 0));\n"
    "}\n"
    "void FsrRcasF(out float pixR, out float pixG, out float pixB, uint2 ip, uint4 con) {\n"
    "  int2 sp = int2(ip);\n"
    "  float3 b = FsrRcasLoadF(sp + int2( 0, -1)).rgb;\n"
    "  float3 d = FsrRcasLoadF(sp + int2(-1,  0)).rgb;\n"
    "  float3 e = FsrRcasLoadF(sp).rgb;\n"
    "  float3 f = FsrRcasLoadF(sp + int2( 1,  0)).rgb;\n"
    "  float3 h = FsrRcasLoadF(sp + int2( 0,  1)).rgb;\n"
    "  float bR = b.r; float bG = b.g; float bB = b.b;\n"
    "  float dR = d.r; float dG = d.g; float dB = d.b;\n"
    "  float eR = e.r; float eG = e.g; float eB = e.b;\n"
    "  float fR = f.r; float fG = f.g; float fB = f.b;\n"
    "  float hR = h.r; float hG = h.g; float hB = h.b;\n"
    "  float mn4R = min(AMin3F1(bR, dR, fR), hR);\n"
    "  float mn4G = min(AMin3F1(bG, dG, fG), hG);\n"
    "  float mn4B = min(AMin3F1(bB, dB, fB), hB);\n"
    "  float mx4R = max(AMax3F1(bR, dR, fR), hR);\n"
    "  float mx4G = max(AMax3F1(bG, dG, fG), hG);\n"
    "  float mx4B = max(AMax3F1(bB, dB, fB), hB);\n"
    "  float2 peakC = float2(1.0, -1.0 * 4.0);\n"
    "  float hitMinR = min(mn4R, eR) * ARcpF1(4.0 * mx4R);\n"
    "  float hitMinG = min(mn4G, eG) * ARcpF1(4.0 * mx4G);\n"
    "  float hitMinB = min(mn4B, eB) * ARcpF1(4.0 * mx4B);\n"
    "  float hitMaxR = (peakC.x - max(mx4R, eR)) * ARcpF1(4.0 * mn4R + peakC.y);\n"
    "  float hitMaxG = (peakC.x - max(mx4G, eG)) * ARcpF1(4.0 * mn4G + peakC.y);\n"
    "  float hitMaxB = (peakC.x - max(mx4B, eB)) * ARcpF1(4.0 * mn4B + peakC.y);\n"
    "  float lobeR = max(-hitMinR, hitMaxR);\n"
    "  float lobeG = max(-hitMinG, hitMaxG);\n"
    "  float lobeB = max(-hitMinB, hitMaxB);\n"
    "  float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeR, lobeG, lobeB), 0.0)) * asfloat(con.x);\n"
    "  float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);\n"
    "  pixR = (lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL;\n"
    "  pixG = (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL;\n"
    "  pixB = (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL;\n"
    "}\n"
    "float4 ps_rcas(V i) : SV_Target {\n"
    "  float r, g, b;\n"
    "  FsrRcasF(r, g, b, uint2(i.pos.xy) - con0.zw, con0);\n"
    "  return float4(r, g, b, 1);\n"
    "}\n";

static uint32_t AU1_AF1(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static float ARcpF1(float a) { return 1.0f / a; }

/* ffx_a.h's AU1_AH1_AF1: a float to a half by its base and shift tables (truncating) */
static uint32_t AU1_AH1_AF1(float f) {
    uint32_t u = AU1_AF1(f), i = u >> 23, sign = i >> 8 ? 0x8000 : 0;
    int e = (int)(i & 0xFF) - 127;
    uint32_t base, shift;
    if (e < -24) base = 0, shift = 24;
    else if (e < -14) base = 0x0400u >> (-e - 14), shift = (uint32_t)(-e - 1);
    else if (e <= 15) base = (uint32_t)(e + 15) << 10, shift = 13;
    else if (e < 128) base = 0x7C00, shift = 24;
    else base = 0x7C00, shift = 13;
    return (sign | base) + ((u & 0x7fffff) >> shift);
}

void fsr_easu_constants(uint32_t con[16], float vp_w, float vp_h, float in_w, float in_h, float out_w, float out_h, float off_x,
                        float off_y) {
    uint32_t *con0 = con, *con1 = con + 4, *con2 = con + 8, *con3 = con + 12;
    con0[0] = AU1_AF1(vp_w * ARcpF1(out_w));
    con0[1] = AU1_AF1(vp_h * ARcpF1(out_h));
    con0[2] = AU1_AF1(0.5f * vp_w * ARcpF1(out_w) - 0.5f + off_x);   /* (FsrEasuConOffset's) */
    con0[3] = AU1_AF1(0.5f * vp_h * ARcpF1(out_h) - 0.5f + off_y);
    con1[0] = AU1_AF1(ARcpF1(in_w));
    con1[1] = AU1_AF1(ARcpF1(in_h));
    con1[2] = AU1_AF1(1.0f * ARcpF1(in_w));
    con1[3] = AU1_AF1(-1.0f * ARcpF1(in_h));
    con2[0] = AU1_AF1(-1.0f * ARcpF1(in_w));
    con2[1] = AU1_AF1(2.0f * ARcpF1(in_h));
    con2[2] = AU1_AF1(1.0f * ARcpF1(in_w));
    con2[3] = AU1_AF1(2.0f * ARcpF1(in_h));
    con3[0] = AU1_AF1(0.0f * ARcpF1(in_w));
    con3[1] = AU1_AF1(4.0f * ARcpF1(in_h));
    con3[2] = con3[3] = 0;
}

void fsr_rcas_constants(uint32_t con[4], float sharpness) {
    sharpness = exp2f(-sharpness);
    con[0] = AU1_AF1(sharpness);
    con[1] = AU1_AH1_AF1(sharpness) + (AU1_AH1_AF1(sharpness) << 16);
    con[2] = 0;
    con[3] = 0;
}
