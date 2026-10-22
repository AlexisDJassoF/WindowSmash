/* The Vulkan renderer's built-in passes (the overlay, the picture's copy into the window, FSR):
 * their HLSL compiled to SPIR-V by vkd3d-shader when the renderer starts. The game's own shaders
 * do not come here: they go straight from Maxwell to SPIR-V.
 *
 * The registers become Vulkan's thus (gpu_vk.h): cbuffer b0 the push constants, Texture t<i>
 * set 0 binding 2i (a sampled image), SamplerState s<i> set 0 binding 2i + 1. */
#include <stdio.h>
#include <string.h>

#include "gpu_vk.h"
#include <vkd3d_shader.h>

#define MAX_TEXTURES 4

VkShaderModule vk_hlsl(const char *src, const char *entry, const char *profile, unsigned push_size, const char *what) {
    struct vkd3d_shader_resource_binding bindings[2 * MAX_TEXTURES];
    for (unsigned i = 0; i < MAX_TEXTURES; i++) {
        bindings[2 * i] = (struct vkd3d_shader_resource_binding){VKD3D_SHADER_DESCRIPTOR_TYPE_SRV, 0, i, VKD3D_SHADER_VISIBILITY_ALL,
                                                                 VKD3D_SHADER_BINDING_FLAG_IMAGE, {0, 2 * i, 1}};
        bindings[2 * i + 1] = (struct vkd3d_shader_resource_binding){VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER, 0, i, VKD3D_SHADER_VISIBILITY_ALL,
                                                                     0, {0, 2 * i + 1, 1}};
    }
    struct vkd3d_shader_push_constant_buffer push = {0, 0, VKD3D_SHADER_VISIBILITY_ALL, 0, push_size};
    struct vkd3d_shader_interface_info iface = {VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO};
    iface.bindings = bindings;
    iface.binding_count = 2 * MAX_TEXTURES;
    iface.push_constant_buffers = push_size ? &push : NULL;
    iface.push_constant_buffer_count = push_size ? 1 : 0;
    struct vkd3d_shader_spirv_target_info spirv = {VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO};
    spirv.next = &iface;
    spirv.entry_point = "main";
    spirv.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;
    /* (both core in the Vulkan 1.3 the renderer requires; float controls carry Shader Model 5's
     * denormal mode) */
    static const enum vkd3d_shader_spirv_extension extensions[] = {VKD3D_SHADER_SPIRV_EXTENSION_KHR_FLOAT_CONTROLS,
                                                                   VKD3D_SHADER_SPIRV_EXTENSION_EXT_DEMOTE_TO_HELPER_INVOCATION};
    spirv.extensions = extensions;
    spirv.extension_count = sizeof extensions / sizeof *extensions;
    struct vkd3d_shader_hlsl_source_info hlsl = {VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO};
    hlsl.next = &spirv;
    hlsl.entry_point = entry;
    hlsl.profile = profile;
    struct vkd3d_shader_compile_option options[] = {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_2_1},
        {VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1},
    };
    struct vkd3d_shader_compile_info info = {VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO};
    info.next = &hlsl;
    info.source.code = src;
    info.source.size = strlen(src);
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
    info.options = options;
    info.option_count = sizeof options / sizeof *options;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = what;
    struct vkd3d_shader_code out = {0};
    char *messages = NULL;
    int ret = vkd3d_shader_compile(&info, &out, &messages);
    if (ret < 0) fprintf(stderr, "[gfx] %s (%s): cannot compile: %s\n", what, entry, messages ? messages : "?");
    vkd3d_shader_free_messages(messages);
    if (ret < 0) {
        vkd3d_shader_free_shader_code(&out);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, out.size, out.code};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(vk.dev, &mi, NULL, &m);
    if (r != VK_SUCCESS) fprintf(stderr, "[gfx] %s (%s): the device refused it (%s)\n", what, entry, vk_result(r));
    vkd3d_shader_free_shader_code(&out);
    return m;
}
