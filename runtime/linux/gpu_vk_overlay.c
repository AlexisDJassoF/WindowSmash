/* The overlay (frame rate, statistics, the start-up's screens, F7's notice, the mode bar) with
 * Vulkan: the quads gpu_overlay_quads.c builds, from a vertex ring in host-visible memory (a part
 * of it for each frame in flight), the glyph atlas a one-channel image. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gpu_vk.h"
#include "gpu_overlay_quads.h"

#define MAX_VERTS (8192 * 6)          /* (gpu_overlay_quads.c's most for one drawing) */
#define DRAWS_PER_FRAME 4             /* the overlay, the notice, the bar, a screen */
#define RING_PART ((VkDeviceSize)MAX_VERTS * DRAWS_PER_FRAME * sizeof(OverlayVertex))

/* (the same drawing as gpu_overlay.c's: position in pixels to clip space, colour times coverage) */
static const char HLSL[] =
    "cbuffer C : register(b0) { float2 inv_size; };\n"
    "struct I { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "O vs(I i) { O o; o.p = float4(i.p * inv_size * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = i.uv; o.c = i.c; return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(O i) : SV_Target { return float4(i.c.rgb, i.c.a * t.Sample(s, i.uv).r); }\n";

static int state;   /* 0 not yet, 1 ready, -1 failed */
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkDescriptorSet set;
static VkBuffer ring;
static OverlayVertex *ring_mem;
static VkDeviceSize ring_used;       /* in this frame's part */
static u64 ring_serial;               /* the frame that part is for */

static int make_atlas(VkImageView *view) {
    static unsigned char coverage[OVERLAY_ATLAS_W * OVERLAY_ATLAS_H];
    if (!overlay_atlas(coverage)) return 0;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM,
                            {OVERLAY_ATLAS_W, OVERLAY_ATLAS_H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    vk_check(vkCreateImage(vk.dev, &ii, NULL, &image), "the overlay's atlas");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, image, &req);
    vk_check(vkBindImageMemory(vk.dev, image, vk_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0), "the atlas's memory");
    VkDeviceMemory staging_mem;
    void *staging_ptr;
    VkBuffer staging = vk_buffer(sizeof coverage, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_mem, &staging_ptr);
    memcpy(staging_ptr, coverage, sizeof coverage);
    VkCommandBuffer cb = vk_once_begin();
    vk_image_barrier(cb, image, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {OVERLAY_ATLAS_W, OVERLAY_ATLAS_H, 1}};
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_image_barrier(cb, image, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_once_end(cb);
    vkDestroyBuffer(vk.dev, staging, NULL);
    vkFreeMemory(vk.dev, staging_mem, NULL);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UNORM,
                                {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk_check(vkCreateImageView(vk.dev, &vi, NULL, view), "the atlas's view");
    return 1;
}

static int init(void) {
    VkShaderModule vs = vk_hlsl(HLSL, "vs", "vs_5_0", 16, "overlay"), ps = vk_hlsl(HLSL, "ps", "ps_5_0", 16, "overlay");
    VkImageView atlas;
    if (!vs || !ps || !make_atlas(&atlas)) return 0;

    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                              VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    vk_check(vkCreateSampler(vk.dev, &si, NULL, &sampler), "the overlay's sampler");
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkDescriptorSetLayout set_layout;
    vk_check(vkCreateDescriptorSetLayout(vk.dev, &li, NULL, &set_layout), "the overlay's descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &push};
    vk_check(vkCreatePipelineLayout(vk.dev, &pli, NULL, &layout), "the overlay's pipeline layout");
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, sizes};
    VkDescriptorPool pool;
    vk_check(vkCreateDescriptorPool(vk.dev, &dpi, NULL, &pool), "the overlay's descriptor pool");
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout};
    vk_check(vkAllocateDescriptorSets(vk.dev, &dai, &set), "the overlay's descriptor set");
    VkDescriptorImageInfo image_info = {VK_NULL_HANDLE, atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler_info = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_info, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_info, NULL, NULL},
    };
    vkUpdateDescriptorSets(vk.dev, 2, w, 0, NULL);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", NULL},
    };
    VkVertexInputBindingDescription vb = {0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(OverlayVertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 3, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend = {VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                                 VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &blend};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &vk_swapchain_format};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vin, &ia, NULL, &vps, &rs, &ms,
                                       NULL, &cbs, &ds, layout};
    vk_check(vkCreateGraphicsPipelines(vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline), "the overlay's pipeline");
    vkDestroyShaderModule(vk.dev, vs, NULL);
    vkDestroyShaderModule(vk.dev, ps, NULL);

    VkDeviceMemory ring_memory;
    void *mapped;
    ring = vk_buffer(RING_PART * VK_FRAMES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     &ring_memory, &mapped);
    ring_mem = mapped;
    return 1;
}

void vk_overlay_draw(VkFrameTarget *t, unsigned x, unsigned y, unsigned w, unsigned h) {
    if (!state) state = init() ? 1 : -1;
    if (state < 0 || !w || !h) return;
    unsigned n;
    const OverlayVertex *verts = overlay_quads(&n);
    if (ring_serial != t->serial) { ring_serial = t->serial; ring_used = 0; }
    VkDeviceSize bytes = (VkDeviceSize)n * sizeof *verts;
    if (!n || ring_used + bytes > RING_PART) return;
    VkDeviceSize offset = RING_PART * t->slot + ring_used;
    memcpy((char *)ring_mem + offset, verts, bytes);
    ring_used += bytes;

    VkViewport vp = vk_viewport(x, y, w, h);
    VkRect2D scissor = {{0, 0}, {t->w, t->h}};
    float inv[4] = {1.0f / (float)w, 1.0f / (float)h, 0, 0};
    vkCmdBindPipeline(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(t->cb, 0, 1, &vp);
    vkCmdSetScissor(t->cb, 0, 1, &scissor);
    vkCmdBindDescriptorSets(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(t->cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof inv, inv);
    vkCmdBindVertexBuffers(t->cb, 0, 1, &ring, &offset);
    vkCmdDraw(t->cb, n, 1, 0, 0);
}
