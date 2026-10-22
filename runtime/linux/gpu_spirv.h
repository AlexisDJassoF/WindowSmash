/* Maxwell shader programs straight to SPIR-V, for the Vulkan renderer (gpu_spirv.c) - the same
 * model and the same ShaderInfo as the HLSL translator (gpu_shader.c), without HLSL, DXBC or
 * vkd3d-shader: every general register a 32-bit integer, every predicate a boolean, floating point
 * values the registers' bits reinterpreted, constant buffers read as raw words (any offset; out of
 * range, 0), vertex attributes pulled from the vertex streams and decoded from each Maxwell format.
 *
 * Resources (one layout for every program, so the renderer needs no table per shader):
 *   set 0  binding  0..17   constant buffers 0..17 of a vertex or compute program  (storage buffers of uint, read only)
 *          binding 18..35   constant buffers 0..17 of a pixel program
 *          binding 36..67   vertex streams 0..31                                    (storage buffers of uint, read only)
 *   set 1  binding  0..31   textures 0..31 of a vertex program's texture table      (combined image samplers)
 *          binding 32..63   textures of a pixel program
 *   set 2  binding  0..15   storage buffers (ShaderInfo.sb_offset): read only, but written by compute programs
 *   set 3  binding  0       DrawParams (gpu_shader.h), a uniform buffer with a dynamic offset
 * Out of range reads give 0 by the device's robustBufferAccess2 (the renderer's binding ranges are
 * the buffers' sizes).
 *
 * The interface between the stages: a vertex program's generic attribute n is Location n, the
 * clip-space w the pixel program's FragCoord.w; colour target n of a pixel program is Location n.
 * Clip space is Direct3D's (DrawParams.fix applies the game's conventions, as in HLSL). */
#pragma once
#include "host.h"
#include "gpu_shader.h"

enum {
    SPIRV_SET_BUFFERS = 0, SPIRV_SET_TEXTURES = 1, SPIRV_SET_STORAGE = 2, SPIRV_SET_PARAMS = 3,
    SPIRV_CBUF_BINDING_PIXEL = 18, SPIRV_STREAM_BINDING = 36, SPIRV_TEXTURE_BINDING_PIXEL = 32, SPIRV_STORAGE_BINDING_PIXEL = 16,
};

/* One program to SPIR-V (malloc'd, *words long), its ShaderInfo filled as shader_translate's;
 * the arguments as shader_translate's (gpu_shader.h). A program it cannot translate fills
 * info->unsupported with the first instruction that failed; the module is then still valid, the
 * instruction left out. */
u32 *shader_spirv(const u8 *program, u32 size, int stage, u32 vs_outputs, const u32 local[3], u32 shared, ShaderInfo *info,
                  CbufReader *read, void *user, int flags, u32 *words);
