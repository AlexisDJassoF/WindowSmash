/* runtime/linux/gpu_spirv_builder.c on its own (tests/test_spirv.py): a pixel shader that writes
 * a constant colour through a local variable, with types and constants asked for twice (made
 * once), into the file given; spirv-val then checks it.
 *
 * Usage: spirv_builder_test <out.spv> */
#include <stdio.h>
#include <stdlib.h>

#include "gpu_spirv_builder.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    Spv b;
    spv_init(&b);
    spv_capability(&b, SpvCapabilityShader);
    spv_capability(&b, SpvCapabilityShader);   /* (declared once) */
    uint32_t f32 = spv_type_float(&b, 32), v4 = spv_type_vector(&b, f32, 4);
    if (spv_type_float(&b, 32) != f32 || spv_const_f32(&b, 0.5f) != spv_const_f32(&b, 0.5f)) { puts("not made once"); return 1; }
    uint32_t out_ptr = spv_type_pointer(&b, SpvStorageClassOutput, v4);
    uint32_t color = spv_global(&b, out_ptr, SpvStorageClassOutput);
    spv_decorate(&b, color, SpvDecorationLocation, 1, (uint32_t[]){0});
    uint32_t voidt = spv_type_void(&b), fnt = spv_type_function(&b, voidt, 0, NULL);
    uint32_t main_fn = spv_function_begin(&b, voidt, fnt);
    spv_label(&b, spv_id(&b));
    uint32_t local = spv_local(&b, spv_type_pointer(&b, SpvStorageClassFunction, v4));
    uint32_t value = spv_const_composite(&b, v4, 4, (uint32_t[]){spv_const_f32(&b, 1), spv_const_f32(&b, 0.5f), spv_const_f32(&b, 0), spv_const_f32(&b, 1)});
    SPV_DO(&b, SpvOpStore, local, value);
    uint32_t loaded = SPV_VAL(&b, SpvOpLoad, v4, local);
    uint32_t clamped = SPV_GLSL(&b, v4, 43 /* FClamp */, loaded, spv_const_null(&b, v4), value);
    SPV_DO(&b, SpvOpStore, color, clamped);
    spv_do(&b, SpvOpReturn, 0, NULL);
    spv_function_end(&b);
    spv_entry_point(&b, SpvExecutionModelFragment, main_fn, "main", 1, &color);
    spv_execution_mode(&b, main_fn, SpvExecutionModeOriginUpperLeft, 0, NULL);
    spv_name(&b, color, "color");
    uint32_t n, *words = spv_finish(&b, &n);
    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(words, 4, n, f) != n) return 1;
    fclose(f);
    free(words);
    spv_free(&b);
    return 0;
}
