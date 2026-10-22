/* display.ini's upscale = fsr with Vulkan: AMD FidelityFX Super Resolution 1 (gpu_fsr.c's HLSL and
 * constants, as gpu_d3d11.c's present uses them). EASU draws the game's picture at its size in the
 * window into a texture of that size, before the window's rendering begins; RCAS draws that,
 * sharpened, into the window, in its rendering. Both over the blit's full-screen triangle. */
#include <stdio.h>
#include <string.h>

#include "gpu_vk.h"
#include "display.h"
#include "gpu_fsr.h"

static const char VS_HLSL[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";

static int state;   /* 0 not yet, 1 ready, -1 failed (the picture is then enlarged linearly) */
static VkPipeline easu, rcas;
static VkPipelineLayout layout;
static VkSampler sampler;
static VkDescriptorSet sets[VK_FRAMES][2];
/* the texture between the two: EASU's output at the picture's size in the window */
static VkImage mid;
static VkDeviceMemory mid_memory;
static VkImageView mid_view;
static unsigned mid_w, mid_h;

static VkPipeline make(VkShaderModule vs, VkShaderModule ps, VkFormat format) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", NULL},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend = {VK_FALSE};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &blend};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &format};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vin, &ia, NULL, &vps, &rs, &ms,
                                       NULL, &cbs, &ds, layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

static int init(void) {
    VkShaderModule vs = vk_hlsl(VS_HLSL, "vs", "vs_5_0", 0, "FSR");
    VkShaderModule e = vs ? vk_hlsl(FSR_HLSL, "ps_easu", "ps_5_0", 64, "FSR") : VK_NULL_HANDLE;
    VkShaderModule r = e ? vk_hlsl(FSR_HLSL, "ps_rcas", "ps_5_0", 64, "FSR") : VK_NULL_HANDLE;
    if (!r) return 0;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                              VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    vk_check(vkCreateSampler(vk.dev, &si, NULL, &sampler), "FSR's sampler");
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkDescriptorSetLayout set_layout;
    vk_check(vkCreateDescriptorSetLayout(vk.dev, &li, NULL, &set_layout), "FSR's descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &push};
    vk_check(vkCreatePipelineLayout(vk.dev, &pli, NULL, &layout), "FSR's pipeline layout");
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * VK_FRAMES}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * VK_FRAMES}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2 * VK_FRAMES, 2, sizes};
    VkDescriptorPool pool;
    vk_check(vkCreateDescriptorPool(vk.dev, &dpi, NULL, &pool), "FSR's descriptor pool");
    for (unsigned i = 0; i < VK_FRAMES; i++)
        for (unsigned k = 0; k < 2; k++) {
            VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout};
            vk_check(vkAllocateDescriptorSets(vk.dev, &dai, &sets[i][k]), "FSR's descriptor sets");
        }
    easu = make(vs, e, VK_FORMAT_R8G8B8A8_UNORM);
    rcas = make(vs, r, vk_swapchain_format);
    vkDestroyShaderModule(vk.dev, vs, NULL);
    vkDestroyShaderModule(vk.dev, e, NULL);
    vkDestroyShaderModule(vk.dev, r, NULL);
    if (!easu || !rcas) return 0;
    fprintf(stderr, "[gfx] FSR 1 ready (EASU + RCAS, sharpness %.1f)\n", display_sharpness());
    return 1;
}

/* The middle texture at w x h, made again when the picture's size in the window changes */
static int mid_target(unsigned w, unsigned h) {
    if (mid && mid_w == w && mid_h == h) return 1;
    if (mid) {   /* (frames in flight may still read the old one) */
        vkQueueWaitIdle(vk.queue);
        vkDestroyImageView(vk.dev, mid_view, NULL);
        vkDestroyImage(vk.dev, mid, NULL);
        vkFreeMemory(vk.dev, mid_memory, NULL);
        mid = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(vk.dev, &ii, NULL, &mid) != VK_SUCCESS) { mid = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, mid, &req);
    mid_memory = vk_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkBindImageMemory(vk.dev, mid, mid_memory, 0), "FSR's texture memory");
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, mid, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk_check(vkCreateImageView(vk.dev, &vi, NULL, &mid_view), "FSR's texture view");
    mid_w = w;
    mid_h = h;
    return 1;
}

static void write_set(VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo image_info = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler_info = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_info, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_info, NULL, NULL},
    };
    vkUpdateDescriptorSets(vk.dev, 2, w, 0, NULL);
}

int vk_fsr_easu(VkFrameTarget *t, u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, unsigned fit_w, unsigned fit_h) {
    if (!state) state = init() ? 1 : -1;
    if (state < 0 || !fit_w || !fit_h) return 0;
    VkImageView src;
    u32 size[2], pic[4];
    if (!vk_draw_picture_view(t->cb, guest, cx, cy, cw, ch, &src, size, pic)) return 0;
    if (pic[2] >= fit_w && pic[3] >= fit_h) return 0;   /* (not enlarged: nothing for FSR to do) */
    if (!mid_target(fit_w, fit_h)) return 0;
    uint32_t con[16];
    fsr_easu_constants(con, (float)pic[2], (float)pic[3], (float)size[0], (float)size[1], (float)fit_w, (float)fit_h, (float)pic[0], (float)pic[1]);
    write_set(sets[t->slot][0], src);
    write_set(sets[t->slot][1], mid_view);
    vk_image_barrier(t->cb, mid, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, mid_view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {fit_w, fit_h}}, 1, 0, 1, &ca, NULL, NULL};
    vkCmdBeginRendering(t->cb, &ri);
    VkViewport vp = vk_viewport(0, 0, fit_w, fit_h);
    VkRect2D all = {{0, 0}, {fit_w, fit_h}};
    vkCmdBindPipeline(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, easu);
    vkCmdSetViewport(t->cb, 0, 1, &vp);
    vkCmdSetScissor(t->cb, 0, 1, &all);
    vkCmdBindDescriptorSets(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[t->slot][0], 0, NULL);
    vkCmdPushConstants(t->cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof con, con);
    vkCmdDraw(t->cb, 3, 1, 0, 0);
    vkCmdEndRendering(t->cb);
    vk_image_barrier(t->cb, mid, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

void vk_fsr_rcas(VkFrameTarget *t, unsigned fit_x, unsigned fit_y, unsigned fit_w, unsigned fit_h) {
    uint32_t con[16] = {0};
    fsr_rcas_constants(con, display_sharpness());
    con[2] = fit_x;   /* (where RCAS's output starts in the window) */
    con[3] = fit_y;
    VkViewport vp = vk_viewport(fit_x, fit_y, fit_w, fit_h);
    VkRect2D where = {{(int32_t)fit_x, (int32_t)fit_y}, {fit_w, fit_h}};
    vkCmdBindPipeline(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rcas);
    vkCmdSetViewport(t->cb, 0, 1, &vp);
    vkCmdSetScissor(t->cb, 0, 1, &where);
    vkCmdBindDescriptorSets(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[t->slot][1], 0, NULL);
    vkCmdPushConstants(t->cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof con, con);
    vkCmdDraw(t->cb, 3, 1, 0, 0);
}
