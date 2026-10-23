/* The Vulkan renderer's drawing of the game (gpu_backend.h's gfx_clear, gfx_draw, gfx_blit2d,
 * gfx_dispatch and the GPU-resident buffers, and the picture gfx_present shows): what gpu_d3d11.c does
 * with Direct3D 11, on Vulkan.
 *
 * Render targets ("surfaces") and textures are images; each keeps its layout and its last use, and
 * a use that needs it gets a barrier (synchronization2), outside rendering: a draw whose targets or
 * barriers differ from the one before ends the rendering and begins another (dynamic rendering).
 * Guest memory a draw reads - constant buffers, vertex streams, indices, storage buffers, texture
 * pixels - goes into the frame slot's upload ring (host-visible memory; the slot is reused once
 * the GPU is done with it). Descriptors are sets from the slot's pool, written per draw, in the
 * layout gpu_spirv.h fixes; DrawParams is a dynamic uniform buffer in the ring. Pipelines are made at
 * a draw's first use of their shaders and fixed state; viewport, scissor, culling, facing, depth and
 * stencil are dynamic state. The shaders are gpu_spirv.c's, translated at their first draw. */
#define _CRT_SECURE_NO_WARNINGS
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>   /* process_vm_readv */
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "gpu_vk.h"
#include "display.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_scale.h"
#include "gpu_spirv.h"
#include "gpu_texture.h"
#include "maxwell3d.h"

extern u64 gfx_stat_clears, gfx_stat_drawn, gfx_stat_skipped, gfx_skip_shader, gfx_skip_topology, gfx_skip_target;
extern u64 gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_textures, gfx_stat_watch_us, gfx_stat_upload_bytes;

static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static int draw_logging;   /* F4: this frame's draws to the log, as gpu_d3d11.c's [draw] lines */

/* The CPU's writes the write watch found: per (hashed) page, the generation they were found in
 * (the kept uploads look here: the watch's bit is gone once anyone resets it) */
static u32 page_gen[1 << 20], gen_now;
#define PAGE_SLOT(page) ((u32)((u64)(page) * 0x9E3779B97F4A7C15ull >> 44))
/* The write watch over [lo, hi) reset, the pages it had found written noted first */
static void watch_reset(u64 lo, u64 hi) {
    static void *pages[4096];
    u64 from = lo & ~0xFFFull, to = (hi + 0xFFF) & ~0xFFFull;
    for (u64 at = from; at < to;) {
        ULONG_PTR count = 4096;
        ULONG granularity;
        if (GetWriteWatch(0, (void *)at, to - at, pages, &count, &granularity) != 0 || !count) break;
        ++gen_now;
        for (ULONG_PTR k = 0; k < count; k++) page_gen[PAGE_SLOT((u64)pages[k] >> 12)] = gen_now;
        if (count < 4096) break;
        at = (u64)pages[count - 1] + 0x1000;   /* (more than the list holds: on from the last) */
    }
    ResetWriteWatch((void *)lo, hi - lo);
}
static u64 vk_stat_residents_made, vk_stat_inputs_made, vk_stat_textures_made;   /* (SSBU_LOG_STATS) */
static u64 sb_bytes, texture_bytes;   /* (SSBU_LOG_STATS: storage buffers' and textures' uploads) */

/* ---- per-frame memory: the upload ring, descriptor pools ----------------------------------- */
#define RING_SIZE (96u << 20)
static struct {
    VkBuffer buf;
    u8 *map;
    VkDeviceSize used;
    VkDescriptorPool pool;
    VkDescriptorSet params_set;   /* set 3: DrawParams, a dynamic uniform buffer in this ring */
} slots[VK_FRAMES];
static VkDescriptorSetLayout set_layout[4];
static VkPipelineLayout layout;
static VkPipelineCache pipeline_cache;
static VkDeviceSize uniform_align = 256, storage_align = 256;
static int ready;

/* Objects freed once the GPU is done with the frame that last used them */
typedef struct Grave { u64 value; VkImage image; VkImageView view; VkDeviceMemory memory; VkBuffer buffer; } Grave;
static Grave *graves;
static u32 ngraves, graves_cap;

static void bury(VkImage image, VkImageView view, VkDeviceMemory memory) {
    if (!image && !view && !memory) return;
    if (ngraves == graves_cap) graves = realloc(graves, sizeof *graves * (graves_cap = graves_cap * 2 + 64));
    graves[ngraves++] = (Grave){vk.submitted + 1, image, view, memory, VK_NULL_HANDLE};   /* (the submission being recorded uses them last) */
}
static void bury_buffer(VkBuffer buffer, VkDeviceMemory memory) {
    if (!buffer) return;
    bury(VK_NULL_HANDLE, VK_NULL_HANDLE, memory);
    graves[ngraves - 1].buffer = buffer;
}

static void release_graves(void) {
    u64 done = 0;
    vkGetSemaphoreCounterValue(vk.dev, vk.timeline, &done);
    u32 k = 0;
    for (u32 i = 0; i < ngraves; i++) {
        Grave *g = &graves[i];
        if (g->value > done) { graves[k++] = *g; continue; }
        if (g->view) vkDestroyImageView(vk.dev, g->view, NULL);
        if (g->image) vkDestroyImage(vk.dev, g->image, NULL);
        if (g->buffer) vkDestroyBuffer(vk.dev, g->buffer, NULL);
        if (g->memory) vkFreeMemory(vk.dev, g->memory, NULL);
    }
    ngraves = k;
}

static void cache_load(void);
static void dynamic_init(void);
static void init(void) {
    if (ready) return;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.pd, &props);
    if (props.limits.minUniformBufferOffsetAlignment > uniform_align) uniform_align = props.limits.minUniformBufferOffsetAlignment;
    if (props.limits.minStorageBufferOffsetAlignment > storage_align) storage_align = props.limits.minStorageBufferOffsetAlignment;
    VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    static const struct { VkDescriptorType type; u32 count; } sets[4] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SPIRV_STREAM_BINDING + 32}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}};
    for (u32 s = 0; s < 4; s++) {
        VkDescriptorSetLayoutBinding b[96];
        for (u32 i = 0; i < sets[s].count; i++) b[i] = (VkDescriptorSetLayoutBinding){i, sets[s].type, 1, stages, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, sets[s].count, b};
        vk_check(vkCreateDescriptorSetLayout(vk.dev, &li, NULL, &set_layout[s]), "a descriptor set layout");
    }
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 4, set_layout, 0, NULL};
    vk_check(vkCreatePipelineLayout(vk.dev, &pli, NULL, &layout), "the pipeline layout");
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vk_check(vkCreatePipelineCache(vk.dev, &ci, NULL, &pipeline_cache), "the pipeline cache");
    for (u32 i = 0; i < VK_FRAMES; i++) {
        VkDeviceMemory mem;
        void *map;
        slots[i].buf = vk_buffer(RING_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem, &map);
        slots[i].map = map;
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 400000}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 200000},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}};
        VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 16384, 3, sizes};
        vk_check(vkCreateDescriptorPool(vk.dev, &dpi, NULL, &slots[i].pool), "a descriptor pool");
        /* (set 3 lives in a pool of its own: the slot's pool is reset every frame) */
        VkDescriptorPoolSize psize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
        VkDescriptorPoolCreateInfo ppi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &psize};
        VkDescriptorPool ppool;
        vk_check(vkCreateDescriptorPool(vk.dev, &ppi, NULL, &ppool), "a descriptor pool");
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, ppool, 1, &set_layout[3]};
        vk_check(vkAllocateDescriptorSets(vk.dev, &ai, &slots[i].params_set), "the parameters' set");
        VkDescriptorBufferInfo bi = {slots[i].buf, 0, sizeof(DrawParams)};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, slots[i].params_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                  NULL, &bi, NULL};
        vkUpdateDescriptorSets(vk.dev, 1, &w, 0, NULL);
    }
    dynamic_init();
    ready = 1;
    cache_load();
}

static void forget_state(void);
void vk_draw_slot_start(unsigned slot) {
    init();
    slots[slot].used = 0;
    vkResetDescriptorPool(vk.dev, slots[slot].pool, 0);
    forget_state();
}

/* Room in the slot's ring: the offset, and where to write. 0 (and *at NULL) when full: the frame
 * is flushed and the next slot's ring taken. */
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, u8 **at, VkBuffer *buf);
/* (SSBU_LOG_STATS: the ring's bytes by what they were for) */
enum { RING_TEX, RING_GPU, RING_COPY, RING_READBACK, RING_PARAMS, RING_INDEX, RING_CS, RING_KINDS };
static u64 ring_bytes[RING_KINDS];
#define ring_alloc_for(kind, size, align, at, buf) (ring_bytes[kind] += (size), ring_alloc(size, align, at, buf))

/* ---- images and their layouts ---------------------------------------------------------------- */
typedef struct Img {
    VkImage image;
    VkDeviceMemory memory;
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;     /* its last use */
    VkAccessFlags2 access;
    VkImageAspectFlags aspect;
    u32 levels, layers, width, height;
} Img;

static int is_write(VkAccessFlags2 a) {
    return (a & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                 VK_ACCESS_2_SHADER_WRITE_BIT)) != 0;
}

/* Barriers gathered for the next draw (or other work), recorded together */
static VkImageMemoryBarrier2 pending[64];
static u32 npending;
static int rendering;                 /* the game's rendering has begun */

static void flush_barriers(VkCommandBuffer cb) {
    if (!npending) return;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, npending, pending};
    vkCmdPipelineBarrier2(cb, &di);
    npending = 0;
}

/* The image is about to be used thus: a barrier when the layout changes or either use writes. 1 if
 * one was added (the rendering must end before it). */
static int use(Img *m, VkImageLayout layout_, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    if (m->layout == layout_ && !is_write(m->access) && !is_write(access)) {   /* (read after read: just note this one too) */
        m->stage |= stage;
        m->access |= access;
        return 0;
    }
    if (m->layout == layout_ && m->access == access && m->stage == stage &&
        (access & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)))
        return 0;   /* (attachment after attachment: rasterization order) */
    if (npending == 64) flush_barriers(vk_cmd());
    pending[npending++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, m->stage ? m->stage : VK_PIPELINE_STAGE_2_NONE,
                                                  m->access, stage, access, m->layout, layout_, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                                  m->image, {m->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    m->layout = layout_;
    m->stage = stage;
    m->access = access;
    return 1;
}

static int make_image(Img *m, VkImageType type, VkFormat format, u32 w, u32 h, u32 d, u32 levels, u32 layers, VkImageUsageFlags usage,
                      VkImageCreateFlags flags, const VkFormat *view_formats, u32 nview_formats) {
    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, nview_formats, view_formats};
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nview_formats ? &list : NULL, flags, type, format, {w, h, d}, levels, layers,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    memset(m, 0, sizeof *m);
    if (vkCreateImage(vk.dev, &ii, NULL, &m->image) != VK_SUCCESS) { m->image = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, m->image, &req);
    m->memory = vk_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkBindImageMemory(vk.dev, m->image, m->memory, 0), "an image's memory");
    m->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    m->levels = levels;
    m->layers = layers;
    m->width = w;
    m->height = h;
    return 1;
}

static VkImageView make_view(const Img *m, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, u32 layers) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, m->image, type, format, {0},
                                {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, layers}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vk.dev, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

/* ---- formats ----------------------------------------------------------------------------- */
/* DXGI's formats (gpu_texture.c's texture headers, and the render target formats below) as Vulkan's */
static VkFormat vk_format_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case DXGI_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    case DXGI_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case DXGI_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case DXGI_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SINT;
    case DXGI_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R10G10B10A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case DXGI_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case DXGI_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case DXGI_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
    case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case DXGI_FORMAT_R32_SINT: return VK_FORMAT_R32_SINT;
    case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_B5G6R5_UNORM: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case DXGI_FORMAT_B5G5R5A1_UNORM: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case DXGI_FORMAT_B4G4R4A4_UNORM: return VK_FORMAT_A4R4G4B4_UNORM_PACK16;
    case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case DXGI_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SINT;
    case DXGI_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_TYPELESS: return VK_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
    case DXGI_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case DXGI_FORMAT_R8_SINT: return VK_FORMAT_R8_SINT;
    case DXGI_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case DXGI_FORMAT_BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case DXGI_FORMAT_BC4_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
    case DXGI_FORMAT_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case DXGI_FORMAT_BC5_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
    case DXGI_FORMAT_BC6H_UF16: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case DXGI_FORMAT_BC6H_SF16: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case DXGI_FORMAT_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* Maxwell render target formats (colour), as gpu_d3d11.c's color_format */
static VkFormat color_format(u32 f) {
    switch (f) {
    case 0xC0: case 0xC3: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0xC1: case 0xC4: return VK_FORMAT_R32G32B32A32_SINT;
    case 0xC2: case 0xC5: return VK_FORMAT_R32G32B32A32_UINT;
    case 0xC6: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0xC7: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0xC8: return VK_FORMAT_R16G16B16A16_SINT;
    case 0xC9: return VK_FORMAT_R16G16B16A16_UINT;
    case 0xCA: case 0xCE: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0xCB: return VK_FORMAT_R32G32_SFLOAT;
    case 0xCC: return VK_FORMAT_R32G32_SINT;
    case 0xCD: return VK_FORMAT_R32G32_UINT;
    case 0xCF: case 0xE6: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0xD0: case 0xE7: return VK_FORMAT_B8G8R8A8_SRGB;
    case 0xD1: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0xD2: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0xD5: case 0xF9: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0xD6: case 0xFA: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0xD7: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0xD8: return VK_FORMAT_R8G8B8A8_SINT;
    case 0xD9: return VK_FORMAT_R8G8B8A8_UINT;
    case 0xDA: return VK_FORMAT_R16G16_UNORM;
    case 0xDB: return VK_FORMAT_R16G16_SNORM;
    case 0xDC: return VK_FORMAT_R16G16_SINT;
    case 0xDD: return VK_FORMAT_R16G16_UINT;
    case 0xDE: return VK_FORMAT_R16G16_SFLOAT;
    case 0xE0: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0xE3: return VK_FORMAT_R32_SINT;
    case 0xE4: return VK_FORMAT_R32_UINT;
    case 0xE5: return VK_FORMAT_R32_SFLOAT;
    case 0xE8: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0xE9: case 0xF8: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 0xEA: return VK_FORMAT_R8G8_UNORM;
    case 0xEB: return VK_FORMAT_R8G8_SNORM;
    case 0xEC: return VK_FORMAT_R8G8_SINT;
    case 0xED: return VK_FORMAT_R8G8_UINT;
    case 0xEE: return VK_FORMAT_R16_UNORM;
    case 0xEF: return VK_FORMAT_R16_SNORM;
    case 0xF0: return VK_FORMAT_R16_SINT;
    case 0xF1: return VK_FORMAT_R16_UINT;
    case 0xF2: return VK_FORMAT_R16_SFLOAT;
    case 0xF3: return VK_FORMAT_R8_UNORM;
    case 0xF4: return VK_FORMAT_R8_SNORM;
    case 0xF5: return VK_FORMAT_R8_SINT;
    case 0xF6: return VK_FORMAT_R8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

static int depth_usable(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk.pd, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}
/* Maxwell depth formats (Z24S8 where the device has it, else Z32S8) */
static VkFormat depth_format(u32 f) {
    static int d24 = -1;
    switch (f) {
    case 0x0A: return VK_FORMAT_D32_SFLOAT;
    case 0x13: return VK_FORMAT_D16_UNORM;
    case 0x14: case 0x15: case 0x16:
        if (d24 < 0) d24 = depth_usable(VK_FORMAT_D24_UNORM_S8_UINT);
        return d24 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x19: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}
static int has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

/* 8-bit colour: the same bytes rendered and read as sRGB or plain, as each use says */
static VkFormat other_gamma(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static int is_srgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }
static VkFormat plain_of(VkFormat f) { return is_srgb(f) ? other_gamma(f) : f; }

static void warn_format(const char *what, u32 f) {
    static u32 seen[64], n;
    for (u32 i = 0; i < n; i++) if (seen[i] == (f | (u32)(what[0] << 24))) return;
    if (n < 64) seen[n++] = f | (u32)(what[0] << 24);
    fprintf(stderr, "[gfx] Vulkan: unsupported %s format %#x\n", what, f);
}

/* ---- surfaces: render targets on the host ------------------------------------------------- */
typedef struct Surface {
    u64 guest;
    u32 width, height, format, depth;     /* (width x height: the size the game sees) */
    u32 pw, ph;                           /* the image's size: width x height times scale */
    float scale;
    VkFormat storage, view_format;        /* the image's format; how it is rendered to now */
    Img img;
    VkImageView view;                     /* as view_format (depth: depth and stencil) */
    VkImageView sampled[2][2];            /* [srgb][array]: read as a texture (depth: its depth) */
    u64 used;
} Surface;
static Surface surfaces[2048];
static u32 nsurfaces;
static u64 tick;

static void release_surface(Surface *s) {
    bury(VK_NULL_HANDLE, s->view, VK_NULL_HANDLE);
    for (u32 i = 0; i < 4; i++) bury(VK_NULL_HANDLE, s->sampled[i / 2][i % 2], VK_NULL_HANDLE);
    bury(s->img.image, VK_NULL_HANDLE, s->img.memory);
    memset(s, 0, sizeof *s);
}

static float render_scale(void) {
    static float s = -1;
    if (s < 0) {
        s = display_scale();
        if (s != 1.0f) {
            unsigned sw, sh;
            display_screen_size(&sw, &sh);
            fprintf(stderr, "[gfx] render scale %.0f%%: %u x %u drawn at %u x %u\n", s * 100, sw, sh, scaled_size(sw, s), scaled_size(sh, s));
        }
    }
    return s;
}
static float surface_scale(u32 width, u32 height) {
    unsigned sw, sh;
    display_screen_size(&sw, &sh);
    return scale_for(width, height, sw, sh, render_scale());
}

static void surface_view(Surface *s, VkFormat view) {
    s->view_format = view;
    s->view = make_view(&s->img, VK_IMAGE_VIEW_TYPE_2D, view, s->depth ? s->img.aspect : VK_IMAGE_ASPECT_COLOR_BIT, 1);
}

/* The host image standing for the render target at guest, made on first use */
static void clear_surface(Surface *s, int color_, const float c[4], VkImageAspectFlags aspects, float depth, u32 stencil);
static Surface *surface(u64 guest, u32 width, u32 height, u32 format, int depth) {
    if (!guest || !width || !height || width > 16384 || height > 16384) return NULL;
    Surface *free_slot = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->img.image) { if (!free_slot) free_slot = s; continue; }
        if (s->guest == guest && (int)s->depth == depth) {
            if (s->width == width && s->height == height && s->format == format) { s->used = ++tick; return s; }
            VkFormat now = depth ? VK_FORMAT_UNDEFINED : color_format(format);
            if (s->width == width && s->height == height && now != VK_FORMAT_UNDEFINED && plain_of(now) == s->storage) {
                bury(VK_NULL_HANDLE, s->view, VK_NULL_HANDLE);   /* the same bytes, now sRGB or plain: a new view */
                surface_view(s, now);
                s->format = format;
                s->used = ++tick;
                return s;
            }
            release_surface(s);   /* the memory now holds a different surface */
            if (!free_slot) free_slot = s;
        }
    }
    VkFormat view = depth ? depth_format(format) : color_format(format);
    if (view == VK_FORMAT_UNDEFINED) { warn_format(depth ? "depth" : "color", format); return NULL; }
    Surface *s = free_slot;
    if (!s) {
        if (nsurfaces < sizeof surfaces / sizeof *surfaces) s = &surfaces[nsurfaces++];
        else {   /* evict the least recently used */
            s = &surfaces[0];
            for (u32 i = 1; i < nsurfaces; i++) if (surfaces[i].used < s->used) s = &surfaces[i];
            release_surface(s);
        }
    }
    float scale = surface_scale(width, height);
    u32 pw = scaled_size(width, scale), ph = scaled_size(height, scale);
    VkFormat storage = depth ? view : plain_of(view), other = other_gamma(storage);
    VkFormat both[2] = {storage, other};
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!make_image(&s->img, VK_IMAGE_TYPE_2D, storage, pw, ph, 1, 1, 1, usage, other ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, both, other ? 2 : 0))
        return NULL;
    s->img.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(view) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
    s->pw = pw; s->ph = ph; s->scale = scale;
    s->depth = (u32)depth;
    s->storage = storage;
    surface_view(s, view);
    s->guest = guest; s->width = width; s->height = height; s->format = format;
    s->used = ++tick;
    /* zeros to begin with, as Direct3D 11 has a new resource (DXVK clears it): the game draws only
     * part of some targets and composes all of them (an image's own contents are undefined) */
    static const float zero[4];
    clear_surface(s, !depth, zero, s->img.aspect, 0.0f, 0);
    return s;
}

/* A surface read as a texture: srgb or plain (8-bit colour), a 2D or 2D-array view */
static VkImageView surface_sampled(Surface *s, int srgb, int array) {
    VkFormat f = s->depth ? s->view_format : (other_gamma(s->storage) && srgb) ? other_gamma(s->storage) : s->storage;
    srgb = s->depth ? 0 : srgb && other_gamma(s->storage);
    VkImageView *v = &s->sampled[srgb][array];
    if (!*v) *v = make_view(&s->img, array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, f, s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 1);
    return *v;
}
static VkImageLayout read_layout(const Img *m) {
    return m->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* ---- textures ------------------------------------------------------------------------------ */
typedef struct Texture {
    u64 guest, size;
    u32 tic[8];
    u64 sample_hash;
    int watched, stale;    /* in the write-watched alias area; written since it was read */
    Img img;
    VkFormat format;
    VkImageView views[8];  /* by the shader's texture type (TEX_*) */
    u64 used;
    /* what its image was made as (an image given up goes to the pool, for another of the same) */
    VkImageType type;
    VkImageCreateFlags flags;
    u32 w, h, depth, levels, layers;
} Texture;
static Texture textures[4096];
static u32 ntextures;

static int watched(u64 g, u64 size) { return g >= ALIAS_BASE && g + size <= ALIAS_BASE + ALIAS_SIZE; }

/* Which textures has the CPU written since they were read (the write watch, once a frame) */
/* The pages written since their watch was last reset, in the watched textures and resident regions:
 * asked once a frame over their ranges sorted and merged where they touch (a kernel call for each of
 * thousands of textures took ~2-4 ms a frame; one over all their span takes in the game's own memory
 * between them, written all the time), each range then looked up in it. Valid during note_writes
 * only; not at all if more pages were written than the list holds (each range is then asked alone). */
static void *frame_dirty[1 << 16];
static ULONG_PTR nframe_dirty;
static int frame_dirty_ok;
/* 1 if a page of [lo, hi) is in the frame's list, 0 if none, -1 if the list is not to be used */
static int frame_range_dirty(u64 lo, u64 hi) {
    if (!frame_dirty_ok) return -1;
    u64 first = lo & ~0xFFFull;
    ULONG_PTR a = 0, b = nframe_dirty;
    while (a < b) { ULONG_PTR m = (a + b) / 2; if ((u64)frame_dirty[m] < first) a = m + 1; else b = m; }
    return a < nframe_dirty && (u64)frame_dirty[a] < hi;
}

static u64 watch_split_ns[3], watch_span, watch_pages;   /* (SSBU_LOG_STATS: note_writes' parts) */
static int watch_ok;
typedef struct WatchRange { u64 lo, hi; } WatchRange;
static int range_order(const void *a, const void *b) {
    const WatchRange *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}
static void residents_ranges(WatchRange *out, u32 *n, u32 cap);
static void residents_sync_all(void);
static void note_writes(void) {
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    static u8 written[4096];
    static WatchRange ranges[4096 + 512];
    u32 nranges = 0;
    for (u32 i = 0; i < ntextures; i++) {
        Texture *t = &textures[i];
        if (!t->img.image || !t->watched || t->stale) continue;
        ranges[nranges++] = (WatchRange){t->guest & ~0xFFFull, (t->guest + t->size + 0xFFF) & ~0xFFFull};
    }
    residents_ranges(ranges, &nranges, sizeof ranges / sizeof *ranges);
    u64 n0 = rt_now_ns();
    qsort(ranges, nranges, sizeof *ranges, range_order);
    u32 merged = 0;
    for (u32 i = 0; i < nranges; i++) {   /* (touching, or a page or so apart) */
        if (merged && ranges[i].lo <= ranges[merged - 1].hi + 0x2000) {
            if (ranges[i].hi > ranges[merged - 1].hi) ranges[merged - 1].hi = ranges[i].hi;
        } else ranges[merged++] = ranges[i];
    }
    ULONG granularity;
    const ULONG_PTR cap = sizeof frame_dirty / sizeof *frame_dirty;
    frame_dirty_ok = 1;
    nframe_dirty = 0;
    for (u32 i = 0; i < merged && frame_dirty_ok; i++) {
        ULONG_PTR count = cap - nframe_dirty;
        if (GetWriteWatch(0, (void *)ranges[i].lo, ranges[i].hi - ranges[i].lo, frame_dirty + nframe_dirty, &count, &granularity) != 0 ||
            nframe_dirty + count >= cap)
            frame_dirty_ok = 0;
        else nframe_dirty += count;
    }
    if (!frame_dirty_ok) nframe_dirty = 0;
    u64 lo = 0, hi = merged;   /* (for the statistics: the ranges asked) */
    u64 n1 = rt_now_ns();
    for (u32 i = 0; i < ntextures; i++) {
        Texture *t = &textures[i];
        written[i] = 0;
        if (!t->img.image || !t->watched || t->stale) continue;
        int d = frame_range_dirty(t->guest, t->guest + t->size);
        if (d < 0) {
            void *page;
            ULONG_PTR one = 1;
            d = GetWriteWatch(0, (void *)t->guest, t->size, &page, &one, &granularity) == 0 && one;
        }
        written[i] = (u8)d;
    }
    for (u32 i = 0; i < ntextures; i++)
        if (written[i]) { textures[i].stale = 1; watch_reset(textures[i].guest, textures[i].guest + textures[i].size); }
    u64 n2 = rt_now_ns();
    residents_sync_all();   /* (after the textures: they share the pages' write watch) */
    u64 n3 = rt_now_ns();
    watch_split_ns[0] += n1 - n0; watch_split_ns[1] += n2 - n1; watch_split_ns[2] += n3 - n2;
    watch_span = hi - lo; watch_pages = nframe_dirty; watch_ok = frame_dirty_ok;
    (void)lo;
    frame_dirty_ok = 0;
    QueryPerformanceCounter(&t1);
    gfx_stat_watch_us += (u64)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
}

static u64 sample_hash(u64 g, u64 size) {
    u64 h = 1469598103934665603ull;
    const u8 *p = P(u8, g);
    u64 step = size / 64 ? size / 64 : 1;
    for (u64 i = 0; i < size; i += step) { u64 v; memcpy(&v, p + (i & ~7ull), 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}

static void warn_texture(const TextureDesc *d) {
    static u32 seen[64], n;
    u32 key = d->format | d->type << 8 | d->header_version << 12;
    for (u32 i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 64) seen[n++] = key;
    fprintf(stderr, "[gfx] Vulkan: unsupported texture: format %#x, type %u, layout %u (%ux%u)\n", d->format, d->type, d->header_version,
            d->width, d->height);
}

/* The texture cache's index, by guest address and texture header (as gpu_d3d11.c's): entries are
 * checked against the texture they name, so a slot given to another texture just stops matching;
 * the index is rebuilt now and then. */
#define TEXTURE_INDEX 16384
static u16 texture_index[TEXTURE_INDEX];   /* textures[] index + 1 */
static u32 texture_index_adds;

static u32 texture_key(u64 g, const u32 *tic) {
    u64 h = g * 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < 8; i++) h = (h ^ tic[i]) * 0x100000001B3ull;
    return (u32)(h >> 40) & (TEXTURE_INDEX - 1);
}
static Texture *texture_find(u64 g, const u32 *tic) {
    for (u32 k = texture_key(g, tic), n = 0; texture_index[k] && n < TEXTURE_INDEX; k = (k + 1) & (TEXTURE_INDEX - 1), n++) {
        Texture *t = &textures[texture_index[k] - 1];
        if (t->img.image && t->guest == g && !memcmp(t->tic, tic, 32)) return t;
    }
    return NULL;
}
static void texture_index_put(Texture *t) {
    u32 k = texture_key(t->guest, t->tic);
    while (texture_index[k]) k = (k + 1) & (TEXTURE_INDEX - 1);
    texture_index[k] = (u16)(t - textures + 1);
}
static void texture_index_add(Texture *t) {
    if (++texture_index_adds >= TEXTURE_INDEX / 2) {   /* entries for textures since replaced pile up: start again */
        memset(texture_index, 0, sizeof texture_index);
        texture_index_adds = 0;
        for (u32 i = 0; i < ntextures; i++)
            if (textures[i].img.image && &textures[i] != t) { texture_index_put(&textures[i]); texture_index_adds++; }
    }
    texture_index_put(t);
}

/* Images of textures given up (their contents changed, or another took their place), kept with
 * their views for a texture of the same kind: making one waits for the kernel (memory cleared,
 * mapped) - a few milliseconds each, which a menu that changes its textures as the cursor moves
 * did several times a second. A pooled image's last use is ordered before its next (one queue,
 * and its layout and last access go with it); the oldest is freed when the pool is full. */
#define IMAGE_POOL 128
static struct { Img img; VkImageView views[8]; VkFormat format; VkImageType type; VkImageCreateFlags flags; u32 w, h, depth, levels, layers; } image_pool[IMAGE_POOL];
static u32 nimage_pool;
u64 vk_stat_images_reused;

static void release_texture(Texture *t) {
    if (t->img.image) {
        if (nimage_pool == IMAGE_POOL) {   /* (the oldest freed, once the GPU is done with it) */
            for (u32 i = 0; i < 8; i++) bury(VK_NULL_HANDLE, image_pool[0].views[i], VK_NULL_HANDLE);
            bury(image_pool[0].img.image, VK_NULL_HANDLE, image_pool[0].img.memory);
            memmove(image_pool, image_pool + 1, sizeof *image_pool * --nimage_pool);
        }
        image_pool[nimage_pool].img = t->img;
        memcpy(image_pool[nimage_pool].views, t->views, sizeof t->views);
        image_pool[nimage_pool].format = t->format;
        image_pool[nimage_pool].type = t->type;
        image_pool[nimage_pool].flags = t->flags;
        image_pool[nimage_pool].w = t->w; image_pool[nimage_pool].h = t->h; image_pool[nimage_pool].depth = t->depth;
        image_pool[nimage_pool].levels = t->levels; image_pool[nimage_pool].layers = t->layers;
        nimage_pool++;
    }
    memset(t->views, 0, sizeof t->views);
    memset(&t->img, 0, sizeof t->img);
}

/* An image of this kind from the pool, with its views, into t; 0 if there is none */
static int pooled_image(Texture *t, VkImageType type, VkFormat format, u32 w, u32 h, u32 depth, u32 levels, u32 layers, VkImageCreateFlags flags) {
    for (u32 i = nimage_pool; i-- > 0;) {   /* (the latest given up first) */
        if (image_pool[i].format != format || image_pool[i].type != type || image_pool[i].flags != flags || image_pool[i].w != w || image_pool[i].h != h ||
            image_pool[i].depth != depth || image_pool[i].levels != levels || image_pool[i].layers != layers)
            continue;
        t->img = image_pool[i].img;
        memcpy(t->views, image_pool[i].views, sizeof t->views);
        memmove(image_pool + i, image_pool + i + 1, sizeof *image_pool * (--nimage_pool - i));
        vk_stat_images_reused++;
        return 1;
    }
    return 0;
}

static void suspend_rendering(void);

/* The texture's pixels from guest memory into its image (through the upload ring) */
static int upload_texture(Texture *t, const TextureDesc *d, u64 g, u32 levels, u32 layers) {
    VkCommandBuffer cb = vk_cmd();
    suspend_rendering();
    use(&t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    flush_barriers(cb);
    if (d->type == TIC_3D) {
        u32 rowb = (d->width + d->bw - 1) / d->bw * d->bytes, rows = (d->height + d->bh - 1) / d->bh;
        u8 *at;
        VkBuffer buf;
        VkDeviceSize off = ring_alloc_for(RING_TEX, (VkDeviceSize)rowb * rows * d->depth + 64, 16, &at, &buf);
        if (!at) return 0;
        texture_read_3d(d, g, at);
        VkBufferImageCopy copy = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {d->width, d->height, d->depth}};
        vkCmdCopyBufferToImage(vk_cmd(), buf, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else
        for (u32 layer = 0; layer < layers; layer++)
            for (u32 l = 0; l < levels; l++) {
                u32 w = (d->width >> l) ? d->width >> l : 1, h = (d->height >> l) ? d->height >> l : 1;
                u32 bw = (w + d->bw - 1) / d->bw, bh = (h + d->bh - 1) / d->bh;
                u8 *at;
                VkBuffer buf;
                VkDeviceSize off = ring_alloc_for(RING_TEX, (VkDeviceSize)bw * bh * d->bytes + 64, 16, &at, &buf);
                if (!at) return 0;
                texture_read(d, g, layer, l, at);
                texture_bytes += (u64)bw * bh * d->bytes;
                /* (block-compressed: whole blocks, as far as the level's own size - the image's level 0 is whole blocks) */
                u32 iw = (t->img.width >> l) ? t->img.width >> l : 1, ih = (t->img.height >> l) ? t->img.height >> l : 1;
                u32 cw = bw * d->bw, ch = bh * d->bh;
                VkBufferImageCopy copy = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1}, {0, 0, 0}, {cw < iw ? cw : iw, ch < ih ? ch : ih, 1}};
                vkCmdCopyBufferToImage(vk_cmd(), buf, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
    return 1;
}

static int depth_sampling = -1;   /* depth buffers as textures (shadow maps, ...); SSBU_NO_DEPTH_SAMPLING: off */

/* The view for a texture header, as the shader's type (TEX_*) wants it; its image in *img.
 * VK_NULL_HANDLE for none (a null descriptor: reads 0). *scale: a scaled render target's factor. */
static VkImageView get_texture(const u32 *tic, u32 shader_type, float *scale, Img **img) {
    *scale = 1.0f;
    *img = NULL;
    TextureDesc d;
    int ok = tic_decode(tic, &d);
    u64 g = gpu_va_to_guest(d.address);
    if (!g) return VK_NULL_HANDLE;
    if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
    int depth_tic = d.format == 0x29 || d.format == 0x2A || d.format == 0x2B || d.format == 0x2F || d.format == 0x30 || d.format == 0x3A;
    Surface *rt = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->img.image || s->guest != g) continue;
        if (s->depth && (!depth_sampling || !depth_tic || !ok || s->width != d.width || s->height != d.height)) continue;
        if (!rt || s->used > rt->used) rt = s;
    }
    if (rt) {   /* a render target at this address: the latest, read as the header says */
        *scale = rt->scale;
        if (shader_type != TEX_2D && shader_type != TEX_ARRAY_2D) return VK_NULL_HANDLE;
        *img = &rt->img;
        return surface_sampled(rt, (int)d.srgb, shader_type == TEX_ARRAY_2D);
    }
    if (!ok || d.type == TIC_1D_BUFFER || d.bytes == 0) { warn_texture(&d); return VK_NULL_HANDLE; }
    VkFormat format = vk_format_of(d.dxgi);
    if (format == VK_FORMAT_UNDEFINED) { warn_texture(&d); return VK_NULL_HANDLE; }
    u32 layers = d.type == TIC_CUBE ? 6 : d.type == TIC_CUBE_ARRAY ? 6 * d.depth : d.type == TIC_2D_ARRAY || d.type == TIC_1D_ARRAY ? d.depth : 1;
    if (d.type == TIC_3D) layers = 1;
    u64 size = d.type == TIC_3D ? texture_3d_size(&d) : texture_layer_size(&d) * layers;
    int watch = watched(g, size);
    u64 sh = watch ? 0 : sample_hash(g, size);
    Texture *t = texture_find(g, tic), *free_slot = NULL, *lru = NULL;
    if (t && (watch ? !t->stale : t->sample_hash == sh)) goto found;
    if (t) release_texture(t);   /* contents changed: made again in place */
    else {
        for (u32 i = 0; i < ntextures; i++) {   /* a new one: a free slot, else the least recently used */
            Texture *x = &textures[i];
            if (!x->img.image) { if (!free_slot) free_slot = x; continue; }
            if (!lru || x->used < lru->used) lru = x;
        }
        t = free_slot ? free_slot : ntextures < 4096 ? &textures[ntextures++] : lru;
        if (t->img.image) release_texture(t);
        t->guest = g;
        memcpy(t->tic, tic, 32);
        texture_index_add(t);
    }
    u32 levels = d.type == TIC_3D ? 1 : d.levels, max_levels = 1;
    while ((d.width >> max_levels) || (d.height >> max_levels)) max_levels++;
    if (levels > max_levels) levels = max_levels;
    if (!levels) levels = 1;
    VkImageType type = d.type == TIC_3D ? VK_IMAGE_TYPE_3D : d.type == TIC_1D || d.type == TIC_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    VkImageCreateFlags flags = (d.type == TIC_CUBE || d.type == TIC_CUBE_ARRAY) && layers % 6 == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    u32 w = d.width, h = type == VK_IMAGE_TYPE_1D ? 1 : d.height;
    if (d.bw > 1 && ((w % 4) || (h % 4))) { w = (w + 3) & ~3u; h = (h + 3) & ~3u; }   /* (block-compressed: whole blocks at level 0) */
    u32 depth3d = d.type == TIC_3D ? d.depth : 1;
    if (!pooled_image(t, type, format, w, h, depth3d, levels, layers, flags)) {
        vk_stat_textures_made++;
        if (!make_image(&t->img, type, format, w, h, depth3d, levels, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, flags, NULL, 0)) {
            warn_texture(&d);
            return VK_NULL_HANDLE;
        }
    }
    t->type = type; t->flags = flags; t->w = w; t->h = h; t->depth = depth3d; t->levels = levels; t->layers = layers;
    t->img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    t->format = format;
    t->guest = g; t->size = size; t->watched = watch; t->stale = 0;
    memcpy(t->tic, tic, 32);
    t->sample_hash = sh;
    if (!upload_texture(t, &d, g, levels, layers)) { release_texture(t); return VK_NULL_HANDLE; }
    gfx_stat_textures++;
found:
    t->used = ++tick;
    if (!t->views[shader_type]) {
        static const VkImageViewType types[8] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                 VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
        VkImageViewType vt = types[shader_type];
        VkImageType it = d.type == TIC_3D ? VK_IMAGE_TYPE_3D : d.type == TIC_1D || d.type == TIC_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
        int fits = it == VK_IMAGE_TYPE_3D ? vt == VK_IMAGE_VIEW_TYPE_3D
                 : it == VK_IMAGE_TYPE_1D ? vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY
                 : vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                   ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && t->img.layers % 6 == 0 && t->img.layers >= 6);
        if (!fits) return VK_NULL_HANDLE;
        u32 nl = vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D ? 1 : vt == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : t->img.layers;
        t->views[shader_type] = make_view(&t->img, vt, t->format, VK_IMAGE_ASPECT_COLOR_BIT, nl);
    }
    *img = &t->img;
    return t->views[shader_type];
}

/* ---- samplers -------------------------------------------------------------------------------- */
typedef struct Sampler { u32 tsc[8]; VkSampler s; } Sampler;
static Sampler samplers[1024];
static u32 nsamplers;

static VkSamplerAddressMode wrap_mode(u32 m) {
    switch (m) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

/* The sampler for a sampler header (as gpu_d3d11.c's get_sampler). compare: the shader compares. */
static VkSampler get_sampler(const u32 *tsc) {
    for (u32 i = 0; i < nsamplers; i++) if (!memcmp(samplers[i].tsc, tsc, 32)) return samplers[i].s;
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3, aniso = (tsc[0] >> 20) & 7, cmp = (tsc[0] >> 9) & 1;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap_mode(tsc[0] & 7); si.addressModeV = wrap_mode((tsc[0] >> 3) & 7); si.addressModeW = wrap_mode((tsc[0] >> 6) & 7);
    i32 bias = (i32)(tsc[1] << 7) >> 19;   /* 13-bit signed, 8 fractional bits */
    si.mipLodBias = bias / 256.0f;
    if (aniso) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = (float)(1u << aniso); si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; }
    si.compareEnable = cmp ? VK_TRUE : VK_FALSE;
    si.compareOp = (VkCompareOp)(((tsc[0] >> 10) & 7));   /* (Maxwell's order: never, less, equal, ... as Vulkan's) */
    si.minLod = (tsc[2] & 0xFFF) / 256.0f;
    si.maxLod = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;   /* (no mipmaps: level 0 only) */
    float border[4];
    memcpy(border, &tsc[4], 16);
    VkSamplerCustomBorderColorCreateInfoEXT custom = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (vk.has.custom_border) {
        memcpy(custom.customBorderColor.float32, border, 16);
        si.pNext = &custom;
        si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else si.borderColor = border[3] > 0.5f ? (border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                              : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(vk.dev, &si, NULL, &s) != VK_SUCCESS) s = VK_NULL_HANDLE;
    if (nsamplers < 1024) { memcpy(samplers[nsamplers].tsc, tsc, 32); samplers[nsamplers++].s = s; }
    return s;
}
static VkSampler default_sampler(void) {
    static const u32 tsc[8] = {0};
    return get_sampler(tsc);
}

/* ---- the upload ring --------------------------------------------------------------------------- */
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, u8 **at, VkBuffer *buf) {
    unsigned slot = vk_slot();
    VkDeviceSize off = (slots[slot].used + align - 1) & ~(align - 1);
    if (off + size > RING_SIZE) {   /* full: this frame's work goes now, the next slot's ring is taken */
        if (size > RING_SIZE) { *at = NULL; return 0; }
        static int warned;
        if (!warned++) fprintf(stderr, "[gfx] Vulkan: the upload ring filled within a frame: its work submitted early\n");
        vk_flush(0);
        slot = vk_slot();
        off = 0;
    }
    slots[slot].used = off + size;
    *at = slots[slot].map + off;
    *buf = slots[slot].buf;
    gfx_stat_upload_bytes += size;
    return off;
}

/* Copy guest GPU memory [va, va + size) into dst (zeros where unmapped) */
static void read_gpu(u64 va, u32 size, void *dst) {
    u64 g = gpu_va_to_guest(va);
    if (!size) return;
    if (g && gpu_va_to_guest(va + size - 1) == g + size - 1) { memcpy(dst, P(void, g), size); return; }
    for (u32 done = 0; done < size;) {
        u32 n = 0x1000 - (u32)((va + done) & 0xFFF);
        if (n > size - done) n = size - done;
        u64 p = gpu_va_to_guest(va + done);
        if (p) memcpy((u8 *)dst + done, P(void, p), n);
        else memset((u8 *)dst + done, 0, n);
        done += n;
    }
}

/* Guest GPU memory as a storage buffer descriptor (16 zero bytes for none) */
static VkDescriptorBufferInfo upload_gpu(u64 va, u32 size) {
    u32 padded = (size + 15) & ~15u;
    if (!padded || !va) padded = 16;
    u8 *at;
    VkBuffer buf;
    VkDeviceSize off = ring_alloc_for(RING_GPU, padded, storage_align, &at, &buf);
    if (!at) return (VkDescriptorBufferInfo){VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    if (va && size) read_gpu(va, size, at);
    if (padded > size || !va) memset(at + (va ? size : 0), 0, padded - (va ? size : 0));
    return (VkDescriptorBufferInfo){buf, off, padded};
}

/* Uploads kept from frame to frame: a buffer uploaded once (constant buffers, vertex streams,
 * storage buffers of up to KEEP_MAX bytes) is used again, frame after frame, while neither the CPU
 * nor the command stream (gfx_stream_wrote) has written it - most of a frame's buffers are the same
 * memory as the frame before's, and on an integrated GPU sending them again takes the memory
 * bandwidth the GPU draws with. Each lives in a block of the kept arena (host-visible memory that
 * outlives the frame); one that changed is put in a new block, the old one freed once the GPU is done
 * with the last submission that used it.
 *
 * The CPU's writes are found by the write watch, asked for a buffer's pages at its first use in each
 * submission. The watch has one bit a page, reset by whoever finds it set: so what is found is noted
 * by page (page_gen), for every buffer on that page to see - not just the one that asked. Only memory
 * in the watched alias area is kept; anything else is uploaded each time. */
extern u64 gpu_stat_batches;   /* (gpu.c: submissions so far) */
#define KEEP_MAX (4u << 20)
typedef struct Block { VkBuffer buf; VkDeviceSize off; u8 *at; u32 cls; } Block;
typedef struct Kept { u64 va, guest, checked, epoch, gen, last_use; u32 size, mapped; Block block; } Kept;
#define KEPT_SETS 4096   /* (32768 buffers: a character select after a battle uses ~10000 a frame, and 8192 thrashed) */
#define KEPT_WAYS 8
static Kept kept[KEPT_SETS * KEPT_WAYS];
/* the command stream's writes: per (hashed) 64 KB block of GPU VA, the epoch of its last one */
static volatile u64 block_written[1 << 16], stream_epoch;
#define KEPT_BLOCK(block) ((u32)((u64)(block) * 0x9E3779B97F4A7C15ull >> 48))
static u64 kept_clock[KEPT_SETS * KEPT_WAYS], kept_tick;
u64 vk_stat_kept_reused;
/* (SSBU_LOG_STATS: why uploads were not kept, and their bytes) */
static u64 kept_miss[4], kept_miss_bytes[4];   /* not watched or too large, no room in the arena, written, first time */


/* The arena: blocks of a power of two from 256 bytes to KEEP_MAX, carved from 32 MB chunks (at most
 * 16), each size's freed ones listed for reuse */
#define ARENA_CHUNK (32u << 20)
#define ARENA_CHUNKS 16
#define BLOCK_CLASSES 15   /* (256 B ... 4 MB) */
static struct { VkBuffer buf; u8 *map; VkDeviceSize used; } chunks[ARENA_CHUNKS];
static u32 nchunks;
static Block *free_blocks[BLOCK_CLASSES];
static u32 nfree[BLOCK_CLASSES], free_cap[BLOCK_CLASSES];
static struct { Block block; u64 value; } *retired;   /* freed once the timeline passes value */
static u32 nretired, retired_cap;

static u32 block_class(u32 size) {
    u32 c = 0;
    while ((256u << c) < size) c++;
    return c;
}

static int block_alloc(u32 size, Block *out) {
    u32 c = block_class(size), bytes = 256u << c;
    if (nfree[c]) { *out = free_blocks[c][--nfree[c]]; return 1; }
    if (!nchunks || chunks[nchunks - 1].used + bytes > ARENA_CHUNK) {
        if (nchunks == ARENA_CHUNKS) return 0;
        VkDeviceMemory mem;
        void *map;
        chunks[nchunks].buf = vk_buffer(ARENA_CHUNK, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &mem, &map);
        chunks[nchunks].map = map;
        chunks[nchunks++].used = 0;
    }
    VkDeviceSize off = (chunks[nchunks - 1].used + bytes - 1) & ~(VkDeviceSize)(bytes - 1);   /* (aligned to its size: past any descriptor's alignment) */
    if (off + bytes > ARENA_CHUNK) return 0;
    chunks[nchunks - 1].used = off + bytes;
    *out = (Block){chunks[nchunks - 1].buf, off, chunks[nchunks - 1].map + off, c};
    return 1;
}

static void block_retire(const Block *b, u64 last_use) {
    if (!b->buf) return;
    if (nretired == retired_cap) retired = realloc(retired, sizeof *retired * (retired_cap = retired_cap * 2 + 256));
    retired[nretired].block = *b;
    retired[nretired++].value = last_use;
}

/* (at each present) the blocks the GPU is done with, free again */
static void blocks_release(void) {
    u64 done = 0;
    vkGetSemaphoreCounterValue(vk.dev, vk.timeline, &done);
    u32 k = 0;
    for (u32 i = 0; i < nretired; i++) {
        if (retired[i].value > done) { retired[k++] = retired[i]; continue; }
        Block *b = &retired[i].block;
        if (nfree[b->cls] == free_cap[b->cls]) free_blocks[b->cls] = realloc(free_blocks[b->cls], sizeof(Block) * (free_cap[b->cls] = free_cap[b->cls] * 2 + 64));
        free_blocks[b->cls][nfree[b->cls]++] = *b;
    }
    nretired = k;
}

static int stream_wrote_since(const Kept *e) {
    for (u64 b = e->va >> 16; b <= (e->va + e->size - 1) >> 16; b++)
        if (block_written[KEPT_BLOCK(b)] > e->epoch) return 1;
    return 0;
}

/* Has the CPU written [g, g + n) since generation gen? The write watch is asked once a submission
 * (checked); what it finds is noted by page for everyone. */
static int cpu_wrote_since(u64 g, u32 n, u64 gen, u64 *checked) {
    u64 lo = g & ~0xFFFull, hi = (g + n + 0xFFF) & ~0xFFFull;
    if (*checked != gpu_stat_batches) {
        void *page;
        ULONG_PTR count = 1;
        ULONG granularity;
        if (GetWriteWatch(0, (void *)lo, hi - lo, &page, &count, &granularity) == 0 && count) watch_reset(lo, hi);   /* (noting each page) */
        *checked = gpu_stat_batches;
    }
    for (u64 p = lo >> 12; p < hi >> 12; p++)
        if (page_gen[PAGE_SLOT(p)] > gen) return 1;
    return 0;
}

static VkDescriptorBufferInfo upload_kept(u64 va, u32 size) {
    u64 g = va && size ? gpu_va_to_guest(va) : 0;
    /* (a 64 KB binding may run past what is mapped: that part reads as zeros, as gpu_d3d11.c's kept_cb
     * has it - what is watched is the mapped part) */
    u32 mapped = g ? 0x1000 - (u32)(va & 0xFFF) : 0;
    while (g && mapped < size && gpu_va_to_guest(va + mapped) == g + mapped) mapped += 0x1000;
    if (mapped > size) mapped = size;
    if (!g || size > KEEP_MAX || !watched(g, mapped)) {
        kept_miss[0]++; kept_miss_bytes[0] += size;
        return upload_gpu(va, size);
    }
    u32 set = (u32)(((va >> 4) ^ size) * 0x9E3779B97F4A7C15ull >> 52) & (KEPT_SETS - 1);
    Kept *ways = &kept[set * KEPT_WAYS], *e = NULL;
    u32 lru = 0;
    for (u32 w = 0; w < KEPT_WAYS && !e; w++) {
        if (ways[w].block.buf && ways[w].va == va && ways[w].size == size && ways[w].guest == g) e = &ways[w];
        else if (kept_clock[set * KEPT_WAYS + w] < kept_clock[set * KEPT_WAYS + lru]) lru = w;
    }
    u32 padded = (size + 15) & ~15u;
    if (e && !stream_wrote_since(e) && !cpu_wrote_since(g, mapped, e->gen, &e->checked)) {
        static int verify = -1;   /* SSBU_KEPT_VERIFY (debugging): each reuse compared with memory, differences reported */
        if (verify < 0) verify = getenv("SSBU_KEPT_VERIFY") != NULL;
        if (verify && memcmp(e->block.at, P(void, g), mapped)) {
            static u32 reported;
            u32 o = 0;
            while (o + 4 <= mapped && !memcmp(e->block.at + o, P(u8, g) + o, 4)) o += 4;
            if (reported++ < 200)
                fprintf(stderr, "[kept] va %#llx (%#x bytes) +%#x: kept %08x, memory %08x (submission %llu)\n", (unsigned long long)va, mapped, o,
                        *(const u32 *)(e->block.at + o), ld32(g + o), (unsigned long long)gpu_stat_batches);
        }
        kept_clock[e - kept] = ++kept_tick;
        e->last_use = vk.submitted + 1;   /* (the submission being recorded) */
        vk_stat_kept_reused++;
        return (VkDescriptorBufferInfo){e->block.buf, e->block.off, padded};
    }
    int why = e ? 2 : 3;
    if (!e) {   /* (the least recently used of the set gives its place: its block freed once unused) */
        e = &ways[lru];
        block_retire(&e->block, e->last_use);
        memset(e, 0, sizeof *e);
        e->va = va; e->size = size; e->guest = g; e->mapped = mapped;
    }
    Block nb;
    if (!block_alloc(padded, &nb)) {   /* (the arena is full: this frame's ring) */
        block_retire(&e->block, e->last_use);
        memset(&e->block, 0, sizeof e->block);
        kept_miss[1]++; kept_miss_bytes[1] += size;
        return upload_gpu(va, size);
    }
    kept_miss[why]++; kept_miss_bytes[why] += size;
    block_retire(&e->block, e->last_use);
    e->block = nb;
    /* (the epoch and generation before the copy: a write during it counts) */
    e->epoch = __atomic_fetch_add(&stream_epoch, 0, __ATOMIC_ACQUIRE);
    if (e->checked != gpu_stat_batches) cpu_wrote_since(g, mapped, 0, &e->checked);   /* (the watch reset over it first) */
    e->gen = gen_now;
    read_gpu(va, size, nb.at);
    if (padded > size) memset(nb.at + size, 0, padded - size);
    gfx_stat_upload_bytes += size;
    e->last_use = vk.submitted + 1;
    kept_clock[e - kept] = ++kept_tick;
    return (VkDescriptorBufferInfo){nb.buf, nb.off, padded};
}

void vk_gfx_stream_wrote(u64 va, u64 bytes);

/* ---- GPU-resident buffers --------------------------------------------------------------------- */
/* As gpu_d3d11.c's: what compute programs write stays on the GPU, a buffer per guest range they
 * write (and per copy-engine destination of such data), used directly by later dispatches and by
 * draws that read inside it; a read only partly inside one gets the guest's data around it. Guest
 * memory under a region is stale; pages the CPU writes inside one (the write watch) are taken from
 * guest memory, and the partial pages at its ends compared byte for byte (gpu_d3d11.c says why).
 *
 * The GPU's own writes to buffers (dispatches, copies) are ordered by a full memory barrier: before
 * such work when anything came since the last one, and before the next draw after it. Data from the
 * CPU goes in through the upload ring and a copy, in the command buffer's order. */
typedef struct Resident {
    u64 va, guest, checked;   /* (checked: the submission it was last checked against the write watch in) */
    u64 written_at;           /* the present before the GPU last wrote it (a dispatch, a copy) */
    u32 size, cap, head, tail;
    VkBuffer buf;
    VkDeviceMemory mem;
    u8 *edge;                 /* guest memory under the first and last partial pages, as the GPU copy last took it */
} Resident;
static Resident residents[512];
static u32 nresidents;
extern u64 gfx_stat_residents, gfx_stat_resident_pages, gfx_stat_gpu_waits, gfx_stat_composed, gfx_stat_presented;
static int gpu_wrote, gpu_read;   /* since the last buffer barrier: buffers written by the GPU; draws or dispatches */
/* Since then, as well: whether all of it was dispatches (copies and draws are not followed range by
 * range), and those dispatches' storage buffers - so that a dispatch after others that touch other
 * buffers (the fighters' skinning, one model after another) needs no barrier between them */
static int untracked;
static int ring_copied;   /* resident data copied into the ring (resident_patch) since the last barrier */
static struct { VkBuffer buf; VkDeviceSize lo, hi; int write; } accessed[64];
static u32 naccessed;
u64 vk_stat_buffer_barriers, vk_stat_compute_barriers;

static void buffer_barrier(void) {
    suspend_rendering();
    VkCommandBuffer cb = vk_cmd();
    flush_barriers(cb);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    gpu_wrote = gpu_read = untracked = ring_copied = 0;
    naccessed = 0;
    vk_stat_buffer_barriers++;
}
/* Between dispatches that touch the same buffers, with only dispatches since the last barrier: the
 * compute stage's writes before its next reads and writes, the graphics work left running. Draws
 * still get the full barrier (gpu_wrote stays). */
static void compute_barrier(void) {
    VkCommandBuffer cb = vk_cmd();
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    naccessed = 0;
    vk_stat_compute_barriers++;
}
/* Copies into fresh parts of the ring, before the draw or dispatch that reads them */
static void copy_barrier(void) {
    suspend_rendering();
    VkCommandBuffer cb = vk_cmd();
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    ring_copied = 0;
}
/* Before the GPU writes buffers that earlier work may use */
static void gpu_write_begin(void) {
    suspend_rendering();
    if (gpu_wrote || gpu_read) buffer_barrier();
}
/* The GPU's writes, then the CPU reads them: submitted and waited for */
static void gpu_read_back(void) {
    suspend_rendering();
    VkCommandBuffer cb = vk_cmd();
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    gfx_stat_gpu_waits++;
    vk_flush(1);
}

static VkBuffer device_buffer(u32 size, VkDeviceMemory *mem) {
    return vk_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem, NULL);
}

/* n bytes into dst at off: through the ring, a copy in the command buffer */
static void buf_write(VkBuffer dst, VkDeviceSize off, const void *data, u64 n) {
    suspend_rendering();
    while (n) {
        u32 part = n > (16u << 20) ? 16u << 20 : (u32)n;
        u8 *at;
        VkBuffer src;
        VkDeviceSize so = ring_alloc_for(RING_COPY, part, 16, &at, &src);
        if (!at) return;
        memcpy(at, data, part);
        VkBufferCopy r = {so, off, part};
        vkCmdCopyBuffer(vk_cmd(), src, dst, 1, &r);
        data = (const u8 *)data + part;
        off += part;
        n -= part;
    }
    gpu_wrote = untracked = 1;
}

static void resident_drop(Resident *r) {
    bury_buffer(r->buf, r->mem);
    free(r->edge);
    *r = residents[--nresidents];
    memset(&residents[nresidents], 0, sizeof *residents);
}

static void resident_snapshot(Resident *r) {
    u32 head = (u32)(0x1000 - (r->guest & 0xFFF)) & 0xFFF;   /* up to the first page boundary */
    if (head > r->size) head = r->size;
    u32 tail = (u32)((r->guest + r->size) & 0xFFF);          /* from the last one */
    if (tail > r->size - head) tail = r->size - head;
    r->head = head;
    r->tail = tail;
    r->edge = realloc(r->edge, head + tail + 1);
    memcpy(r->edge, P(void, r->guest), head);
    memcpy(r->edge + head, P(void, r->guest + r->size - tail), tail);
}

/* n bytes of guest memory at g, or 0 if they are not there any more (the guest can unmap memory
 * under a region): process_vm_readv fails with EFAULT where a read would fault */
static int read_guarded(void *dst, u64 g, u32 n) {
    struct iovec to = {dst, n}, from = {P(void, g), n};
    ssize_t got = process_vm_readv(getpid(), &to, 1, &from, 1, 0);
    if (got == (ssize_t)n) return 1;
    if (got >= 0 || errno == EFAULT) return 0;
    memcpy(dst, P(void, g), n);   /* (where the call isn't allowed, as in some sandboxes: a plain read) */
    return 1;
}

/* A partial page's bytes inside the region: taken from guest memory if they changed. 0 if that
 * memory is gone. */
static int resident_take_edge(Resident *r, u64 g, u8 *was, u32 n) {
    u8 now[0x1000];
    if (!n) return 1;
    if (!read_guarded(now, g, n)) return 0;
    if (!memcmp(was, now, n)) return 1;
    memcpy(was, now, n);
    gpu_write_begin();
    buf_write(r->buf, g - r->guest, was, n);
    gfx_stat_resident_pages++;
    return 1;
}

static void resident_watch_reset(Resident *r) {
    u64 lo = (r->guest + 0xFFF) & ~0xFFFull, hi = (r->guest + r->size) & ~0xFFFull;
    if (lo < hi) watch_reset(lo, hi);
}

/* What the CPU wrote inside the region since its GPU copy took guest memory: its whole pages the
 * write watch names, and the partial pages at its ends where their bytes changed. Checked when a
 * draw or dispatch uses the region (once a submission), and when it is flushed or written. */
static void resident_sync(Resident *r, int always) {
    static void *pages[16384];
    static int no_pages = -1;   /* debug: leave resident regions alone */
    if (no_pages < 0) no_pages = getenv("SSBU_NO_RESIDENT_PAGES") != NULL;
    if (no_pages || (!always && r->checked == gpu_stat_batches) || !r->edge || !watched(r->guest, r->size)) return;
    r->checked = gpu_stat_batches;
    if (!resident_take_edge(r, r->guest, r->edge, r->head) || !resident_take_edge(r, r->guest + r->size - r->tail, r->edge + r->head, r->tail))
        return;
    u64 lo = (r->guest + 0xFFF) & ~0xFFFull, hi = (r->guest + r->size) & ~0xFFFull;
    if (lo >= hi || !frame_range_dirty(lo, hi)) return;   /* (the frame's list says nothing was written there) */
    ULONG_PTR count = 16384;
    ULONG granularity;
    if (GetWriteWatch(0, (void *)lo, hi - lo, pages, &count, &granularity) != 0 || !count) return;
    watch_reset(lo, hi);   /* (before taking them: what the CPU writes meanwhile is seen next time) */
    gpu_write_begin();
    for (ULONG_PTR k = 0; k < count; k++) {
        u64 pg = (u64)pages[k];
        buf_write(r->buf, pg - r->guest, P(void, pg), granularity);
        gfx_stat_resident_pages++;
    }
}

/* The watched resident regions' whole pages, added to the ranges note_writes asks about */
static void residents_ranges(WatchRange *out, u32 *n, u32 cap) {
    for (u32 i = 0; i < nresidents && *n < cap; i++) {
        Resident *r = &residents[i];
        if (!r->edge || !watched(r->guest, r->size)) continue;
        u64 lo = (r->guest + 0xFFF) & ~0xFFFull, hi = (r->guest + r->size) & ~0xFFFull;
        if (lo < hi) out[(*n)++] = (WatchRange){lo, hi};
    }
}

/* Regions no dispatch (or copy) has written for 2 s - a battle's, once it is over - given back to
 * guest memory and dropped: the game reuses that memory for other data, and every draw reading
 * near a region it covers went the composed way (a copy, barriers, the rendering broken: hundreds
 * a frame, the next battle at 40 FPS). What the CPU wrote in them is taken first, so what goes back
 * is memory as the GPU and the CPU left it; all of them read back together, one wait. Every 0.5 s. */
u64 vk_stat_residents_retired;
static void residents_retire(void) {
    static u64 last;
    if (gfx_stat_presented - last < 30) return;
    last = gfx_stat_presented;
    static struct { u32 index; u8 *at; u64 lo, hi; } back[64];
    u32 nback = 0;
    u64 bytes = 0;
    for (u32 i = 0; i < nresidents && nback < 64; i++) {
        Resident *r = &residents[i];
        if (gfx_stat_presented - r->written_at < 120 || bytes + r->cap > (64u << 20)) continue;
        resident_sync(r, 1);
        gpu_write_begin();
        VkBuffer rb;
        u8 *at;
        VkDeviceSize ro = ring_alloc(r->cap, 16, &at, &rb);
        if (!at) break;
        VkBufferCopy c = {0, ro, r->cap};
        vkCmdCopyBuffer(vk_cmd(), r->buf, rb, 1, &c);
        back[nback++] = (typeof(back[0])){i, at, r->va, r->va + r->size};
        bytes += r->cap;
    }
    if (!nback) return;
    gpu_read_back();
    for (u32 k = nback; k-- > 0;) {   /* (from the last: resident_drop moves the last region into the slot it frees) */
        for (u64 a = back[k].lo; a < back[k].hi;) {   /* (a page at a time; only what differs written) */
            u64 n = 0x1000 - (a & 0xFFF), g = gpu_va_to_guest(a);
            if (n > back[k].hi - a) n = back[k].hi - a;
            const u8 *src = back[k].at + (a - back[k].lo);
            if (g && memcmp(P(void, g), src, n)) memcpy(P(void, g), src, n);
            a += n;
        }
        Resident *r = &residents[back[k].index];
        if (watched(r->guest, r->size)) watch_reset(r->guest, r->guest + r->size);   /* (our writes are no CPU writes for the kept uploads) */
        resident_drop(r);
        vk_stat_residents_retired++;
    }
}

/* GPU-resident buffers the CPU has written to (those no draw has checked yet): once a frame */
static void residents_sync_all(void) {
    for (u32 i = 0; i < nresidents; i++) resident_sync(&residents[i], 1);
}

static Resident *resident_covering(u64 lo, u64 hi) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va <= lo && hi <= residents[i].va + residents[i].size) { resident_sync(&residents[i], 0); return &residents[i]; }
    return NULL;
}

static Resident *resident_overlapping(u64 va, u32 size) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va < va + size && va < residents[i].va + residents[i].size) { resident_sync(&residents[i], 0); return &residents[i]; }
    return NULL;
}

/* [va, va + bytes) inside r as a storage buffer: the descriptor starts at an aligned offset, *delta
 * bytes before va (DrawParams tells the shader) */
static VkDescriptorBufferInfo resident_desc(const Resident *r, u64 va, u32 bytes, u32 *delta) {
    u64 at = (va - r->va) & ~(storage_align - 1), end = (va + bytes - r->va + 3) & ~3ull;
    if (end > r->cap) end = r->cap;
    *delta = (u32)(va - r->va - at);
    return (VkDescriptorBufferInfo){r->buf, at, end - at};
}

static Resident *resident_create(u64 va, u32 size) {
    if (nresidents == 512) resident_drop(&residents[0]);
    Resident *r = &residents[nresidents];
    memset(r, 0, sizeof *r);
    r->cap = (size + 15) & ~15u;
    r->buf = device_buffer(r->cap, &r->mem);
    u8 *init = calloc(1, r->cap);
    read_gpu(va, size, init);   /* what the CPU put there */
    buf_write(r->buf, 0, init, r->cap);
    free(init);
    r->va = va;
    r->size = size;
    r->guest = gpu_va_to_guest(va);
    if (watched(r->guest, r->size)) resident_snapshot(r);
    r->written_at = gfx_stat_presented;
    nresidents++;
    gfx_stat_residents++;
    vk_stat_residents_made++;
    return r;
}

/* The region holding [va, va + size), made (or widened) as needed. Regions never overlap: a range
 * that overlaps some becomes one region over all of them, the CPU's data underneath and each old
 * region's GPU data copied over it. */
static Resident *resident_acquire(u64 va, u32 size) {
    u64 lo = va, hi = va + size;
    int overlaps = 0;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        if (r->va <= va && va + size <= r->va + r->size) return r;
        if (r->va < hi && va < r->va + r->size) {
            overlaps = 1;
            if (r->va < lo) lo = r->va;
            if (r->va + r->size > hi) hi = r->va + r->size;
        }
    }
    if (hi - lo > (256u << 20)) return NULL;
    Resident *n = resident_create(lo, (u32)(hi - lo));
    if (overlaps) {
        Resident merged = *n;   /* resident_drop moves entries around */
        n->buf = VK_NULL_HANDLE; n->mem = VK_NULL_HANDLE; n->edge = NULL;
        resident_drop(n);
        gpu_write_begin();   /* (after the guest's data went in, and the old regions' last writes) */
        for (u32 i = 0; i < nresidents;) {
            Resident *r = &residents[i];
            if (r->va >= merged.va && r->va + r->size <= merged.va + merged.size && r->buf != merged.buf) {
                VkBufferCopy c = {0, r->va - merged.va, (r->size + 3) & ~3u};
                if (c.dstOffset + c.size > merged.cap) c.size = merged.cap - c.dstOffset;
                vkCmdCopyBuffer(vk_cmd(), r->buf, merged.buf, 1, &c);
                resident_drop(r);
            } else i++;
        }
        gpu_wrote = untracked = 1;
        residents[nresidents++] = merged;
        n = &residents[nresidents - 1];
    }
    if (watched(n->guest, n->size)) resident_watch_reset(n);
    return n;
}

/* dst (at off) holds guest memory's [va, va + size): what resident regions hold of it, copied over */
static void resident_patch(VkBuffer dst, VkDeviceSize off, u64 va, u64 size) {
    int any = 0;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        if (r->va < va + size && va < r->va + r->size) { resident_sync(r, 0); any = 1; }
    }
    if (!any) return;
    /* (dst is a fresh part of the ring: only the regions' last writes are waited for, and what reads
     * it next waits for the copy alone - copy_barrier) */
    suspend_rendering();
    if (gpu_wrote) buffer_barrier();
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + size < r->va + r->size ? va + size : r->va + r->size;
        if (lo >= hi) continue;
        VkBufferCopy c = {lo - r->va, off + (lo - va), hi - lo};
        vkCmdCopyBuffer(vk_cmd(), r->buf, dst, 1, &c);
    }
    ring_copied = 1;
}

/* A range only partly held by resident regions, as the GPU sees it: the guest's data with the
 * regions' parts over it, in the ring. Regions are never widened for a read: what lies around them
 * is the CPU's, and it keeps changing in guest memory. */
static VkDescriptorBufferInfo upload_composed(u64 va, u32 size) {
    VkDescriptorBufferInfo d = upload_gpu(va, size);
    gfx_stat_composed++;
    if (d.buffer) resident_patch(d.buffer, d.offset, va, size);
    return d;
}

/* Bring the GPU's data for [va, va + bytes) back to guest memory (waits for the GPU) */
void vk_gfx_resident_flush(u64 va, u64 bytes) {
    vk_enter();
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);   /* (what the CPU wrote before, first: the write watch is reset below) */
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        if (hi > r->va + r->cap) hi = r->va + r->cap;
        for (u64 at = lo; at < hi;) {   /* (through the ring, in parts it holds) */
            u32 part = hi - at > (32u << 20) ? 32u << 20 : (u32)(hi - at);
            gpu_write_begin();
            u8 *map;
            VkBuffer rb;
            VkDeviceSize ro = ring_alloc_for(RING_READBACK, part, 16, &map, &rb);
            if (!map) break;
            VkBufferCopy c = {at - r->va, ro, part};
            vkCmdCopyBuffer(vk_cmd(), r->buf, rb, 1, &c);
            gpu_read_back();
            for (u32 k = 0; k < part; k += 4) {
                u64 g = gpu_va_to_guest(at + k);
                u32 v;
                memcpy(&v, map + k, 4);
                if (g) st32(g, v);
            }
            at += part;
        }
        if (watched(r->guest, r->size)) { resident_watch_reset(r); resident_snapshot(r); }
    }
    vk_leave();
}

/* Guest memory in [va, va + bytes) changed under a resident region: the GPU's copy is updated */
void vk_gfx_resident_written(u64 va, u64 bytes) {
    if (!bytes || bytes > (256u << 20)) return;
    vk_enter();
    vk_gfx_stream_wrote(va, bytes);   /* (and kept uploads there are taken again) */
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        if (hi > r->va + r->cap) hi = r->va + r->cap;
        u8 *data = malloc((size_t)(hi - lo));
        read_gpu(lo, (u32)(hi - lo), data);
        gpu_write_begin();
        buf_write(r->buf, lo - r->va, data, hi - lo);
        free(data);
        if (watched(r->guest, r->size)) { resident_watch_reset(r); resident_snapshot(r); }
    }
    vk_leave();
}

/* The copy engine and resident data: copy [src, src + bytes) to dst on the GPU if the source is
 * such data (1 if done) */
int vk_gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes) {
    if (!bytes || bytes > (256u << 20)) return 0;
    vk_enter();
    Resident *src = resident_overlapping(src_va, (u32)bytes);
    if (!src || src_va < src->va || src_va + bytes > src->va + src->size) {
        if (src) vk_gfx_resident_flush(src_va, bytes);   /* partly resident: through guest memory */
        vk_leave();
        return 0;
    }
    Resident *dst = resident_acquire(dst_va, (u32)bytes);   /* may merge regions: look the source up again */
    if (dst) dst->written_at = gfx_stat_presented;
    src = resident_covering(src_va, src_va + bytes);
    if (!dst || !src) { vk_leave(); return 0; }
    gpu_write_begin();
    VkBufferCopy c = {src_va - src->va, dst_va - dst->va, bytes};
    if (dst != src || c.srcOffset + bytes <= c.dstOffset || c.dstOffset + bytes <= c.srcOffset)
        vkCmdCopyBuffer(vk_cmd(), src->buf, dst->buf, 1, &c);
    else {   /* overlapping within one buffer: through a scratch one */
        static VkBuffer tmp;
        static VkDeviceMemory tmp_mem;
        static u32 tmp_cap;
        if (bytes > tmp_cap) {
            bury_buffer(tmp, tmp_mem);
            tmp_cap = (u32)((bytes + 0xFFFF) & ~0xFFFFull);
            tmp = device_buffer(tmp_cap, &tmp_mem);
        }
        VkBufferCopy a = {c.srcOffset, 0, bytes}, b = {0, c.dstOffset, bytes};
        vkCmdCopyBuffer(vk_cmd(), src->buf, tmp, 1, &a);
        gpu_wrote = untracked = 1;
        buffer_barrier();
        vkCmdCopyBuffer(vk_cmd(), tmp, dst->buf, 1, &b);
    }
    gpu_wrote = untracked = 1;
    vk_leave();
    return 1;
}

/* Storage buffers a compute program only reads (the fighters' bind pose and skin weights): kept on
 * the GPU while guest memory under them goes unwritten (the write watch), not uploaded for every
 * dispatch */
typedef struct InputBuf { u64 va, guest, used; u32 size; VkBuffer buf; VkDeviceMemory mem; } InputBuf;
static InputBuf inputs[128];
static u64 input_clock;

static int input_buffer(u64 va, u32 size, VkDescriptorBufferInfo *out) {
    u64 g = gpu_va_to_guest(va);
    if (!g || !size || !watched(g, size) || gpu_va_to_guest(va + size - 1) != g + size - 1) return 0;   /* one watched piece only */
    InputBuf *b = NULL, *lru = &inputs[0];
    for (u32 i = 0; i < 128 && !b; i++) {
        if (inputs[i].buf && inputs[i].va == va && inputs[i].size == size && inputs[i].guest == g) b = &inputs[i];
        else if (inputs[i].used < lru->used) lru = &inputs[i];
    }
    u32 cap = (size + 15) & ~15u;
    if (b) {
        void *page;
        ULONG_PTR count = 1;
        ULONG granularity;
        if (GetWriteWatch(0, (void *)g, size, &page, &count, &granularity) == 0 && count) {   /* written since: taken again */
            watch_reset(g, g + size);
            gpu_write_begin();
            buf_write(b->buf, 0, P(void, g), size);
        }
    } else {
        b = lru;
        vk_stat_inputs_made++;
        bury_buffer(b->buf, b->mem);
        memset(b, 0, sizeof *b);
        b->buf = device_buffer(cap, &b->mem);
        u8 *init = calloc(1, cap);
        read_gpu(va, size, init);
        buf_write(b->buf, 0, init, cap);
        free(init);
        b->va = va; b->guest = g; b->size = size;
        watch_reset(g, g + size);
    }
    b->used = ++input_clock;
    *out = (VkDescriptorBufferInfo){b->buf, 0, cap};
    return 1;
}

/* ---- shaders ---------------------------------------------------------------------------------- */
typedef struct Shader {
    u64 hash, table_hash;
    u32 vs_outputs, local[3], shared;   /* (compute: the workgroup's size and shared memory) */
    int stage, failed;
    u32 learn_runs, written_mask;       /* (compute: runs learning which buffers it writes; those it does) */
    int logged;
    VkShaderModule module;
    VkPipeline compute;
    VkPipeline lib[2];                  /* (graphics pipeline libraries: vertex fill and lines, or pixel) */
    volatile int lib_state[2];          /* (0 not asked for, 1 compiling, 2 ready, 3 failed) */
    u64 vin;                            /* (vertex: its attributes as vertex inputs, shader_spirv_ex's; 0 from the streams) */
    int from_cache;                     /* (read from the disk cache) */
    struct Shader *variant[4];          /* (vertex, from the streams: its vertex-input versions) */
    char name[64];
    ShaderInfo info;
} Shader;
static Shader **shader_cache;
static u32 shader_count, shader_cap;

typedef struct CbufSource { const CbBinding *cb; } CbufSource;
static u32 read_cbuf_word(void *user, u32 bank, u32 offset) {
    const CbufSource *src = user;
    u32 v = 0;
    if (bank < 18 && src->cb[bank].address && offset + 4 <= src->cb[bank].size) read_gpu(src->cb[bank].address + offset, 4, &v);
    return v;
}
static u64 table_hash(const ShaderInfo *info, CbufSource *src) {
    u64 h = 0;
    for (u32 t = 0; t < info->ntables; t++)
        for (u32 e = 0; e < info->table[t].count; e++)
            h = (h ^ read_cbuf_word(src, info->table[t].bank, info->table[t].offset + 4 * e)) * 1099511628211ull;
    return h;
}

/* Programs by where they are (as gpu_d3d11.c's): each address remembers the shader it last held,
 * recognised again by a few of its words */
struct Shader;
static void cache_add(const struct Shader *s, const u32 *code, u32 words);
static u64 vk_stat_translate_ns, vk_stat_module_ns, vk_stat_pipeline_ns, vk_stat_translated, vk_stat_pipelines_made;   /* (SSBU_LOG_STATS) */
static u64 vk_stat_libraries_made, vk_stat_link_ns, vk_stat_optimize_ns;
typedef struct ShaderSlot { u64 g, key, print; u32 size; Shader *s; } ShaderSlot;
#define SHADER_SLOTS 16384
static ShaderSlot shader_slots[SHADER_SLOTS];
static u64 fingerprint(int stage, const u8 *p, u32 size) {   /* (a compute program's code only) */
    if (stage == SHADER_COMPUTE) { p += 0x50; size -= 0x50; }
    u64 h = size;
    for (u32 i = 0; i < 32; i++) {
        u64 v;
        memcpy(&v, p + (((u64)size - 8) * i / 31 & ~7ull), 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

/* The program at p translated into s (its key filled in), its module made and its SPIR-V in the
 * disk cache */
static void translate(Shader *s, const u8 *p, u32 size, CbufSource *src) {
    u32 words = 0;
    u64 t0 = rt_now_ns();
    u32 *code = shader_spirv_ex(p, size, s->stage, s->vs_outputs, s->stage == SHADER_COMPUTE ? s->local : NULL, s->shared, &s->info, read_cbuf_word,
                                src, 0, s->vin, &words);
    s->table_hash = table_hash(&s->info, src);
    if (s->info.unsupported[0]) fprintf(stderr, "[gfx] %s: %s\n", s->name, s->info.unsupported);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)words * 4, code};
    u64 t1 = rt_now_ns();
    vk_stat_translate_ns += t1 - t0;
    vk_stat_translated++;
    int made = code && vkCreateShaderModule(vk.dev, &mi, NULL, &s->module) == VK_SUCCESS;
    vk_stat_module_ns += rt_now_ns() - t1;
    if (!made) {
        s->failed = 1;
        gfx_stat_shader_failures++;
        fprintf(stderr, "[gfx] %s: the device refused its SPIR-V\n", s->name);
    } else {
        gfx_stat_shaders++;
        cache_add(s, code, words);
    }
    const char *dump = getenv("SSBU_DUMP_SPIRV");   /* debug: <dir>/<name>.spv */
    if (dump && code) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.spv", dump, s->name);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(code, 4, words, f); fclose(f); }
    }
    free(code);
}

/* The shader for the program at guest address g, translated now if it is new; NULL if it fails. */
static Shader *get_shader(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, const u32 *local, u32 shared) {
    /* a compute program has no header: it is translated as if it had one (never read), as gpu_d3d11.c does */
    const u8 *p = stage == SHADER_COMPUTE ? P(u8, g) - 0x50 : P(u8, g);
    static const u32 no_local[3];
    if (!local) local = no_local;
    u64 key = (u64)stage | (u64)vs_outputs << 4 | (u64)(local[0] ^ local[1] << 10 ^ local[2] << 20 ^ shared << 5) << 36;
#define SAME_COMPUTE(s) (!memcmp((s)->local, local, sizeof (s)->local) && (s)->shared == shared)
    ShaderSlot *slot = &shader_slots[((g >> 4) * 0x9E3779B97F4A7C15ull ^ key) >> 50 & (SHADER_SLOTS - 1)];
    CbufSource src = {stage_cb};
    if (slot->s && slot->g == g && slot->key == key && slot->print == fingerprint(stage, p, slot->size) && SAME_COMPUTE(slot->s)) {
        Shader *s = slot->s;
        if (!s->info.ntables || s->table_hash == table_hash(&s->info, &src)) return s->failed ? NULL : s;
    }
    u32 size = shader_size(p, 0x40000);
    u32 skip = stage == SHADER_COMPUTE ? 0x50 : 0;
    u64 h = 1469598103934665603ull;   /* (gpu_d3d11.c's names: FNV-1a over the program, a compute one's code and launch shape) */
    for (u32 i = skip; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
    if (stage == SHADER_COMPUTE) h = (h ^ ((u64)local[0] | (u64)local[1] << 16 | (u64)local[2] << 32 | (u64)shared << 40)) * 1099511628211ull;
    for (u32 i = 0; i < shader_count; i++) {
        Shader *s = shader_cache[i];
        if (s->hash != h || s->stage != stage || s->vin || (stage == SHADER_PIXEL && s->vs_outputs != vs_outputs) || !SAME_COMPUTE(s)) continue;
        if (s->info.ntables && s->table_hash != table_hash(&s->info, &src)) continue;
        *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s};
        return s->failed ? NULL : s;
    }
    if (shader_count == shader_cap) shader_cache = realloc(shader_cache, sizeof *shader_cache * (shader_cap = shader_cap * 2 + 64));
    Shader *s = calloc(1, sizeof *s);
    shader_cache[shader_count++] = s;
    s->hash = h; s->stage = stage; s->vs_outputs = vs_outputs;
    memcpy(s->local, local, sizeof s->local);
    s->shared = shared;
    *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s};
    snprintf(s->name, sizeof s->name, "%s_%016llx", stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", (unsigned long long)h);
    translate(s, p, size, &src);
    return s->failed ? NULL : s;
#undef SAME_COMPUTE
}

/* The vertex-input version of vertex shader base (the program at g): attributes of these classes
 * fetched by the device (vin, shader_spirv_ex's). NULL if it fails. */
/* Every vertex-input version, by its program, table hash and classes (a search of the whole cache
 * for one a base's recent ones lack was a scan of thousands of shaders, draw after draw) */
static Shader **variants;
static u64 vk_stat_variant_lookups;   /* (SSBU_LOG_STATS: a base's recent versions missed) */
static u32 nvariants, variants_cap;
static u64 variant_hash(u64 hash, u64 table_hash, u64 vin) { return (hash ^ table_hash * 31 ^ vin * 0x9E3779B97F4A7C15ull) * 1099511628211ull; }
static Shader *variant_find(u64 hash, u64 table_hash, u64 vin) {
    if (!variants_cap) return NULL;
    for (u32 i = (u32)variant_hash(hash, table_hash, vin) & (variants_cap - 1); variants[i]; i = (i + 1) & (variants_cap - 1))
        if (variants[i]->hash == hash && variants[i]->table_hash == table_hash && variants[i]->vin == vin) return variants[i];
    return NULL;
}
static void variant_add(Shader *s) {
    if (nvariants * 2 >= variants_cap) {   /* (grown and placed again) */
        u32 cap = variants_cap ? variants_cap * 2 : 1024;
        Shader **t = calloc(cap, sizeof *t);
        for (u32 i = 0; i < variants_cap; i++) {
            if (!variants[i]) continue;
            u32 k = (u32)variant_hash(variants[i]->hash, variants[i]->table_hash, variants[i]->vin) & (cap - 1);
            while (t[k]) k = (k + 1) & (cap - 1);
            t[k] = variants[i];
        }
        free(variants);
        variants = t;
        variants_cap = cap;
    }
    u32 k = (u32)variant_hash(s->hash, s->table_hash, s->vin) & (variants_cap - 1);
    while (variants[k]) k = (k + 1) & (variants_cap - 1);
    variants[k] = s;
    nvariants++;
}

static Shader *vertex_variant(Shader *base, u64 g, const CbBinding *stage_cb, u64 vin) {
    for (u32 i = 0; i < 4; i++)
        if (base->variant[i] && base->variant[i]->vin == vin) return base->variant[i]->failed ? NULL : base->variant[i];
    vk_stat_variant_lookups++;
    Shader *s = variant_find(base->hash, base->table_hash, vin);   /* (made before, or from the disk cache) */
    if (!s) {
        const u8 *p = P(u8, g);
        CbufSource src = {stage_cb};
        if (shader_count == shader_cap) shader_cache = realloc(shader_cache, sizeof *shader_cache * (shader_cap = shader_cap * 2 + 64));
        s = calloc(1, sizeof *s);
        shader_cache[shader_count++] = s;
        s->hash = base->hash; s->stage = SHADER_VERTEX; s->vin = vin;
        memcpy(s->name, base->name, sizeof s->name);
        translate(s, p, shader_size(p, 0x40000), &src);
        s->table_hash = base->table_hash;   /* (as found by: the base's) */
        variant_add(s);
    }
    memmove(base->variant + 1, base->variant, sizeof base->variant - sizeof *base->variant);   /* (the latest first) */
    base->variant[0] = s;
    return s->failed ? NULL : s;
}

/* ---- fixed-function state ------------------------------------------------------------------------ */
static VkCompareOp compare_op(u32 f) {
    if (f >= 0x200 && f <= 0x207) return (VkCompareOp)(f - 0x200);   /* GL-style values */
    if (f >= 1 && f <= 8) return (VkCompareOp)(f - 1);                /* D3D-style */
    return VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp stencil_op(u32 op) {
    switch (op) {
    case 1: case 0x1E00: return VK_STENCIL_OP_KEEP;
    case 2: case 0: return VK_STENCIL_OP_ZERO;
    case 3: case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 4: case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: case 0x150A: return VK_STENCIL_OP_INVERT;
    case 7: case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}
static VkBlendFactor blend_factor(u32 f) {
    switch (f) {
    case 0x1: case 0x4000: return VK_BLEND_FACTOR_ZERO;
    case 0x2: case 0x4001: return VK_BLEND_FACTOR_ONE;
    case 0x3: case 0x4300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x4: case 0x4301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: case 0x4302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x6: case 0x4303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x7: case 0x4304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x8: case 0x4305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: case 0x4306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: case 0x4307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: case 0x4308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0xE: case 0xC001: case 0xC003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0xF: case 0xC002: case 0xC004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x10: case 0xC900: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: case 0xC901: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: case 0xC902: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: case 0xC903: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendFactor alpha_factor(VkBlendFactor b) {   /* (colour factors for alpha: their alpha, as Direct3D has it) */
    switch (b) {
    case VK_BLEND_FACTOR_SRC_COLOR: return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR: return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case VK_BLEND_FACTOR_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return b;
    }
}
static VkBlendOp blend_op(u32 op) {
    switch (op) {
    case 2: case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 3: case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: case 0x8007: return VK_BLEND_OP_MIN;
    case 5: case 0x8008: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}
static int topology(u32 t, VkPrimitiveTopology *out) {
    switch (t) {
    case 0: *out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return 1;
    case 1: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return 1;
    case 3: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return 1;
    case 4: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return 1;
    case 5: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return 1;
    case 10: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return 1;
    case 11: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; return 1;
    case 12: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return 1;
    case 13: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; return 1;
    }
    return 0;
}

/* ---- pipelines ------------------------------------------------------------------------------------ */
typedef struct BlendTarget { u8 enable, mask, src, dst, op, asrc, adst, aop; } BlendTarget;
typedef struct PipelineKey {
    const struct Shader *vs, *ps;      /* (the shaders themselves: a pixel shader's module is freed once its library is made) */
    VkFormat color[8], depth;
    u32 ncolor, topology, polygon;
    BlendTarget blend[8];
} PipelineKey;
/* A pipeline: made at its key's first draw. With graphics pipeline libraries (D10) it is linked from
 * libraries - each shader's compiled once, on the compiler threads, the fixed parts on this one - at
 * once (no link-time optimization), and the optimized one is made on the compiler threads and
 * used from when it is ready; a draw whose shaders' libraries are not ready yet is skipped, as
 * gpu_d3d11.c skips one whose shaders are compiling. Without them, the whole pipeline is made here
 * at its first draw. */
typedef struct Pipe { PipelineKey key; VkPipeline fast; VkPipeline volatile optimized; VkPipeline libs[4]; } Pipe;
typedef struct PipelineEntry { PipelineKey key; Pipe *pipe; } PipelineEntry;
static PipelineEntry *pipelines;
static u32 npipelines, pipelines_cap;
u64 vk_stat_pipelines, vk_stat_compiling_skips;

static u64 key_hash(const PipelineKey *k) {
    u64 h = 1469598103934665603ull;
    const u8 *p = (const u8 *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static VkDynamicState DYNAMIC[32] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                         VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                         VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                         VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                         VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                                         /* (with extended dynamic state 3: blending and write masks too) */
                                         VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
#define DYNAMIC_FIXED 14   /* (the ones every device has) */
static VkPipelineDynamicStateCreateInfo DYNAMIC_INFO = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, DYNAMIC_FIXED, DYNAMIC};
static void dynamic_init(void) {
    DYNAMIC_INFO.dynamicStateCount = DYNAMIC_FIXED + (vk.has.eds3 ? 3 : 0);
    if (vk.has.vertex_input) DYNAMIC[DYNAMIC_INFO.dynamicStateCount++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;   /* (the attributes' formats, as each draw has them) */
}

/* The fixed state's pieces, as each kind of pipeline (or library) takes them */
static int topology_strip(VkPrimitiveTopology topo) {
    return topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
           topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}
static const VkPipelineVertexInputStateCreateInfo NO_VERTEX_INPUT = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};   /* (the shaders read the streams) */
static const VkPipelineViewportStateCreateInfo ONE_VIEWPORT = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
static const VkPipelineMultisampleStateCreateInfo ONE_SAMPLE = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
static VkPipelineRasterizationStateCreateInfo raster_state(u32 polygon) {
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = (VkPolygonMode)polygon;
    rs.lineWidth = 1.0f;
    return rs;
}
static VkPipelineDepthStencilStateCreateInfo depth_state(void) {
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    return ds;
}
static void blend_attachments(const PipelineKey *k, VkPipelineColorBlendAttachmentState att[8]) {
    for (u32 i = 0; i < k->ncolor; i++) {
        const BlendTarget *b = &k->blend[i];
        att[i] = (VkPipelineColorBlendAttachmentState){b->enable, b->src, b->dst, b->op, b->asrc, b->adst, b->aop, b->mask};
    }
}

/* The whole pipeline at once (no libraries) */
static VkPipeline make_pipeline(const PipelineKey *k, const Shader *vs, const Shader *ps) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->module, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps ? ps->module : VK_NULL_HANDLE, "main", NULL},
    };
    VkPrimitiveTopology topo = (VkPrimitiveTopology)k->topology;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, topology_strip(topo) ? VK_TRUE : VK_FALSE};
    VkPipelineRasterizationStateCreateInfo rs = raster_state(k->polygon);
    VkPipelineDepthStencilStateCreateInfo ds = depth_state();
    VkPipelineColorBlendAttachmentState att[8];
    blend_attachments(k, att);
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, k->ncolor, att};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncolor, k->color, k->depth,
                                        has_stencil(k->depth) ? k->depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, ps ? 2 : 1, stages, &NO_VERTEX_INPUT, &ia, NULL, &ONE_VIEWPORT,
                                       &rs, &ONE_SAMPLE, &ds, &cbs, &DYNAMIC_INFO, layout};
    VkPipeline p = VK_NULL_HANDLE;
    u64 t0 = rt_now_ns();
    VkResult r = vkCreateGraphicsPipelines(vk.dev, pipeline_cache, 1, &pi, NULL, &p);
    __atomic_add_fetch(&vk_stat_pipeline_ns, rt_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&vk_stat_pipelines_made, 1, __ATOMIC_RELAXED);
    if (r != VK_SUCCESS) { fprintf(stderr, "[gfx] Vulkan: cannot make a pipeline (%s)\n", vk_result(r)); return VK_NULL_HANDLE; }
    return p;
}

/* A library: one part of a pipeline (VK_GRAPHICS_PIPELINE_LIBRARY_*), made from pi */
static VkPipeline make_library(VkGraphicsPipelineLibraryFlagsEXT part, VkGraphicsPipelineCreateInfo *pi, VkPipelineRenderingCreateInfo *ri) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, ri, part};
    pi->pNext = &li;
    pi->flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk.dev, pipeline_cache, 1, pi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

/* A shader's library (the compiler threads'): the vertex stage's (polygon: fill 0, lines 1) or
 * the pixel stage's */
static VkPipeline shader_library(const Shader *s, u32 polygon) {
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                             s->stage == SHADER_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, s->module, "main", NULL};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 1;
    pi.pStages = &stage;
    pi.pDynamicState = &DYNAMIC_INFO;
    pi.layout = layout;
    u64 t0 = rt_now_ns();
    VkPipeline p;
    if (s->stage == SHADER_VERTEX) {
        VkPipelineRasterizationStateCreateInfo rs = raster_state(polygon ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
        pi.pViewportState = &ONE_VIEWPORT;
        pi.pRasterizationState = &rs;
        p = make_library(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &pi, &ri);
    } else {
        VkPipelineDepthStencilStateCreateInfo ds = depth_state();
        pi.pMultisampleState = &ONE_SAMPLE;
        pi.pDepthStencilState = &ds;
        p = make_library(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, &pi, &ri);
    }
    __atomic_add_fetch(&vk_stat_pipeline_ns, rt_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&vk_stat_libraries_made, 1, __ATOMIC_RELAXED);
    return p;
}

/* The pixel stage of a draw without one (depth only): a library without a shader */
static VkPipeline no_fragment_library(void) {
    static VkPipeline lib;
    if (!lib) {
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo ds = depth_state();
        VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pMultisampleState = &ONE_SAMPLE;
        pi.pDepthStencilState = &ds;
        pi.pDynamicState = &DYNAMIC_INFO;
        pi.layout = layout;
        lib = make_library(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, &pi, &ri);
    }
    return lib;
}

/* The vertex input part (no vertex input: only the topology), one a topology */
static VkPipeline input_library(VkPrimitiveTopology topo) {
    static VkPipeline libs[16];
    if ((u32)topo < 16 && libs[topo]) return libs[topo];
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, topology_strip(topo) ? VK_TRUE : VK_FALSE};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pVertexInputState = &NO_VERTEX_INPUT;
    pi.pInputAssemblyState = &ia;
    pi.pDynamicState = &DYNAMIC_INFO;
    VkPipeline p = make_library(VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, &pi, NULL);
    if ((u32)topo < 16) libs[topo] = p;
    return p;
}

/* The fragment output part: the targets' formats and blending, one for each such combination */
typedef struct OutputKey { VkFormat color[8], depth; u32 ncolor; BlendTarget blend[8]; } OutputKey;
static struct { OutputKey key; VkPipeline lib; } *outputs;
static u32 noutputs, outputs_cap;
static VkPipeline output_library(const PipelineKey *k) {
    OutputKey ok;
    memset(&ok, 0, sizeof ok);
    memcpy(ok.color, k->color, sizeof ok.color);
    ok.depth = k->depth;
    ok.ncolor = k->ncolor;
    memcpy(ok.blend, k->blend, sizeof ok.blend);
    for (u32 i = 0; i < noutputs; i++)
        if (!memcmp(&outputs[i].key, &ok, sizeof ok)) return outputs[i].lib;
    VkPipelineColorBlendAttachmentState att[8];
    blend_attachments(k, att);
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, k->ncolor, att};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncolor, k->color, k->depth,
                                        has_stencil(k->depth) ? k->depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pMultisampleState = &ONE_SAMPLE;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &DYNAMIC_INFO;
    VkPipeline p = make_library(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, &pi, &ri);
    if (noutputs == outputs_cap) outputs = realloc(outputs, sizeof *outputs * (outputs_cap = outputs_cap * 2 + 64));
    outputs[noutputs].key = ok;
    outputs[noutputs++].lib = p;
    return p;
}

/* The pipeline from its four libraries: at once, or (optimize) with link-time optimization */
static VkPipeline link_pipeline(const VkPipeline libs[4], int optimize) {
    VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, NULL, 4, libs};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li, optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0};
    pi.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    u64 t0 = rt_now_ns();
    VkResult r = vkCreateGraphicsPipelines(vk.dev, pipeline_cache, 1, &pi, NULL, &p);
    __atomic_add_fetch(optimize ? &vk_stat_optimize_ns : &vk_stat_link_ns, rt_now_ns() - t0, __ATOMIC_RELAXED);
    return r == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

/* ---- the compiler threads ---- */
/* Jobs: a shader's library (state: 0 not asked for, 1 queued, 2 ready, 3 failed), or a pipeline's
 * optimized version */
typedef struct Job { Shader *shader; u32 polygon; Pipe *pipe; } Job;
static Job *jobs;
static u32 jobs_head, jobs_tail, jobs_cap;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_ready = PTHREAD_COND_INITIALIZER;
static volatile int compilers_started;
static volatile long compiles_pending;

/* What each compiler thread is doing (for a crash report: a fault in the driver's compiler names
 * the shader) */
static struct { DWORD thread; Shader *volatile shader; Pipe *volatile pipe; volatile u64 since_ms; } compilers[4];
static volatile long ncompilers;

static void *compiler(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);   /* (below the game's threads) */
    long me = __atomic_fetch_add(&ncompilers, 1, __ATOMIC_ACQ_REL);
    compilers[me].thread = GetCurrentThreadId();
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (jobs_head == jobs_tail) pthread_cond_wait(&jobs_ready, &jobs_lock);
        /* libraries before optimized pipelines: those stand between a draw and the screen */
        u32 pick = jobs_head;
        for (u32 i = jobs_head; i != jobs_tail; i = (i + 1) % jobs_cap)
            if (jobs[i].shader) { pick = i; break; }
        Job j = jobs[pick];
        jobs[pick] = jobs[jobs_head];
        jobs_head = (jobs_head + 1) % jobs_cap;
        pthread_mutex_unlock(&jobs_lock);
        compilers[me].since_ms = GetTickCount64();
        compilers[me].shader = j.shader;
        compilers[me].pipe = j.pipe;
        if (j.shader) {
            VkPipeline lib = shader_library(j.shader, j.polygon);
            j.shader->lib[j.polygon] = lib;
            if (lib && j.shader->stage == SHADER_PIXEL) {   /* (its only library: the module, the SPIR-V the driver keeps, no longer needed) */
                vkDestroyShaderModule(vk.dev, j.shader->module, NULL);
                j.shader->module = VK_NULL_HANDLE;
            }
            __atomic_store_n(&j.shader->lib_state[j.polygon], lib ? 2 : 3, __ATOMIC_RELEASE);
        } else {
            VkPipeline p = link_pipeline(j.pipe->libs, 1);
            if (p) __atomic_store_n(&j.pipe->optimized, p, __ATOMIC_RELEASE);
        }
        compilers[me].shader = NULL;
        compilers[me].pipe = NULL;
        __atomic_sub_fetch(&compiles_pending, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

/* gfx_report_compiles: the compiler threads at the time of a crash or a hang */
void vk_draw_report_compiles(void (*line)(const char *text)) {
    char text[300];
    DWORD self = GetCurrentThreadId();
    u64 now = GetTickCount64();
    int busy = 0;
    for (long i = 0; i < ncompilers && i < 4; i++) {
        Shader *s = compilers[i].shader;
        Pipe *p = compilers[i].pipe;
        if (!s && !p) continue;
        if (!busy++) line("Vulkan compiler threads at the time:\n");
        if (s) snprintf(text, sizeof text, "  thread %lu%s: the driver compiling %s's library, for %.1f s\n", (unsigned long)compilers[i].thread,
                        compilers[i].thread == self ? " (this thread)" : "", s->name, (now - compilers[i].since_ms) / 1000.0);
        else snprintf(text, sizeof text, "  thread %lu%s: the driver optimizing a pipeline, for %.1f s\n", (unsigned long)compilers[i].thread,
                      compilers[i].thread == self ? " (this thread)" : "", (now - compilers[i].since_ms) / 1000.0);
        line(text);
    }
    snprintf(text, sizeof text, "Vulkan: %ld compiles pending, %u shaders, %llu pipelines\n", compiles_pending, shader_count, (unsigned long long)vk_stat_pipelines);
    line(text);
}

static void queue_job(Job j) {
    if (!compilers_started) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int n = cpus > 4 ? (int)(cpus / 2) : 1;
        if (n > 4) n = 4;   /* (compilers[]' size) */
        for (int i = 0; i < n; i++) {
            pthread_t t;
            pthread_create(&t, NULL, compiler, NULL);
            pthread_detach(t);
        }
        compilers_started = 1;
    }
    pthread_mutex_lock(&jobs_lock);
    if ((jobs_tail + 1) % (jobs_cap ? jobs_cap : 1) == jobs_head || !jobs_cap) {   /* (full: a larger ring, in order) */
        u32 cap = jobs_cap ? jobs_cap * 2 : 1024, n = 0;
        Job *t = malloc(sizeof *t * cap);
        for (u32 i = jobs_head; jobs_cap && i != jobs_tail; i = (i + 1) % jobs_cap) t[n++] = jobs[i];
        free(jobs);
        jobs = t;
        jobs_cap = cap;
        jobs_head = 0;
        jobs_tail = n;
    }
    jobs[jobs_tail] = j;
    jobs_tail = (jobs_tail + 1) % jobs_cap;
    __atomic_add_fetch(&compiles_pending, 1, __ATOMIC_RELAXED);
    pthread_cond_signal(&jobs_ready);
    pthread_mutex_unlock(&jobs_lock);
}

/* Is the shader's library ready? (asked for now if it is not) */
static int library_ready(Shader *s, u32 polygon) {
    int st = __atomic_load_n(&s->lib_state[polygon], __ATOMIC_ACQUIRE);
    if (st == 0) {
        s->lib_state[polygon] = 1;
        queue_job((Job){s, polygon, NULL});
        return 0;
    }
    return st == 2;
}

static VkPipeline get_pipeline(const PipelineKey *k, Shader *vs, Shader *ps) {
    if (npipelines * 2 >= pipelines_cap) {   /* (open addressing: grown and placed again) */
        u32 cap = pipelines_cap ? pipelines_cap * 2 : 1024;
        PipelineEntry *t = calloc(cap, sizeof *t);
        for (u32 i = 0; i < pipelines_cap; i++) {
            if (!pipelines[i].pipe) continue;
            u32 s = (u32)key_hash(&pipelines[i].key) & (cap - 1);
            while (t[s].pipe) s = (s + 1) & (cap - 1);
            t[s] = pipelines[i];
        }
        free(pipelines);
        pipelines = t;
        pipelines_cap = cap;
    }
    u32 s = (u32)key_hash(k) & (pipelines_cap - 1);
    for (; pipelines[s].pipe; s = (s + 1) & (pipelines_cap - 1))
        if (!memcmp(&pipelines[s].key, k, sizeof *k)) {
            Pipe *p = pipelines[s].pipe;
            VkPipeline o = __atomic_load_n(&p->optimized, __ATOMIC_ACQUIRE);
            return o ? o : p->fast;
        }
    Pipe *p = calloc(1, sizeof *p);
    p->key = *k;
    if (vk.has.gpl) {
        u32 line = k->polygon == VK_POLYGON_MODE_LINE;
        p->libs[0] = input_library((VkPrimitiveTopology)k->topology);
        p->libs[1] = vs->lib[line];
        p->libs[2] = ps ? ps->lib[0] : no_fragment_library();
        p->libs[3] = output_library(k);
        for (u32 i = 0; i < 4; i++)
            if (!p->libs[i]) { fprintf(stderr, "[gfx] Vulkan: a pipeline library is missing\n"); free(p); return VK_NULL_HANDLE; }
        p->fast = link_pipeline(p->libs, 0);
        if (p->fast) queue_job((Job){NULL, 0, p});
    } else
        p->fast = make_pipeline(k, vs, ps);
    if (!p->fast) { free(p); return VK_NULL_HANDLE; }
    pipelines[s] = (PipelineEntry){*k, p};
    npipelines++;
    vk_stat_pipelines++;
    return p->fast;
}

/* ---- the shaders' disk cache -------------------------------------------------------------------- */
/* Every shader translated is appended to dump/spirvcache/shaders_<translator version>.bin (or
 * SSBU_SPIRV_CACHE's directory): its key, ShaderInfo and SPIR-V. At the renderer's start the file is
 * read back - each shader made a module again, and with graphics pipeline libraries its library
 * queued for the compiler threads - so a later session's draws find their shaders translated and,
 * by the time the battle needs them, compiled (prewarm). A translator that changed has another
 * version: the older files are deleted. */
#include "spirv_version.h"
typedef struct CacheRecord {
    u32 magic, words;
    int stage;
    u32 vs_outputs, local[3], shared;
    u64 hash, table_hash, vin;
    ShaderInfo info;
} CacheRecord;
#define CACHE_MAGIC 0x56535053u   /* "SPSV" */
static FILE *cache_file;
static u32 vk_stat_cached_shaders;

static void cache_dir(char *out, size_t n) {
    const char *env = getenv("SSBU_SPIRV_CACHE");
    if (env && *env) snprintf(out, n, "%s", env);
    else snprintf(out, n, "%s/../spirvcache", rt_data_dir);
}

/* (while the file is being read: the shaders translated meanwhile, written once it is) */
static struct { CacheRecord r; u32 *code; } *cache_pending;
static u32 ncache_pending, cache_pending_cap;
static int cache_loading;

static void cache_write(const CacheRecord *r, const u32 *code) {
    if (fwrite(r, sizeof *r, 1, cache_file) != 1 || fwrite(code, 4, r->words, cache_file) != r->words) { fclose(cache_file); cache_file = NULL; return; }
    fflush(cache_file);
}

static void cache_add(const Shader *s, const u32 *code, u32 words) {
    CacheRecord r = {CACHE_MAGIC, words, s->stage, s->vs_outputs, {s->local[0], s->local[1], s->local[2]}, s->shared, s->hash, s->table_hash, s->vin, s->info};
    if (cache_loading) {
        if (ncache_pending == cache_pending_cap) cache_pending = realloc(cache_pending, sizeof *cache_pending * (cache_pending_cap = cache_pending_cap * 2 + 64));
        cache_pending[ncache_pending].r = r;
        cache_pending[ncache_pending].code = malloc(4 * (size_t)words);
        memcpy(cache_pending[ncache_pending++].code, code, 4 * (size_t)words);
        return;
    }
    if (cache_file) cache_write(&r, code);
}

/* The file read on a thread of its own (half a gigabyte of shaders took seconds, the renderer held
 * meanwhile): each shader made a module there, and put in the cache, its library queued, under the
 * renderer's lock - a batch at a time. A shader the game asks for first is translated as without the
 * file; its copy from the file is then left out. */
static char cache_path[1100];
static void *cache_loader(void *arg) {
    (void)arg;
    FILE *f = fopen(cache_path, "rb");
    long good = 0;   /* (where the last whole record ends: a write cut short is dropped) */
    u32 *code = NULL, cap = 0, loaded = 0;
    CacheRecord r;
    Shader *batch[64];
    u32 nbatch = 0;
    for (int more = f != NULL; more || nbatch;) {
        if (more && fread(&r, sizeof r, 1, f) == 1 && r.magic == CACHE_MAGIC && r.words && r.words < (16u << 20)) {
            if (r.words > cap) code = realloc(code, 4 * (size_t)(cap = r.words));
            if (fread(code, 4, r.words, f) != r.words) { more = 0; continue; }
            good = ftell(f);
            Shader *s = calloc(1, sizeof *s);
            s->hash = r.hash; s->stage = r.stage; s->vs_outputs = r.vs_outputs; s->table_hash = r.table_hash; s->vin = r.vin;
            memcpy(s->local, r.local, sizeof s->local);
            s->shared = r.shared;
            s->info = r.info;
            s->from_cache = 1;
            snprintf(s->name, sizeof s->name, "%s_%016llx", s->stage == SHADER_VERTEX ? "vs" : s->stage == SHADER_PIXEL ? "ps" : "cs", (unsigned long long)s->hash);
            VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)r.words * 4, code};
            if (vkCreateShaderModule(vk.dev, &mi, NULL, &s->module) != VK_SUCCESS) { free(s); continue; }
            batch[nbatch++] = s;
            if (nbatch < 64) continue;
        } else more = 0;
        vk_enter();
        for (u32 b = 0; b < nbatch; b++) {
            Shader *s = batch[b];
            int dup = 0;   /* (translated meanwhile, or twice in the file) */
            if (s->vin) dup = variant_find(s->hash, s->table_hash, s->vin) != NULL;
            else
                for (u32 i = 0; i < shader_count && !dup; i++) {
                    Shader *o = shader_cache[i];
                    dup = o->hash == s->hash && o->stage == s->stage && !o->vin && o->vs_outputs == s->vs_outputs && o->table_hash == s->table_hash &&
                          !memcmp(o->local, s->local, sizeof s->local) && o->shared == s->shared;
                }
            if (dup) { vkDestroyShaderModule(vk.dev, s->module, NULL); free(s); continue; }
            if (shader_count == shader_cap) shader_cache = realloc(shader_cache, sizeof *shader_cache * (shader_cap = shader_cap * 2 + 64));
            shader_cache[shader_count++] = s;
            if (s->vin) variant_add(s);
            gfx_stat_shaders++;
            loaded++;
            if (vk.has.gpl && s->stage != SHADER_COMPUTE) library_ready(s, 0);   /* (queued: compiled now, on the compiler threads) */
        }
        vk_leave();
        nbatch = 0;
    }
    free(code);
    vk_enter();   /* (the file whole again, then what was translated meanwhile) */
    if (f) { fclose(f); truncate(cache_path, good); }
    cache_file = fopen(cache_path, "ab");
    cache_loading = 0;
    for (u32 i = 0; i < ncache_pending; i++) {
        if (cache_file) cache_write(&cache_pending[i].r, cache_pending[i].code);
        free(cache_pending[i].code);
    }
    ncache_pending = 0;
    vk_stat_cached_shaders = loaded;
    if (loaded) fprintf(stderr, "[gfx] Vulkan: %u shaders from %s%s\n", loaded, cache_path, vk.has.gpl ? ", their libraries compiling" : "");
    vk_leave();
    return NULL;
}

static void cache_load(void) {
    char dir[1024], name[64], old[1100];
    cache_dir(dir, sizeof dir);
    if (!strcmp(dir, "0")) return;   /* (SSBU_SPIRV_CACHE=0: none) */
    mkdir(dir, 0755);
    snprintf(name, sizeof name, "shaders_%s.bin", SPIRV_TRANSLATOR_VERSION);
    DIR *d = opendir(dir);   /* (an older translator's: gone) */
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strncmp(e->d_name, "shaders_", 8) && strcmp(e->d_name, name)) {
            snprintf(old, sizeof old, "%s/%s", dir, e->d_name);
            unlink(old);
        }
    if (d) closedir(d);
    snprintf(cache_path, sizeof cache_path, "%s/%s", dir, name);
    cache_loading = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, cache_loader, NULL) != 0) { cache_loading = 0; cache_file = fopen(cache_path, "ab"); return; }
    pthread_detach(t);
}

/* ---- rendering -------------------------------------------------------------------------------------- */
/* The rendering in progress: its attachments (surfaces), begun when a draw needed them */
static Surface *cur_color[8], *cur_depth;
static u32 cur_ncolor;
static VkPipeline cur_pipeline;
/* What the slot's command buffer has of a draw's state: dropped when the slot starts, or a pass of
 * our own sets it */
static int dyn_valid;
static VkDescriptorSet bound_sets[3];
static void forget_state(void) { dyn_valid = 0; memset(bound_sets, 0, sizeof bound_sets); }

static void suspend_rendering(void) {
    if (!rendering) return;
    vkCmdEndRendering(vk_cmd());
    rendering = 0;
    cur_pipeline = VK_NULL_HANDLE;
}
void vk_draw_suspend(void) { suspend_rendering(); flush_barriers(vk_cmd()); }

/* Begin rendering into these attachments (their barriers recorded first) */
static void begin_rendering(Surface *const color[8], u32 ncolor, Surface *depth) {
    VkCommandBuffer cb = vk_cmd();
    flush_barriers(cb);
    VkRenderingAttachmentInfo ca[8], da;
    u32 w = 16384, h = 16384;
    for (u32 i = 0; i < ncolor; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, color[i] ? color[i]->view : VK_NULL_HANDLE,
                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        if (color[i]) { if (color[i]->pw < w) w = color[i]->pw; if (color[i]->ph < h) h = color[i]->ph; }
    }
    if (depth) {
        da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, depth->view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
                                         VK_ATTACHMENT_STORE_OP_STORE};
        if (depth->pw < w) w = depth->pw;
        if (depth->ph < h) h = depth->ph;
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, ncolor, ca, depth ? &da : NULL,
                          depth && has_stencil(depth->view_format) ? &da : NULL};
    vkCmdBeginRendering(cb, &ri);
    rendering = 1;
    memcpy(cur_color, color, sizeof cur_color);
    cur_ncolor = ncolor;
    cur_depth = depth;
    cur_pipeline = VK_NULL_HANDLE;
}

static int attachment_of(const Img *m) {
    if (!m) return 0;
    for (u32 i = 0; i < cur_ncolor; i++) if (cur_color[i] && &cur_color[i]->img == m) return 1;
    return cur_depth && &cur_depth->img == m;
}

/* ---- vertex inputs ------------------------------------------------------------------------------ */
/* With dynamic vertex input (VK_EXT_vertex_input_dynamic_state), a draw whose every attribute has a
 * format the device fetches is drawn with the vertex shader's vertex-input version (vertex_variant):
 * the device's vertex fetch reads and converts them, instead of the shader decoding each from its
 * stream's words (fetch_attribute: every format's arithmetic, for every vertex). The formats are
 * dynamic state, not the pipeline's. Any other draw reads its streams as before. */
static VkPhysicalDeviceLimits vin_limits;
static u64 vin_why[8];   /* (debug: why draws read their streams) */

/* The attribute format word's Vulkan format, its class (0 float, 1 signed, 2 unsigned) and component
 * size in bytes (alignment); VK_FORMAT_UNDEFINED if there is none the device fetches */
static VkFormat vertex_format(u32 fmt, u32 *cls, u32 *comp) {
    u32 size = (fmt >> 21) & 0x3F, type = (fmt >> 27) & 7, reverse = fmt >> 31;
    u32 n = 0, bits = 0;
    switch (size) {
    case 0x01: n = 4; bits = 32; break; case 0x02: n = 3; bits = 32; break; case 0x03: n = 4; bits = 16; break;
    case 0x04: n = 2; bits = 32; break; case 0x05: n = 3; bits = 16; break; case 0x0A: case 0x33: n = 4; bits = 8; break;
    case 0x0F: n = 2; bits = 16; break; case 0x12: n = 1; bits = 32; break; case 0x13: n = 3; bits = 8; break;
    case 0x18: case 0x32: n = 2; bits = 8; break; case 0x1B: n = 1; bits = 16; break; case 0x1D: case 0x34: n = 1; bits = 8; break;
    case 0x30: n = 4; bits = 10; break;   /* (10-10-10-2) */
    default: return VK_FORMAT_UNDEFINED;
    }
    /* by type: 1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled, 7 float */
    static const VkFormat f8[4][7] = {
        {VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SINT, VK_FORMAT_R8_UINT, VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, 0},
        {VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, 0},
        {VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, 0},
        {VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_USCALED,
         VK_FORMAT_R8G8B8A8_SSCALED, 0}};
    static const VkFormat f16[4][7] = {
        {VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SFLOAT},
        {VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED,
         VK_FORMAT_R16G16_SFLOAT},
        {VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_USCALED,
         VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16_SFLOAT},
        {VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_UINT,
         VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SFLOAT}};
    static const VkFormat f32[4][7] = {
        {0, 0, VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT, 0, 0, VK_FORMAT_R32_SFLOAT},
        {0, 0, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT, 0, 0, VK_FORMAT_R32G32_SFLOAT},
        {0, 0, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT, 0, 0, VK_FORMAT_R32G32B32_SFLOAT},
        {0, 0, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT}};
    static const VkFormat f1010102[7] = {VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32,
                                         VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32, 0};
    static const VkFormat bgra8[7] = {VK_FORMAT_B8G8R8A8_SNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SINT, VK_FORMAT_B8G8R8A8_UINT,
                                      VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED, 0};
    if (!type) return VK_FORMAT_UNDEFINED;
    VkFormat f = bits == 10 ? f1010102[type - 1] : bits == 8 ? f8[n - 1][type - 1] : bits == 16 ? f16[n - 1][type - 1] : f32[n - 1][type - 1];
    if (reverse) f = bits == 8 && n == 4 ? bgra8[type - 1] : VK_FORMAT_UNDEFINED;   /* (B and R swapped: only 8-8-8-8 has such a format) */
    if (!f) return VK_FORMAT_UNDEFINED;
    static u8 fetchable[256];   /* (0 not asked, 1 yes, 2 no; the formats here are all below 256) */
    if ((u32)f < 256 && !fetchable[f]) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk.pd, f, &fp);
        fetchable[f] = fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT ? 1 : 2;
    }
    if ((u32)f >= 256 || fetchable[f] != 1) return VK_FORMAT_UNDEFINED;
    *cls = type == 3 ? 1 : type == 4 ? 2 : 0;
    *comp = bits == 10 ? 4 : bits / 8;
    return f;
}

typedef struct VertexInput {
    u32 nbind, nattr;
    VkVertexInputBindingDescription2EXT bind[32];
    VkVertexInputAttributeDescription2EXT attr[32];
    VkBuffer buf[32];
    VkDeviceSize off[32], size[32];
} VertexInput;

/* The draw's attributes as vertex inputs (vin: shader_spirv_ex's), from its streams' buffers; 0 if
 * one cannot be: an unknown or unfetchable format, an instance divisor, a misaligned attribute, past
 * the device's limits. (The base instance is then the draw's firstInstance.) */
static int vertex_inputs(const Shader *vs, const DrawParams *params, const VkDescriptorBufferInfo stream_of[32], const u32 stream_delta[32],
                         VertexInput *vi, u64 *vin) {
    static int off = -1;   /* debug: SSBU_VK_NO_VERTEX_INPUT=1 reads every draw's streams in the shader */
    if (off < 0) off = getenv("SSBU_VK_NO_VERTEX_INPUT") != NULL;
    if (off || !vk.has.vertex_input) return 0;
    if (!vin_limits.maxVertexInputAttributes) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk.pd, &props);
        vin_limits = props.limits;
    }
    u32 bind_of[33];
    memset(bind_of, 0xFF, sizeof bind_of);
    vi->nbind = vi->nattr = 0;
    *vin = SPIRV_VERTEX_INPUTS;
    for (u32 n = 0; n < 32; n++) {
        if (!(vs->info.inputs >> n & 1)) continue;
        if (n >= vin_limits.maxVertexInputAttributes) { vin_why[1]++; return 0; }
        u32 fmt = params->attr[n][0], stride = params->attr[n][1], divisor = params->attr[n][2], j = fmt & 0x1F, at = (fmt >> 7) & 0x3FFF;
        u32 cls = 0, comp = 4, key = j;
        VkFormat f;
        if (!params->attr[n][3] || (fmt & 0x40)) {   /* no stream (or a constant one): (0, 0, 0, 1), as from a null buffer */
            f = VK_FORMAT_R32G32B32A32_SFLOAT;
            key = 32;
            at = 0;
        } else {
            f = vertex_format(fmt, &cls, &comp);
            if (!f) {
                static int shown;
                if (shown++ < 20) fprintf(stderr, "[vin] format %#x: size %#x type %u reverse %u\n", fmt, (fmt >> 21) & 0x3F, (fmt >> 27) & 7, fmt >> 31);
                vin_why[2]++; return 0;
            }
            if (divisor > 1) { vin_why[3]++; return 0; }
            if (at > vin_limits.maxVertexInputAttributeOffset || stride > vin_limits.maxVertexInputBindingStride) { vin_why[4]++; return 0; }
            if ((stream_of[j].offset + stream_delta[j] + at) % comp || stride % comp) { vin_why[5]++; return 0; }
        }
        if (bind_of[key] == 0xFFFFFFFF) {
            if (vi->nbind >= vin_limits.maxVertexInputBindings) return 0;
            u32 b = bind_of[key] = vi->nbind++;
            vi->bind[b] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, b, key == 32 ? 0 : stride,
                                                                 divisor && key != 32 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
            if (key == 32) { vi->buf[b] = VK_NULL_HANDLE; vi->off[b] = 0; vi->size[b] = VK_WHOLE_SIZE; }
            else {
                vi->buf[b] = stream_of[j].buffer;
                vi->off[b] = stream_of[j].offset + stream_delta[j];
                vi->size[b] = stream_of[j].range == VK_WHOLE_SIZE ? VK_WHOLE_SIZE : stream_of[j].range - stream_delta[j];
            }
        }
        vi->attr[vi->nattr++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, n, bind_of[key], f, at};
        *vin |= (u64)cls << (2 * n);
    }
    return 1;
}
u64 vk_stat_vertex_input_draws;

/* ---- draws ------------------------------------------------------------------------------------------ */
static u32 instance_counter;

/* A stage's constant buffers, storage buffers and textures into its descriptors */
typedef struct Binding {
    VkDescriptorBufferInfo cb[18], sb[16];
    VkDescriptorImageInfo tex[32];
    Img *tex_img[32];
} Binding;

static void bind_stage(const u32 *regs, const CbBinding cb[STAGES][18], int stage, const Shader *s, DrawParams *params, Binding *b) {
    int mstage = stage == SHADER_VERTEX ? STAGE_VERTEX : STAGE_FRAGMENT, si = stage == SHADER_VERTEX ? 0 : 1;
    for (u32 k = 0; k < 18; k++) {
        if (!(s->info.cbuf_mask >> k & 1)) continue;
        u32 size = cb[mstage][k].size;
        if (size > 0x10000) size = 0x10000;
        if (size > s->info.cbuf_extent[k]) size = s->info.cbuf_extent[k];   /* only what the program can read */
        b->cb[k] = upload_kept(cb[mstage][k].address, size);
    }
    for (u32 k = 0; k < s->info.sb_count; k++) {   /* storage buffers: their descriptors in constant buffer 0 */
        u32 desc[4] = {0};
        if (cb[mstage][0].address) read_gpu(cb[mstage][0].address + s->info.sb_offset[k], 16, desc);
        u64 va = (u64)desc[1] << 32 | desc[0];
        u64 end = (va + desc[2] + 0xFFF) & ~0xFFFull;
        u32 size = desc[2] && end - va <= (16u << 20) ? (u32)(end - va) : desc[2] ? 16u << 20 : 0;
        params->sb[si][k][0] = desc[0];
        params->sb[si][k][1] = desc[1];
        params->sb[si][k][2] = size;
        if (!va || !size) { b->sb[k] = upload_gpu(0, 0); continue; }
        Resident *res = (va & 3) == 0 ? resident_covering(va, va + size) : NULL;   /* written by compute: the GPU's copy */
        if (res) { b->sb[k] = resident_desc(res, va, size, &params->sb[si][k][3]); continue; }
        b->sb[k] = resident_overlapping(va, size) ? upload_composed(va, size) : upload_kept(va, size);
        sb_bytes += size;
    }
    u32 tex_cb = regs[M_BINDLESS_TEXTURE] & 0x1F;
    u64 tic_pool = (u64)regs[M_TEX_HEADER_POOL] << 32 | regs[M_TEX_HEADER_POOL + 1];
    u64 tsc_pool = (u64)regs[M_TEX_SAMPLER_POOL] << 32 | regs[M_TEX_SAMPLER_POOL + 1];
    for (u32 k = 0; k < s->info.ntex; k++) {
        u32 handle = 0, tic[8], tsc[8];
        if (tex_cb < 18 && cb[mstage][tex_cb].address) read_gpu(cb[mstage][tex_cb].address + s->info.tex[k].handle_offset, 4, &handle);
        u32 tic_index = handle & 0xFFFFF, tsc_index = regs[M_SAMPLER_BINDING] & 1 ? tic_index : (handle >> 20) & 0xFFF;
        read_gpu(tic_pool + tic_index * 32ull, 32, tic);
        read_gpu(tsc_pool + tsc_index * 32ull, 32, tsc);
        float tscale;
        Img *img;
        VkImageView view = get_texture(tic, s->info.tex[k].type, &tscale, &img);
        params->texscale[si][k] = tscale - 1.0f;
        params->swz[si][k] = (tic[0] >> 19) & 0xFFF;
        VkSampler smp = get_sampler(tsc);
        b->tex[k] = (VkDescriptorImageInfo){smp ? smp : default_sampler(), view, img ? read_layout(img) : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        b->tex_img[k] = view ? img : NULL;
    }
}

/* The descriptor sets of a draw (or dispatch): sets 0-2, each from the slot's pool - or the draw
 * before's again when it would hold the same (most draws of a menu share their buffers) */
typedef struct SetContent {
    u64 buffers;                       /* (set 0: bits 0-63 of 68 bindings; the last four in more) */
    u32 more, nimages;
    VkDescriptorBufferInfo buf[SPIRV_STREAM_BINDING + 32];
    VkDescriptorImageInfo img[64];
    u64 images;                        /* (set 1) */
} SetContent;
static SetContent last_content[3];
static VkDescriptorSet last_set[3];
static u64 last_set_frame;
u64 vk_stat_sets_reused;

static void write_sets(const Shader *const sh[2], const Binding *b, const VkDescriptorBufferInfo streams[32], VkDescriptorSet sets[3]) {
    static SetContent want[3];
    memset(want, 0, sizeof want);
    for (u32 st = 0; st < 2; st++) {
        const Shader *s = sh[st];
        if (!s) continue;
        u32 cb_base = st ? SPIRV_CBUF_BINDING_PIXEL : 0, tex_base = st ? SPIRV_TEXTURE_BINDING_PIXEL : 0, sb_base = st ? SPIRV_STORAGE_BINDING_PIXEL : 0;
        for (u32 k = 0; k < 18; k++)
            if (s->info.cbuf_mask >> k & 1) { want[0].buffers |= 1ull << (cb_base + k); want[0].buf[cb_base + k] = b[st].cb[k]; }
        for (u32 k = 0; k < s->info.ntex; k++) { want[1].images |= 1ull << (tex_base + k); want[1].img[tex_base + k] = b[st].tex[k]; }
        for (u32 k = 0; k < s->info.sb_count; k++) { want[2].buffers |= 1ull << (sb_base + k); want[2].buf[sb_base + k] = b[st].sb[k]; }
    }
    if (streams)
        for (u32 k = 0; k < 32; k++)
            if (sh[0]->info.inputs >> k & 1) {
                u32 at = SPIRV_STREAM_BINDING + k;
                if (at < 64) want[0].buffers |= 1ull << at; else want[0].more |= 1u << (at - 64);
                want[0].buf[at] = streams[k];
            }
    u64 frame = vk_frame_serial();
    VkWriteDescriptorSet w[256];
    u32 n = 0;
    for (u32 i = 0; i < 3; i++) {
        if (last_set_frame == frame && last_set[i] && !memcmp(&want[i], &last_content[i], sizeof want[i])) {
            sets[i] = last_set[i];
            vk_stat_sets_reused++;
            continue;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, slots[vk_slot()].pool, 1, &set_layout[i]};
        if (vkAllocateDescriptorSets(vk.dev, &ai, &sets[i]) != VK_SUCCESS) {   /* the pool is full: the frame's work goes, the next slot's pool */
            vk_flush(0);
            frame = vk_frame_serial();
            ai.descriptorPool = slots[vk_slot()].pool;
            vk_check(vkAllocateDescriptorSets(vk.dev, &ai, &sets[i]), "descriptor sets");
            for (u32 j = 0; j < i; j++) {   /* (the sets before, reused from the frame now gone, are made again too) */
                vk_check(vkAllocateDescriptorSets(vk.dev, &(VkDescriptorSetAllocateInfo){VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, ai.descriptorPool, 1, &set_layout[j]}, &sets[j]),
                         "descriptor sets");
                last_set[j] = VK_NULL_HANDLE;
            }
            n = 0;
            for (u32 j = 0; j < i; j++) {
                const SetContent *c = &want[j];
                for (u32 k = 0; k < 68; k++)
                    if (j != 1 && (k < 64 ? c->buffers >> k & 1 : c->more >> (k - 64) & 1))
                        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[j], k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &c->buf[k], NULL};
                    else if (j == 1 && k < 64 && c->images >> k & 1)
                        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[j], k, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &c->img[k], NULL, NULL};
            }
        }
        const SetContent *c = &want[i];
        for (u32 k = 0; k < 68; k++)
            if (i != 1 && (k < 64 ? c->buffers >> k & 1 : c->more >> (k - 64) & 1))
                w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[i], k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &c->buf[k], NULL};
            else if (i == 1 && k < 64 && c->images >> k & 1)
                w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[i], k, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &c->img[k], NULL, NULL};
        last_content[i] = want[i];
        last_set[i] = sets[i];
    }
    last_set_frame = frame;
    if (n) vkUpdateDescriptorSets(vk.dev, n, w, 0, NULL);
}

/* SSBU_GPU_TIMES=<second>: the draws of one frame from then on timed on the GPU (a timestamp before
 * and after each), and the costliest shader pairs written to the log, as gpu_d3d11.c does */
#define GPU_TIMES_MAX 8192
static int gpu_times_state;   /* 0: not asked for, 1: waiting for the second, 2: timing a frame, 3: done */
static VkQueryPool gpu_times_pool;
static u32 gpu_times_n;
static struct { char vs[24], ps[24]; u32 w, h; } gpu_times_what[GPU_TIMES_MAX / 2];

static void gpu_times_before(VkCommandBuffer cb) {
    if (gpu_times_state != 2 || gpu_times_n + 2 > GPU_TIMES_MAX) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_times_pool, gpu_times_n);
}
static void gpu_times_after(VkCommandBuffer cb, const char *vs, const char *ps, u32 w, u32 h) {
    if (gpu_times_state != 2 || gpu_times_n + 2 > GPU_TIMES_MAX) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_times_pool, gpu_times_n + 1);
    u32 k = gpu_times_n / 2;
    snprintf(gpu_times_what[k].vs, sizeof gpu_times_what[k].vs, "%s", vs);
    snprintf(gpu_times_what[k].ps, sizeof gpu_times_what[k].ps, "%s", ps);
    gpu_times_what[k].w = w; gpu_times_what[k].h = h;
    gpu_times_n += 2;
}
/* (at each present, before the frame is shown) */
static void gpu_times_frame(void) {
    if (gpu_times_state == 0) {
        gpu_times_state = getenv("SSBU_GPU_TIMES") ? 1 : 3;
        if (gpu_times_state == 1) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, GPU_TIMES_MAX + 2, 0};
            if (vkCreateQueryPool(vk.dev, &qi, NULL, &gpu_times_pool) != VK_SUCCESS) gpu_times_state = 3;
        }
    }
    if (gpu_times_state == 1 && rt_now_ns() > (u64)(atof(getenv("SSBU_GPU_TIMES")) * 1e9)) {
        suspend_rendering();
        VkCommandBuffer cb = vk_cmd();
        vkCmdResetQueryPool(cb, gpu_times_pool, 0, GPU_TIMES_MAX + 2);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_times_pool, GPU_TIMES_MAX);
        gpu_times_n = 0;
        gpu_times_state = 2;
        return;
    }
    if (gpu_times_state != 2) return;
    gpu_times_state = 3;
    suspend_rendering();
    vkCmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_times_pool, GPU_TIMES_MAX + 1);
    vk_flush(1);
    u64 *t = calloc(GPU_TIMES_MAX + 2, sizeof *t);
    if (gpu_times_n) vkGetQueryPoolResults(vk.dev, gpu_times_pool, 0, gpu_times_n, gpu_times_n * 8, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    vkGetQueryPoolResults(vk.dev, gpu_times_pool, GPU_TIMES_MAX, 2, 16, t + GPU_TIMES_MAX, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.pd, &props);
    double us = props.limits.timestampPeriod / 1000.0, total = (double)(t[GPU_TIMES_MAX + 1] - t[GPU_TIMES_MAX]) * us, drawn = 0;
    /* by shader pair and target size: count, time */
    struct { u32 first, count; double us; } *g = calloc(GPU_TIMES_MAX / 2, sizeof *g);
    u32 ng = 0;
    for (u32 k = 0; k < gpu_times_n / 2; k++) {
        double d = t[2 * k + 1] > t[2 * k] ? (double)(t[2 * k + 1] - t[2 * k]) * us : 0;
        drawn += d;
        u32 j = 0;
        for (; j < ng; j++) {
            u32 f = g[j].first;
            if (!strcmp(gpu_times_what[f].vs, gpu_times_what[k].vs) && !strcmp(gpu_times_what[f].ps, gpu_times_what[k].ps) &&
                gpu_times_what[f].w == gpu_times_what[k].w && gpu_times_what[f].h == gpu_times_what[k].h) break;
        }
        if (j == ng) g[ng++].first = k;
        g[j].count++;
        g[j].us += d;
    }
    fprintf(stderr, "[gpu times] one frame: %.0f us on the GPU from start to present, %u draws taking %.0f us; costliest:\n", total, gpu_times_n / 2, drawn);
    for (u32 shown = 0; shown < 25 && shown < ng; shown++) {
        u32 best = 0;
        for (u32 j = 1; j < ng; j++) if (g[j].us > g[best].us) best = j;
        u32 f = g[best].first;
        fprintf(stderr, "[gpu times]   %8.0f us  %4u draws  %s %s  %ux%u\n", g[best].us, g[best].count, gpu_times_what[f].vs, gpu_times_what[f].ps,
                gpu_times_what[f].w, gpu_times_what[f].h);
        g[best].us = -1;
    }
    free(g);
    free(t);
}

void vk_gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) {
    vk_enter();
    u32 begin = regs[M_BEGIN];
    if (begin & (1u << 26)) instance_counter++;
    else if (!(begin & (1u << 27))) instance_counter = 0;
    VkPrimitiveTopology topo;
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 vs_g = gpu_va_to_guest(region + regs[M_PIPELINE(1) + 1]);
    u64 ps_g = regs[M_PIPELINE(5)] & 1 ? gpu_va_to_guest(region + regs[M_PIPELINE(5) + 1]) : 0;
    Shader *vs = vs_g ? get_shader(vs_g, SHADER_VERTEX, 0, cb[STAGE_VERTEX], NULL, 0) : NULL;
    Shader *ps = vs && ps_g ? get_shader(ps_g, SHADER_PIXEL, vs->info.outputs, cb[STAGE_FRAGMENT], NULL, 0) : NULL;
    if (!vs || (ps_g && !ps) || !topology(begin & 0xFFFF, &topo)) {
        gfx_stat_skipped++;
        if (!topology(begin & 0xFFFF, &topo)) gfx_skip_topology++; else gfx_skip_shader++;
        vk_leave();
        return;
    }

    /* render targets */
    Surface *color[8] = {0}, *depth = NULL;
    u32 sel = regs[M_CT_SELECT], count = sel & 15, rt_w = 0, rt_h = 0, ncolor = 0;
    float rts = 1.0f;
    for (u32 i = 0; i < count && i < 8; i++) {
        u32 ct = (sel >> (4 + 3 * i)) & 7;
        u64 va = (u64)regs[M_CT(ct)] << 32 | regs[M_CT(ct) + 1];
        if (!va || !regs[M_CT(ct) + 4]) continue;
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_CT(ct) + 2], regs[M_CT(ct) + 3], regs[M_CT(ct) + 4], 0);
        if (sf) { color[i] = sf; ncolor = i + 1; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rts = sf->scale; } }
    }
    if (regs[M_ZT_SELECT] & 1) {
        u64 va = (u64)regs[M_ZT_A] << 32 | regs[M_ZT_A + 1];
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_ZT_SIZE], regs[M_ZT_SIZE + 1], regs[M_ZT_A + 2], 1);
        if (sf) { depth = sf; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rts = sf->scale; } }
    }
    if (!rt_w) { gfx_stat_skipped++; gfx_skip_target++; vk_leave(); return; }

    /* viewport and the clip-space fix-up (as gpu_d3d11.c's) */
    DrawParams params;
    memset(&params, 0, sizeof params);
    float sx = f32_of(regs[M_VIEWPORT_SCALE(0)]), sy = f32_of(regs[M_VIEWPORT_SCALE(0) + 1]), sz = f32_of(regs[M_VIEWPORT_SCALE(0) + 2]);
    float ox = f32_of(regs[M_VIEWPORT_SCALE(0) + 3]), oy = f32_of(regs[M_VIEWPORT_SCALE(0) + 4]), oz = f32_of(regs[M_VIEWPORT_SCALE(0) + 5]);
    int lower_left = regs[M_WINDOW_ORIGIN] & 1;
    if (lower_left) { oy = (float)rt_h - oy; sy = -sy; }
    int minus_one = !(regs[M_VIEWPORT_Z_CLIP] & 1);
    float zmin = minus_one ? oz - sz : oz, zmax = oz + sz;
    zmin = zmin < 0 ? 0 : zmin > 1 ? 1 : zmin;
    zmax = zmax < 0 ? 0 : zmax > 1 ? 1 : zmax;
    float vx = ox - fabsf(sx), vy = oy - fabsf(sy), vw = 2 * fabsf(sx), vh = 2 * fabsf(sy);
    if (vw < 1) { vx = 0; vw = (float)rt_w; }
    if (vh < 1) { vy = 0; vh = (float)rt_h; }
    if (rts != 1.0f) { vx *= rts; vy *= rts; vw *= rts; vh *= rts; }
    params.fix[0] = sx < 0 ? -1.0f : 1.0f;
    params.fix[1] = sy < 0 ? 1.0f : -1.0f;
    params.fix[2] = minus_one ? 0.5f : 1.0f;
    params.fix[3] = minus_one ? 0.5f : 0.0f;
    params.frag[0] = lower_left ? 1.0f : 0.0f;
    params.frag[1] = (float)rt_h;
    params.rtscale[0] = params.rtscale[1] = rts - 1.0f;
    params.rtscale[2] = params.rtscale[3] = 1.0f / rts - 1.0f;
    int flipped = (!lower_left) != ((regs[M_WINDOW_ORIGIN] >> 4) & 1);
    long sc[4] = {0, 0, 16384, 16384};
    if (regs[M_SCISSOR(0)] & 1) {
        u32 h = regs[M_SCISSOR(0) + 1], v = regs[M_SCISSOR(0) + 2];
        sc[0] = h & 0xFFFF; sc[1] = v & 0xFFFF; sc[2] = h >> 16; sc[3] = v >> 16;
        if (lower_left) { long t = (long)rt_h - sc[3]; sc[3] = (long)rt_h - sc[1]; sc[1] = t; }
        if (rts != 1.0f) scale_rect(sc, rts);
    }

    /* the pipeline's fixed state */
    PipelineKey key;
    memset(&key, 0, sizeof key);
    key.vs = vs;
    key.ps = ps;
    key.ncolor = ncolor;
    for (u32 i = 0; i < ncolor; i++) key.color[i] = color[i] ? color[i]->view_format : VK_FORMAT_UNDEFINED;
    key.depth = depth ? depth->view_format : VK_FORMAT_UNDEFINED;
    key.topology = topo;
    key.polygon = regs[M_FILL_MODE] == 0x1B01 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    for (u32 i = 0; i < ncolor; i++) {
        BlendTarget *t = &key.blend[i];
        u32 mask = regs[M_CT_WRITE(i)];
        t->mask = (u8)((mask & 1 ? 1 : 0) | (mask & 0x10 ? 2 : 0) | (mask & 0x100 ? 4 : 0) | (mask & 0x1000 ? 8 : 0));
        if (!color[i]) t->mask = 0;
        t->enable = regs[M_BLEND_ENABLE(i)] & 1;
        if (!t->enable) continue;
        u32 base = regs[M_BLEND_STATE_PER_TARGET] & 1 ? M_BLEND_PER_TARGET(i) + 1 : 0, cop, csrc, cdst, aop, asrc, adst;
        if (base) {
            cop = regs[base]; csrc = regs[base + 1]; cdst = regs[base + 2]; aop = regs[base + 3]; asrc = regs[base + 4]; adst = regs[base + 5];
            if (!(regs[base - 1] & 1)) { aop = cop; asrc = csrc; adst = cdst; }
        } else {
            cop = regs[M_BLEND_COLOR_OP]; csrc = regs[M_BLEND_COLOR_SRC]; cdst = regs[M_BLEND_COLOR_DST];
            aop = regs[M_BLEND_ALPHA_OP]; asrc = regs[M_BLEND_ALPHA_SRC]; adst = regs[M_BLEND_ALPHA_DST];
            if (!(regs[M_BLEND_SEPARATE_ALPHA] & 1)) { aop = cop; asrc = csrc; adst = cdst; }
        }
        t->op = (u8)blend_op(cop); t->src = (u8)blend_factor(csrc); t->dst = (u8)blend_factor(cdst);
        t->aop = (u8)blend_op(aop); t->asrc = (u8)alpha_factor(blend_factor(asrc)); t->adst = (u8)alpha_factor(blend_factor(adst));
    }

    /* (blending and write masks as dynamic state, where the device has it: not in the key) */
    BlendTarget blend[8];
    memcpy(blend, key.blend, sizeof blend);
    if (vk.has.eds3) memset(key.blend, 0, sizeof key.blend);

    /* their libraries compiling (on the compiler threads): the draw is skipped until they are ready */
    if (vk.has.gpl) {
        int vs_ready = library_ready(vs, key.polygon == VK_POLYGON_MODE_LINE), ps_ready = !ps || library_ready(ps, 0);
        if (!vs_ready || !ps_ready) {
            if (vs->lib_state[key.polygon == VK_POLYGON_MODE_LINE] == 3 || (ps && ps->lib_state[0] == 3)) gfx_skip_shader++;
            else vk_stat_compiling_skips++;
            gfx_stat_skipped++;
            vk_leave();
            return;
        }
    }

    /* the resources: vertex streams, each stage's buffers and textures */
    VkDescriptorBufferInfo streams[32], stream_of[32];
    int have_stream[32] = {0};
    u32 stream_delta[32];
    for (u32 n = 0; n < 32; n++) {
        if (!(vs->info.inputs >> n & 1)) continue;
        u32 fmt = regs[M_VERTEX_ATTRIBUTE(n)], j = fmt & 0x1F, st = regs[M_VERTEX_STREAM(j)];
        u64 start = (u64)regs[M_VERTEX_STREAM(j) + 1] << 32 | regs[M_VERTEX_STREAM(j) + 2];
        u64 limit = (u64)regs[M_VERTEX_STREAM_LIMIT(j)] << 32 | regs[M_VERTEX_STREAM_LIMIT(j) + 1];
        params.attr[n][0] = fmt;
        params.attr[n][1] = st & 0xFFF;
        params.attr[n][2] = regs[M_VERTEX_STREAM_INSTANCED(j)] & 1 ? (regs[M_VERTEX_STREAM(j) + 3] ? regs[M_VERTEX_STREAM(j) + 3] : 1) : 0;
        params.attr[n][3] = (st >> 12) & 1 && start;
        if (!params.attr[n][3]) { streams[n] = upload_gpu(0, 0); continue; }
        u64 size = limit >= start ? limit - start + 1 : 0;
        if (size > (64u << 20)) size = 64u << 20;
        if (!have_stream[j]) {
            /* a stream inside data the GPU keeps (compute results) is read from there; one only partly
             * inside gets the guest's data around the GPU's */
            Resident *res = (start & 3) == 0 ? resident_covering(start, start + size) : NULL;
            stream_delta[j] = 0;
            if (res) stream_of[j] = resident_desc(res, start, (u32)size, &stream_delta[j]);
            else stream_of[j] = resident_overlapping(start, (u32)size) ? upload_composed(start, (u32)size) : upload_kept(start, (u32)size);
            have_stream[j] = 1;
        }
        streams[n] = stream_of[j];
        params.attr[n][3] |= stream_delta[j] << 8;   /* (where the stream starts in its descriptor) */
    }
    params.base[0] = regs[M_GLOBAL_BASE_VERTEX];
    params.base[1] = regs[M_GLOBAL_BASE_INSTANCE] + instance_counter;
    Binding b[2];
    memset(b, 0, sizeof b);
    bind_stage(regs, cb, SHADER_VERTEX, vs, &params, &b[0]);
    if (ps) bind_stage(regs, cb, SHADER_PIXEL, ps, &params, &b[1]);

    /* the attributes fetched by the device when they can be (the vertex-input version, once its
     * library is ready: until then the draw reads its streams as before) */
    VertexInput vi;
    u64 vin = 0;
    Shader *vsx = vs;
    if (vertex_inputs(vs, &params, stream_of, stream_delta, &vi, &vin)) {
        Shader *v = vertex_variant(vs, vs_g, cb[STAGE_VERTEX], vin);
        if (v && (!vk.has.gpl || library_ready(v, key.polygon == VK_POLYGON_MODE_LINE))) vsx = v;
    }
    int use_vin = vsx != vs;
    if (use_vin) { key.vs = vsx; vk_stat_vertex_input_draws++; }
    VkPipeline pipeline = get_pipeline(&key, vsx, ps);
    if (!pipeline) { gfx_stat_skipped++; gfx_skip_shader++; vk_leave(); return; }

    /* the images' uses: the targets as attachments, the textures read (a texture that is one of the
     * draw's targets reads 0, as Direct3D unbinds it) */
    if (gpu_wrote) buffer_barrier();   /* (buffers the GPU wrote: compute results, copies) */
    else if (ring_copied) copy_barrier();
    gpu_read = untracked = 1;
    int changed = !rendering || cur_ncolor != ncolor || cur_depth != depth || memcmp(cur_color, color, sizeof color);
    int barriers = 0;
    for (u32 i = 0; i < ncolor; i++)
        if (color[i]) barriers |= use(&color[i]->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (depth) barriers |= use(&depth->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    for (u32 st = 0; st < 2; st++) {
        const Shader *s = st ? ps : vs;
        if (!s) continue;
        for (u32 k = 0; k < s->info.ntex; k++) {
            Img *m = b[st].tex_img[k];
            if (!m) continue;
            int target = 0;
            for (u32 i = 0; i < ncolor; i++) target |= color[i] && &color[i]->img == m;
            target |= depth && &depth->img == m;
            if (target) { b[st].tex[k].imageView = VK_NULL_HANDLE; continue; }
            barriers |= use(m, read_layout(m), st ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }
    if (changed || barriers) {
        suspend_rendering();
        begin_rendering(color, ncolor, depth);
    }
    VkCommandBuffer cmd = vk_cmd();

    /* DrawParams (the last draw's again when they are the same, in this frame's ring), and the
     * descriptor sets */
    static DrawParams last_params;
    static u64 last_params_frame;
    static u32 last_poff;
    u8 *at;
    VkBuffer pbuf;
    u32 poff;
    /* (parameters a draw of this frame wrote already - another pass of the same mesh, say - used again:
     * found by their hash, compared with a copy - the ring's memory is slow to read) */
    static struct { u64 hash, frame; u32 off; DrawParams copy; } seen[2048];
    if (last_params_frame == vk_frame_serial() && !memcmp(&last_params, &params, sizeof params)) poff = last_poff;
    else {
        u64 h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof params / 8; i++) h = (h ^ ((const u64 *)&params)[i]) * 1099511628211ull;
        u32 slot = (u32)(h >> 53);
        u64 frame = vk_frame_serial();
        if (seen[slot].frame == frame && seen[slot].hash == h && !memcmp(&seen[slot].copy, &params, sizeof params))
            poff = seen[slot].off;
        else {
            poff = (u32)ring_alloc_for(RING_PARAMS, sizeof params, uniform_align, &at, &pbuf);
            if (!at) { vk_leave(); return; }
            memcpy(at, &params, sizeof params);
            seen[slot].hash = h;
            seen[slot].frame = vk_frame_serial();   /* (the ring's frame now: an allocation may have begun another) */
            seen[slot].off = poff;
            seen[slot].copy = params;
        }
        last_params = params;
        last_params_frame = vk_frame_serial();
        last_poff = poff;
    }
    const Shader *sh[2] = {vs, ps};
    VkDescriptorSet sets[4];
    write_sets(sh, b, streams, sets);
    if (!rendering) begin_rendering(color, ncolor, depth);   /* (a flush on a full pool ended it) */
    cmd = vk_cmd();
    sets[3] = slots[vk_slot()].params_set;
    if (cur_pipeline != pipeline) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); cur_pipeline = pipeline; }
    if (memcmp(bound_sets, sets, sizeof bound_sets)) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, sets, 1, &poff);
        memcpy(bound_sets, sets, sizeof bound_sets);
    } else
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 1, &sets[3], 1, &poff);

    /* dynamic state, set only when it changes */
    static struct DynState {
        VkViewport vp; VkRect2D scissor; VkCullModeFlags cull; VkFrontFace front; u32 depth_test, depth_write, depth_op, stencil;
        u32 sfront[5], sback[5], two; float bc[4];
        BlendTarget blend[8]; u32 nblend;
        u64 vin_hash;
    } have;
    if (!dyn_valid) { memset(&have, 0xFF, sizeof have); dyn_valid = 1; }
    VkViewport vp = {vx, vy + vh, vw, -vh, zmin, zmax};   /* (Direct3D's clip space: y up, the viewport turned over) */
    if (memcmp(&have.vp, &vp, sizeof vp)) { vkCmdSetViewport(cmd, 0, 1, &vp); have.vp = vp; }
    if (sc[0] < 0) sc[0] = 0;
    if (sc[1] < 0) sc[1] = 0;
    VkRect2D scissor = {{(int32_t)sc[0], (int32_t)sc[1]}, {sc[2] > sc[0] ? (u32)(sc[2] - sc[0]) : 0, sc[3] > sc[1] ? (u32)(sc[3] - sc[1]) : 0}};
    if (memcmp(&have.scissor, &scissor, sizeof scissor)) { vkCmdSetScissor(cmd, 0, 1, &scissor); have.scissor = scissor; }
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (regs[M_CULL_ENABLE] & 1) cull = regs[M_CULL_FACE] == 0x404 ? VK_CULL_MODE_FRONT_BIT : regs[M_CULL_FACE] == 0x405 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK;
    if (have.cull != cull) { vkCmdSetCullMode(cmd, cull); have.cull = cull; }
    VkFrontFace front_face = (regs[M_FRONT_FACE] == 0x901) != flipped ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (have.front != front_face) { vkCmdSetFrontFace(cmd, front_face); have.front = front_face; }
    u32 depth_test = (regs[M_DEPTH_TEST] & 1) && depth, depth_write = (regs[M_DEPTH_WRITE] & 1) && depth, depth_op = compare_op(regs[M_DEPTH_FUNC]);
    if (have.depth_test != depth_test) { vkCmdSetDepthTestEnable(cmd, depth_test); have.depth_test = depth_test; }
    if (have.depth_write != depth_write) { vkCmdSetDepthWriteEnable(cmd, depth_write); have.depth_write = depth_write; }
    if (have.depth_op != depth_op) { vkCmdSetDepthCompareOp(cmd, (VkCompareOp)depth_op); have.depth_op = depth_op; }
    if (dyn_valid == 1) { vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE); dyn_valid = 2; }
    u32 stencil = (regs[M_STENCIL_TEST] & 1) && depth && has_stencil(depth->view_format);
    if (have.stencil != stencil) { vkCmdSetStencilTestEnable(cmd, stencil); have.stencil = stencil; }
    /* (WINDOW_ORIGIN.FLIP_Y swaps the faces for culling, not for the stencil state: with it set, the
     * hardware's back state is what the front faces get, as gpu_d3d11.c found) */
    int swap = (regs[M_WINDOW_ORIGIN] >> 4) & 1, two = regs[M_TWO_SIDED_STENCIL] & 1;
    VkStencilFaceFlags front = swap ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT, back = swap ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
    if (stencil) {   /* (the stencil state matters only with the test on) */
        u32 sf[5] = {stencil_op(regs[M_STENCIL_OP_FAIL]), stencil_op(regs[M_STENCIL_OP_ZPASS]), stencil_op(regs[M_STENCIL_OP_ZFAIL]),
                     compare_op(regs[M_STENCIL_FUNC]), (regs[M_STENCIL_FUNC_MASK] & 0xFF) | (regs[M_STENCIL_MASK] & 0xFF) << 8 | (regs[M_STENCIL_REF] & 0xFF) << 16};
        u32 sb[5] = {stencil_op(regs[M_BACK_STENCIL_OP_FAIL]), stencil_op(regs[M_BACK_STENCIL_OP_ZPASS]), stencil_op(regs[M_BACK_STENCIL_OP_ZFAIL]),
                     compare_op(regs[M_BACK_STENCIL_FUNC]),
                     (regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF) | (regs[M_BACK_STENCIL_MASK] & 0xFF) << 8 | (regs[M_BACK_STENCIL_REF] & 0xFF) << 16};
        if (!two) memcpy(sb, sf, sizeof sb);
        if (memcmp(have.sfront, sf, sizeof sf) || memcmp(have.sback, sb, sizeof sb) || have.two != (u32)swap) {
            const u32 *v[2] = {sf, sb};
            VkStencilFaceFlags faces[2] = {front, back};
            for (int k = 0; k < 2; k++) {
                vkCmdSetStencilOp(cmd, faces[k], (VkStencilOp)v[k][0], (VkStencilOp)v[k][1], (VkStencilOp)v[k][2], (VkCompareOp)v[k][3]);
                vkCmdSetStencilCompareMask(cmd, faces[k], v[k][4] & 0xFF);
                vkCmdSetStencilWriteMask(cmd, faces[k], v[k][4] >> 8 & 0xFF);
                vkCmdSetStencilReference(cmd, faces[k], v[k][4] >> 16 & 0xFF);
            }
            memcpy(have.sfront, sf, sizeof sf);
            memcpy(have.sback, sb, sizeof sb);
            have.two = (u32)swap;
        }
    }
    float bc[4] = {f32_of(regs[M_BLEND_CONST(0)]), f32_of(regs[M_BLEND_CONST(1)]), f32_of(regs[M_BLEND_CONST(2)]), f32_of(regs[M_BLEND_CONST(3)])};
    if (memcmp(have.bc, bc, sizeof bc)) { vkCmdSetBlendConstants(cmd, bc); memcpy(have.bc, bc, sizeof bc); }
    if (vk.has.eds3 && ncolor && (have.nblend != ncolor || memcmp(have.blend, blend, ncolor * sizeof *blend))) {
        VkBool32 enable[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags mask[8];
        for (u32 i = 0; i < ncolor; i++) {
            const BlendTarget *t = &blend[i];
            enable[i] = t->enable;
            eq[i] = (VkColorBlendEquationEXT){t->src, t->dst, t->op, t->asrc, t->adst, t->aop};
            mask[i] = t->mask;
        }
        vkCmdSetColorBlendEnableEXT(cmd, 0, ncolor, enable);
        vkCmdSetColorBlendEquationEXT(cmd, 0, ncolor, eq);
        vkCmdSetColorWriteMaskEXT(cmd, 0, ncolor, mask);
        memcpy(have.blend, blend, ncolor * sizeof *blend);
        have.nblend = ncolor;
    }
    if (vk.has.vertex_input) {   /* (the formats: set when they change; the buffers each draw) */
        u64 h = 1;   /* (no attributes: 1) */
        if (use_vin) {
            h = 1469598103934665603ull ^ vi.nbind << 8 ^ vi.nattr;
            for (u32 i = 0; i < vi.nbind; i++) h = (h ^ vi.bind[i].stride ^ (u64)vi.bind[i].inputRate << 32) * 1099511628211ull;
            for (u32 i = 0; i < vi.nattr; i++) h = (h ^ vi.attr[i].location ^ (u64)vi.attr[i].binding << 8 ^ (u64)vi.attr[i].format << 16 ^ (u64)vi.attr[i].offset << 40) * 1099511628211ull;
            if (h == 1) h = 2;
        }
        if (have.vin_hash != h) {
            vkCmdSetVertexInputEXT(cmd, use_vin ? vi.nbind : 0, vi.bind, use_vin ? vi.nattr : 0, vi.attr);
            have.vin_hash = h;
        }
        if (use_vin && vi.nbind) vkCmdBindVertexBuffers2(cmd, 0, vi.nbind, vi.buf, vi.off, vi.size, NULL);
    }
    if (draw_logging) {
        fprintf(stderr, "[draw] programs: region %#llx vs +%#x ps +%#x\n", (unsigned long long)region, regs[M_PIPELINE(1) + 1], regs[M_PIPELINE(5) + 1]);
        u32 c0 = sel >> 4 & 7;
        fprintf(stderr, "[draw] %s %s rt0 %#llx %ux%u fmt %#x (ct sel %#x) zt %d | vp %.0f,%.0f %.0fx%.0f fix %.2f %.2f %.2f %.2f | "
                "%s %u (first %u) topo %u | mask %#x blend %d | depth %d/%d func %#x | cull %d (en %#x face %#x front %#x origin %#x scale %g %g)\n",
                vs->name, ps ? ps->name : "(no ps)", (unsigned long long)gpu_va_to_guest((u64)regs[M_CT(c0)] << 32 | regs[M_CT(c0) + 1]), rt_w, rt_h,
                regs[M_CT(c0) + 4], sel, depth != NULL, vx, vy, vw, vh, params.fix[0], params.fix[1], params.fix[2], params.fix[3],
                indexed ? "indexed" : "arrays", indexed ? regs[M_DRAW_INDEX_COUNT] : regs[M_VERTEX_ARRAY_COUNT],
                indexed ? regs[M_INDEX_BUFFER + 5] : regs[M_VERTEX_ARRAY_START], begin & 0xFFFF, regs[M_CT_WRITE(0)], blend[0].enable,
                (int)depth_test, regs[M_DEPTH_WRITE] & 1, regs[M_DEPTH_FUNC], (int)cull, regs[M_CULL_ENABLE], regs[M_CULL_FACE], regs[M_FRONT_FACE],
                regs[M_WINDOW_ORIGIN], f32_of(regs[M_VIEWPORT_SCALE(0)]), f32_of(regs[M_VIEWPORT_SCALE(0) + 1]));
        u32 sh = regs[M_SCISSOR(0) + 1], sv = regs[M_SCISSOR(0) + 2];
        fprintf(stderr, "[draw]   scissor %s x %u-%u y %u-%u | stencil %d func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "two-sided %u: back func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "blend src %d dst %d op %d, alpha src %d dst %d op %d\n",
                regs[M_SCISSOR(0)] & 1 ? "on" : "off", sh & 0xFFFF, sh >> 16, sv & 0xFFFF, sv >> 16, (int)stencil, regs[M_STENCIL_FUNC],
                regs[M_STENCIL_REF] & 0xFF, regs[M_STENCIL_FUNC_MASK] & 0xFF, regs[M_STENCIL_MASK] & 0xFF, regs[M_STENCIL_OP_FAIL],
                regs[M_STENCIL_OP_ZFAIL], regs[M_STENCIL_OP_ZPASS], regs[M_TWO_SIDED_STENCIL] & 1, regs[M_BACK_STENCIL_FUNC],
                regs[M_BACK_STENCIL_REF] & 0xFF, regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF, regs[M_BACK_STENCIL_MASK] & 0xFF,
                regs[M_BACK_STENCIL_OP_FAIL], regs[M_BACK_STENCIL_OP_ZFAIL], regs[M_BACK_STENCIL_OP_ZPASS], blend[0].src, blend[0].dst, blend[0].op,
                blend[0].asrc, blend[0].adst, blend[0].aop);
    }

    if (indexed) {
        u32 fmt = regs[M_INDEX_BUFFER + 4], isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4, first = regs[M_INDEX_BUFFER + 5];
        u32 n = regs[M_DRAW_INDEX_COUNT];
        u64 va = ((u64)regs[M_INDEX_BUFFER] << 32 | regs[M_INDEX_BUFFER + 1]) + (u64)first * isz;
        if (!n || n > (16u << 20)) { gfx_stat_skipped++; vk_leave(); return; }
        u32 unit = isz == 4 ? 4 : 2;
        VkBuffer ib;
        VkDeviceSize ioff;
        if (isz != 1) {   /* 16- and 32-bit indices: kept from frame to frame, as the vertex streams (the meshes' own) */
            VkDescriptorBufferInfo d = upload_kept(va, n * isz);
            if (!d.buffer) { vk_leave(); return; }
            ib = d.buffer;
            ioff = d.offset;
            goto indices;
        }
        ioff = ring_alloc_for(RING_INDEX, (VkDeviceSize)n * unit, 4, &at, &ib);
        if (!at) { vk_leave(); return; }
        {   /* 8-bit indices widen to 16 */
            static u8 tmp8[1 << 16];
            for (u32 done = 0; done < n;) {
                u32 k = n - done > sizeof tmp8 ? (u32)sizeof tmp8 : n - done;
                read_gpu(va + done, k, tmp8);
                for (u32 i = 0; i < k; i++) ((u16 *)at)[done + i] = tmp8[i];
                done += k;
            }
        }
    indices:
        if (!rendering) { begin_rendering(color, ncolor, depth); cmd = vk_cmd(); }
        vkCmdBindIndexBuffer(cmd, ib, ioff, isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        gpu_times_before(cmd);
        vkCmdDrawIndexed(cmd, n, 1, 0, (int32_t)regs[M_GLOBAL_BASE_VERTEX], use_vin ? params.base[1] : 0);
    } else {
        gpu_times_before(cmd);
        vkCmdDraw(cmd, regs[M_VERTEX_ARRAY_COUNT], 1, regs[M_VERTEX_ARRAY_START], use_vin ? params.base[1] : 0);
    }
    gpu_times_after(cmd, vsx->name, ps ? ps->name : "-", rt_w, rt_h);
    gfx_stat_drawn++;
    vk_leave();
}

/* ---- compute ------------------------------------------------------------------------------------ */
extern u64 gfx_stat_dispatched;

static VkPipeline compute_pipeline(Shader *s) {
    if (!s->compute) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, s->module, "main", NULL},
                                          layout, VK_NULL_HANDLE, 0};
        if (vkCreateComputePipelines(vk.dev, pipeline_cache, 1, &ci, NULL, &s->compute) != VK_SUCCESS) {
            s->compute = VK_NULL_HANDLE;
            s->failed = 1;
            fprintf(stderr, "[gfx] %s: the device could not make its compute pipeline\n", s->name);
            return VK_NULL_HANDLE;
        }
        vk_stat_pipelines++;
    }
    return s->compute;
}

/* As gpu_d3d11.c's: a program's first runs learn which storage buffers it writes (what it wrote is
 * read back to guest memory, and the buffers that changed are noted); after that, what it writes
 * stays on the GPU (resident regions), and what it only reads comes from there or from the
 * input buffers kept on the GPU. */
void vk_gfx_dispatch(const u32 *regs, const u32 *q) {
    vk_enter();
    init();
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 g = gpu_va_to_guest(region + q[8]);
    u32 local[3] = {q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16};
    u32 grid[3] = {q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16};
    u32 shared = q[17] & 0x3FFFF;
    CbBinding cb[18];
    memset(cb, 0, sizeof cb);
    for (u32 i = 0; i < 8; i++)
        if (q[20] >> i & 1) { cb[i].address = (u64)(q[30 + 2 * i] & 0xFF) << 32 | q[29 + 2 * i]; cb[i].size = q[30 + 2 * i] >> 15; }
    if (!g || !grid[0] || !grid[1] || !grid[2] || !local[0]) { vk_leave(); return; }
    Shader *cs = get_shader(g, SHADER_COMPUTE, 0, cb, local, shared);
    if (!cs || !compute_pipeline(cs)) { gfx_skip_shader++; vk_leave(); return; }
    static const char *skip = (const char *)1;   /* debug: SSBU_VK_SKIP_CS=<name prefixes, commas between> are not run */
    if (skip == (const char *)1) skip = getenv("SSBU_VK_SKIP_CS");
    if (skip) {
        char list[1024];
        snprintf(list, sizeof list, "%s", skip);
        for (char *t = strtok(list, ","); t; t = strtok(NULL, ","))
            if (!strncmp(cs->name, t, strlen(t))) { vk_leave(); return; }
    }
    static int log_new = -1;   /* debug: SSBU_VK_LOG_CS=1 names each compute program at its first run */
    if (log_new < 0) log_new = getenv("SSBU_VK_LOG_CS") != NULL;
    if (log_new && !cs->learn_runs && !cs->logged) {
        cs->logged = 1;
        fprintf(stderr, "[cs] %s: grid %ux%ux%u, local %ux%ux%u, shared %u, %u buffers%s\n", cs->name, grid[0], grid[1], grid[2], local[0], local[1], local[2],
                shared, cs->info.sb_count, cs->info.stores ? ", stores" : "");
    }

    DrawParams params;
    memset(&params, 0, sizeof params);
    Binding b[2];
    memset(b, 0, sizeof b);
    for (u32 k = 0; k < 18; k++) {   /* constant buffers */
        if (!(cs->info.cbuf_mask >> k & 1)) continue;
        u32 size = cb[k].size > 0x10000 ? 0x10000 : cb[k].size;
        if (size > cs->info.cbuf_extent[k]) size = cs->info.cbuf_extent[k];
        b[0].cb[k] = upload_kept(cb[k].address && size ? cb[k].address : 0, size);
    }
    for (u32 k = 0; k < cs->info.ntex; k++)   /* (textures read 0 in a dispatch, as gpu_d3d11.c binds none) */
        b[0].tex[k] = (VkDescriptorImageInfo){default_sampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    static int no_resident = -1;   /* debug: always read results back to guest memory */
    if (no_resident < 0) no_resident = getenv("SSBU_NO_RESIDENT") != NULL;
    int learned = cs->learn_runs >= 8 && !no_resident;   /* until then, what it writes is read back and noted */
    struct { u8 *at, *before; u64 va; u32 size; } rw[16];
    memset(rw, 0, sizeof rw);
    for (u32 k = 0; k < cs->info.sb_count; k++) {   /* storage buffers: their descriptors in constant buffer 0 */
        u32 desc[4] = {0};
        if (cb[0].address) read_gpu(cb[0].address + cs->info.sb_offset[k], 16, desc);
        u64 va = (u64)desc[1] << 32 | desc[0];
        u32 size = desc[2] > (64u << 20) ? 64u << 20 : desc[2];
        params.sb[0][k][0] = desc[0];
        params.sb[0][k][1] = desc[1];
        params.sb[0][k][2] = size;
        if (!va || !size) { b[0].sb[k] = upload_gpu(0, 0); continue; }
        int written = cs->written_mask >> k & 1;
        Resident *over = resident_overlapping(va, size);
        if (learned) {
            Resident *res = written ? resident_acquire(va, size) : over ? resident_covering(va, va + size) : NULL;
            if (res && written) res->written_at = gfx_stat_presented;
            if (res) { b[0].sb[k] = resident_desc(res, va, size, &params.sb[0][k][3]); continue; }
            if (!written && !over && input_buffer(va, size, &b[0].sb[k])) continue;   /* only read: kept */
        } else if (over)
            vk_gfx_resident_flush(va, size);   /* learning compares it all with guest memory, which needs the GPU's data first */
        /* a copy in the ring, the program's to write */
        u32 padded = (size + 15) & ~15u;
        u8 *at;
        VkBuffer rb;
        VkDeviceSize ro = ring_alloc_for(RING_CS, padded, storage_align, &at, &rb);
        if (!at) { b[0].sb[k] = upload_gpu(0, 0); continue; }
        read_gpu(va, size, at);
        memset(at + size, 0, padded - size);
        if (learned && over) resident_patch(rb, ro, va, size);
        b[0].sb[k] = (VkDescriptorBufferInfo){rb, ro, padded};
        sb_bytes += size;
        if (!learned && cs->info.stores) {
            rw[k].at = at;
            rw[k].before = malloc(size);
            memcpy(rw[k].before, at, size);
            rw[k].va = va;
            rw[k].size = size;
        }
    }

    u8 *pat;
    VkBuffer pbuf;
    u32 poff = (u32)ring_alloc_for(RING_PARAMS, sizeof params, uniform_align, &pat, &pbuf);
    if (!pat) { vk_leave(); return; }
    memcpy(pat, &params, sizeof params);
    const Shader *sh[2] = {cs, NULL};
    VkDescriptorSet sets[4];
    write_sets(sh, b, NULL, sets);
    sets[3] = slots[vk_slot()].params_set;
    /* a barrier before it (after the uploads and copies above) unless only dispatches came since the
     * last, and none wrote what this one touches nor read what it writes */
    suspend_rendering();
    int conflict = 0;
    for (u32 k = 0; k < cs->info.sb_count && !conflict; k++) {
        const VkDescriptorBufferInfo *d = &b[0].sb[k];
        int write = !learned || (cs->written_mask >> k & 1);
        for (u32 i = 0; i < naccessed && !conflict; i++)
            conflict = accessed[i].buf == d->buffer && accessed[i].lo < d->offset + d->range && d->offset < accessed[i].hi && (write || accessed[i].write);
    }
    if (untracked || naccessed + cs->info.sb_count > 64) buffer_barrier();
    else {   /* (only dispatches since the last, and copies into the ring: each waited for as it needs) */
        if (ring_copied) copy_barrier();
        if (conflict) compute_barrier();
    }
    for (u32 k = 0; k < cs->info.sb_count; k++) {
        const VkDescriptorBufferInfo *d = &b[0].sb[k];
        accessed[naccessed].buf = d->buffer;
        accessed[naccessed].lo = d->offset;
        accessed[naccessed].hi = d->offset + d->range;
        accessed[naccessed++].write = !learned || (cs->written_mask >> k & 1);
    }
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs->compute);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 4, sets, 1, &poff);
    vkCmdDispatch(cmd, grid[0], grid[1], grid[2]);
    gpu_wrote = gpu_read = 1;
    gfx_stat_dispatched++;
    if (draw_logging)
        fprintf(stderr, "[draw] dispatch %s grid %ux%ux%u, buffers %#llx+%#x %#llx+%#x %#llx+%#x (written %#x)\n", cs->name, grid[0], grid[1], grid[2],
                (unsigned long long)((u64)params.sb[0][0][1] << 32 | params.sb[0][0][0]), params.sb[0][0][2],
                (unsigned long long)((u64)params.sb[0][1][1] << 32 | params.sb[0][1][0]), params.sb[0][1][2],
                (unsigned long long)((u64)params.sb[0][2][1] << 32 | params.sb[0][2][0]), params.sb[0][2][2], cs->written_mask);

    /* while learning, what it wrote goes back to guest memory; the buffers written are noted */
    if (cs->info.stores && !learned) {
        cs->learn_runs++;
        gpu_read_back();
        for (u32 k = 0; k < 16; k++) {
            if (!rw[k].at) continue;
            const u32 *now = (const u32 *)rw[k].at, *was = (const u32 *)rw[k].before;
            int changed = 0;
            for (u32 i = 0; i < rw[k].size / 4; i++) {
                if (now[i] == was[i]) continue;
                changed = 1;
                u64 gg = gpu_va_to_guest(rw[k].va + 4ull * i);
                if (gg) st32(gg, now[i]);
            }
            free(rw[k].before);
            if (changed) {
                cs->written_mask |= 1u << k;
                vk_gfx_resident_written(rw[k].va, rw[k].size);   /* a region over it takes the new data too */
            }
        }
    }
    vk_leave();
}

/* ---- clears ------------------------------------------------------------------------------------- */
#define RT(i, field) regs[0x200 + (i) * 0x10 + (field)]

/* Some channels of a colour surface: a drawn rectangle with a write mask (a built-in pass) */
static void clear_channels(Surface *s, const float color[4], u32 mask) {
    static const char HLSL[] =
        "cbuffer K : register(b0) { float4 kcolor; };\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 ps() : SV_Target { return kcolor; }\n";
    static VkShaderModule vsm, psm;
    static VkPipelineLayout pl;
    static struct { VkFormat f; u32 mask; VkPipeline p; } made[64];
    static u32 nmade;
    if (!vsm) {
        vsm = vk_hlsl(HLSL, "vs", "vs_5_0", 16, "clear");
        psm = vk_hlsl(HLSL, "ps", "ps_5_0", 16, "clear");
        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push};
        vk_check(vkCreatePipelineLayout(vk.dev, &pli, NULL, &pl), "the clear's pipeline layout");
    }
    if (!vsm || !psm) return;
    VkPipeline p = VK_NULL_HANDLE;
    for (u32 i = 0; i < nmade; i++) if (made[i].f == s->view_format && made[i].mask == mask) p = made[i].p;
    if (!p) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", NULL},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psm, "main", NULL}};
        VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState att = {VK_FALSE, 0, 0, 0, 0, 0, 0, mask};
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &att};
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &s->view_format};
        VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vin, &ia, NULL, &vps, &rs, &ms, NULL,
                                           &cbs, &dsi, pl};
        if (vkCreateGraphicsPipelines(vk.dev, pipeline_cache, 1, &pi, NULL, &p) != VK_SUCCESS) return;
        if (nmade < 64) made[nmade++] = (typeof(made[0])){s->view_format, mask, p};
    }
    Surface *only[8] = {s};
    int barrier = use(&s->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (barrier || !rendering || cur_ncolor != 1 || cur_color[0] != s || cur_depth) { suspend_rendering(); begin_rendering(only, 1, NULL); }
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    cur_pipeline = VK_NULL_HANDLE;
    forget_state();
    VkViewport vp = vk_viewport(0, 0, s->pw, s->ph);
    VkRect2D all = {{0, 0}, {s->pw, s->ph}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &all);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, 16, color);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

/* A whole surface cleared: inside the rendering when it is one of its attachments, else on its own */
static void clear_surface(Surface *s, int color_, const float c[4], VkImageAspectFlags aspects, float depth, u32 stencil) {
    if (rendering && attachment_of(&s->img) && !npending) {
        VkClearAttachment a = {aspects, 0, {{{0}}}};
        if (color_) {
            for (u32 i = 0; i < cur_ncolor; i++) if (cur_color[i] == s) a.colorAttachment = i;
            memcpy(a.clearValue.color.float32, c, 16);
        } else a.clearValue.depthStencil = (VkClearDepthStencilValue){depth, stencil};
        VkClearRect r = {{{0, 0}, {s->pw, s->ph}}, 0, 1};
        vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
        return;
    }
    suspend_rendering();
    use(&s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    flush_barriers(vk_cmd());
    VkImageSubresourceRange range = {aspects, 0, 1, 0, 1};
    if (color_) {
        VkClearColorValue v;
        memcpy(v.float32, c, 16);
        vkCmdClearColorImage(vk_cmd(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    } else {
        VkClearDepthStencilValue v = {depth, stencil};
        vkCmdClearDepthStencilImage(vk_cmd(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    }
}

void vk_gfx_clear(const u32 *regs, u32 arg) {
    vk_enter();
    gfx_stat_clears++;
    u32 rt = (arg >> 6) & 0xF;
    if (draw_logging)
        fprintf(stderr, "[draw] clear rt %u (%#llx %ux%u fmt %#x) channels %#x%s, zt %#llx: color %g %g %g %g depth %g stencil %#x\n", rt,
                (unsigned long long)gpu_va_to_guest((u64)RT(rt & 7, 0) << 32 | RT(rt & 7, 1)), RT(rt & 7, 2), RT(rt & 7, 3), RT(rt & 7, 4),
                (arg >> 2) & 0xF, arg & 3 ? (arg & 1 ? " +depth" : " +stencil") : "",
                (unsigned long long)gpu_va_to_guest((u64)regs[0x3F8] << 32 | regs[0x3F9]), f32_of(regs[0x360]), f32_of(regs[0x361]),
                f32_of(regs[0x362]), f32_of(regs[0x363]), f32_of(regs[0x364]), regs[0x368] & 0xFF);
    if (arg & 0x3C && rt < 8) {   /* any of R, G, B, A */
        u64 va = (u64)RT(rt, 0) << 32 | RT(rt, 1);
        Surface *s = surface(gpu_va_to_guest(va), RT(rt, 2), RT(rt, 3), RT(rt, 4), 0);
        if (s) {
            float c[4] = {f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363])};
            if (((arg >> 2) & 0xF) == 0xF) clear_surface(s, 1, c, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
            else clear_channels(s, c, (arg >> 2) & 0xF);
        }
    }
    if (arg & 3) {   /* depth, stencil */
        u64 va = (u64)regs[0x3F8] << 32 | regs[0x3F9];
        Surface *s = surface(gpu_va_to_guest(va), regs[0x48A], regs[0x48B], regs[0x3FA], 1);
        if (s) {
            VkImageAspectFlags a = (arg & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (arg & 2 && has_stencil(s->view_format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (a) clear_surface(s, 0, NULL, a, f32_of(regs[0x364]), regs[0x368] & 0xFF);
        }
    }
    vk_leave();
}

/* ---- the 2D engine's blit ------------------------------------------------------------------------- */
void vk_gfx_blit2d(const u32 *regs) {
    vk_enter();
    u64 src_g = gpu_va_to_guest((u64)regs[0x94] << 32 | regs[0x95]), dst_g = gpu_va_to_guest((u64)regs[0x88] << 32 | regs[0x89]);
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].img.image && !surfaces[i].depth && surfaces[i].guest == src_g) src = &surfaces[i];
    Surface *dst = surface(dst_g, regs[0x86], regs[0x87], regs[0x80], 0);
    if (draw_logging)
        fprintf(stderr, "[draw] 2D blit %#llx (%ux%u fmt %#x, %s) -> %#llx (%ux%u fmt %#x): dst rect %u,%u %ux%u\n", (unsigned long long)src_g, regs[0x92],
                regs[0x93], regs[0x8C], src ? "on the host" : "in memory", (unsigned long long)dst_g, regs[0x86], regs[0x87], regs[0x80], regs[0x22C],
                regs[0x22D], regs[0x22E], regs[0x22F]);
    if (!src || !dst || src == dst) { vk_leave(); return; }   /* (sources only in guest memory: not yet) */
    double du = regs[0x231] + regs[0x230] / 4294967296.0, dv = regs[0x233] + regs[0x232] / 4294967296.0;
    double sx = regs[0x235] + regs[0x234] / 4294967296.0, sy = (i32)regs[0x237] + regs[0x236] / 4294967296.0;
    u32 dx = regs[0x22C], dy = regs[0x22D], dw = regs[0x22E], dh = regs[0x22F];
    float ss = src->scale, ds = dst->scale;
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        {{(int32_t)(sx * ss), (int32_t)(sy * ss), 0}, {(int32_t)((sx + dw * du) * ss), (int32_t)((sy + dh * dv) * ss), 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        {{(int32_t)(dx * ds), (int32_t)(dy * ds), 0}, {(int32_t)((dx + dw) * ds), (int32_t)((dy + dh) * ds), 1}}};
    suspend_rendering();
    use(&src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    use(&dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    flush_barriers(vk_cmd());
    vkCmdBlitImage(vk_cmd(), src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   (regs[0x223] >> 4) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_leave();
}

/* ---- the picture ---------------------------------------------------------------------------------- */
void vk_draw_presenting(void) {
    init();
    static int log_stats = -1;
    static u32 frames;
    if (log_stats < 0) log_stats = getenv("SSBU_LOG_STATS") != NULL;
    if (log_stats && ++frames % 300 == 0) {
        fprintf(stderr, "[vk stats] last 300 frames: kept %llu; not kept: unwatched or large %llu (%.1f MB), arena full %llu (%.1f MB), written %llu (%.1f MB), first %llu (%.1f MB); uploaded %.1f MB\n",
                (unsigned long long)vk_stat_kept_reused, (unsigned long long)kept_miss[0], kept_miss_bytes[0] / 1048576.0, (unsigned long long)kept_miss[1],
                kept_miss_bytes[1] / 1048576.0, (unsigned long long)kept_miss[2], kept_miss_bytes[2] / 1048576.0, (unsigned long long)kept_miss[3],
                kept_miss_bytes[3] / 1048576.0, gfx_stat_upload_bytes / 1048576.0);
        fprintf(stderr, "[vk stats]   ring: textures %.1f MB, uploads %.1f MB, copies %.1f MB, read back %.1f MB, draw parameters %.1f MB, indices %.1f MB, compute buffers %.1f MB\n",
                ring_bytes[RING_TEX] / 1048576.0, ring_bytes[RING_GPU] / 1048576.0, ring_bytes[RING_COPY] / 1048576.0, ring_bytes[RING_READBACK] / 1048576.0,
                ring_bytes[RING_PARAMS] / 1048576.0, ring_bytes[RING_INDEX] / 1048576.0, ring_bytes[RING_CS] / 1048576.0);
        memset(ring_bytes, 0, sizeof ring_bytes);
        fprintf(stderr, "[vk stats]   storage buffers %.1f MB, textures %.1f MB; descriptor sets reused %llu\n", sb_bytes / 1048576.0,
                texture_bytes / 1048576.0, (unsigned long long)vk_stat_sets_reused);
        extern u64 vk_stat_allocs, vk_stat_alloc_bytes, vk_stat_alloc_ns, vk_stat_wait_ns, vk_stat_flushes, vk_stat_flush_waits;
        fprintf(stderr, "[vk stats]   GPU waits %.0f ms; submissions in the middle of a frame %llu (%llu waited for); buffer barriers %llu, compute ones %llu\n",
                vk_stat_wait_ns / 1e6, (unsigned long long)vk_stat_flushes, (unsigned long long)vk_stat_flush_waits, (unsigned long long)vk_stat_buffer_barriers,
                (unsigned long long)vk_stat_compute_barriers);
        vk_stat_wait_ns = vk_stat_flushes = vk_stat_flush_waits = vk_stat_buffer_barriers = vk_stat_compute_barriers = 0;
        static u64 last_composed, last_pages;
        fprintf(stderr, "[vk stats]   write watch: scan %.0f ms, textures %.0f ms, residents %.0f ms (%.0f ranges asked, %llu pages written, list %s)\n",
                watch_split_ns[0] / 1e6, watch_split_ns[1] / 1e6, watch_split_ns[2] / 1e6, (double)watch_span, (unsigned long long)watch_pages,
                watch_ok ? "used" : "not used");
        memset(watch_split_ns, 0, sizeof watch_split_ns);
        fprintf(stderr, "[vk stats]   composed uploads %llu, resident pages taken %llu; draws with vertex inputs %llu (versions looked up %llu)\n",
                (unsigned long long)(gfx_stat_composed - last_composed), (unsigned long long)(gfx_stat_resident_pages - last_pages),
                (unsigned long long)vk_stat_vertex_input_draws, (unsigned long long)vk_stat_variant_lookups);
        vk_stat_variant_lookups = 0;
        vk_stat_vertex_input_draws = 0;
        fprintf(stderr, "[vk stats]   streams read: base instance %llu, attributes %llu, format %llu, divisor %llu, limits %llu, alignment %llu\n",
                (unsigned long long)vin_why[0], (unsigned long long)vin_why[1], (unsigned long long)vin_why[2], (unsigned long long)vin_why[3],
                (unsigned long long)vin_why[4], (unsigned long long)vin_why[5]);
        memset(vin_why, 0, sizeof vin_why);
        last_composed = gfx_stat_composed;
        last_pages = gfx_stat_resident_pages;
        fprintf(stderr, "[vk stats]   device memory: %llu allocations, %.1f MB, %.0f ms; residents made %llu (retired %llu), input buffers made %llu, textures made %llu (images reused %llu)\n",
                (unsigned long long)vk_stat_allocs, vk_stat_alloc_bytes / 1048576.0, vk_stat_alloc_ns / 1e6, (unsigned long long)vk_stat_residents_made,
                (unsigned long long)vk_stat_residents_retired, (unsigned long long)vk_stat_inputs_made, (unsigned long long)vk_stat_textures_made, (unsigned long long)vk_stat_images_reused);
        vk_stat_images_reused = 0;
        vk_stat_allocs = vk_stat_alloc_bytes = vk_stat_alloc_ns = vk_stat_residents_made = vk_stat_inputs_made = vk_stat_textures_made = vk_stat_residents_retired = 0;
        fprintf(stderr, "[vk stats]   shaders translated %llu in %.0f ms (modules %.0f ms); pipelines made %llu, libraries %llu, in %.0f ms; "
                        "links %.0f ms, optimized ones %.0f ms; draws skipped compiling %llu; %ld compiles pending\n",
                (unsigned long long)vk_stat_translated, vk_stat_translate_ns / 1e6, vk_stat_module_ns / 1e6, (unsigned long long)vk_stat_pipelines_made,
                (unsigned long long)vk_stat_libraries_made, vk_stat_pipeline_ns / 1e6, vk_stat_link_ns / 1e6, vk_stat_optimize_ns / 1e6,
                (unsigned long long)vk_stat_compiling_skips, compiles_pending);
        vk_stat_translated = vk_stat_pipelines_made = vk_stat_translate_ns = vk_stat_module_ns = vk_stat_pipeline_ns = 0;
        vk_stat_libraries_made = vk_stat_link_ns = vk_stat_optimize_ns = vk_stat_compiling_skips = 0;
        sb_bytes = texture_bytes = 0;
        vk_stat_sets_reused = 0;
        vk_stat_kept_reused = 0;
        memset(kept_miss, 0, sizeof kept_miss);
        memset(kept_miss_bytes, 0, sizeof kept_miss_bytes);
        gfx_stat_upload_bytes = 0;
    }
    note_writes();
    residents_retire();
    gpu_times_frame();
    release_graves();
    blocks_release();
}

int vk_draw_picture(VkCommandBuffer cb, VkImage dst, u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, u32 fx, u32 fy, u32 fw, u32 fh) {
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].img.image && !surfaces[i].depth && surfaces[i].guest == guest) src = &surfaces[i];
    if (!src) return 0;
    /* the part of the surface that is the picture: all of it, or the crop (in the game's pixels) */
    int cropped = cw && ch && cx + cw <= src->width && cy + ch <= src->height;
    float s = src->scale;
    u32 px = cropped ? (u32)(cx * s + 0.5f) : 0, py = cropped ? (u32)(cy * s + 0.5f) : 0;
    u32 pw = cropped ? (u32)(cw * s + 0.5f) : src->pw, ph = cropped ? (u32)(ch * s + 0.5f) : src->ph;
    use(&src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    flush_barriers(cb);
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{(int32_t)px, (int32_t)py, 0}, {(int32_t)(px + pw), (int32_t)(py + ph), 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{(int32_t)fx, (int32_t)fy, 0}, {(int32_t)(fx + fw), (int32_t)(fy + fh), 1}}};
    /* (the bytes as they are: the image's own format is the plain one, as the display shows sRGB bytes) */
    vkCmdBlitImage(cb, src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    return 1;
}

int vk_draw_picture_view(VkCommandBuffer cb, u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, VkImageView *view, u32 size[2], u32 pic[4]) {
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].img.image && !surfaces[i].depth && surfaces[i].guest == guest) src = &surfaces[i];
    if (!src) return 0;
    int cropped = cw && ch && cx + cw <= src->width && cy + ch <= src->height;
    float s = src->scale;
    pic[0] = cropped ? (u32)(cx * s + 0.5f) : 0;
    pic[1] = cropped ? (u32)(cy * s + 0.5f) : 0;
    pic[2] = cropped ? (u32)(cw * s + 0.5f) : src->pw;
    pic[3] = cropped ? (u32)(ch * s + 0.5f) : src->ph;
    size[0] = src->pw;
    size[1] = src->ph;
    use(&src->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    flush_barriers(cb);
    *view = surface_sampled(src, 0, 0);   /* (the bytes as they are, as the copy without FSR) */
    return 1;
}

/* The picture as the game drew it (no overlay), at the game's resolution, into a 24-bit BMP at path,
 * as gpu_d3d11.c's captures: blitted into an RGBA8 image (any format, any scale), read back. Submits
 * the frame's work so far and waits for it. 0 if there is no picture. */
int vk_draw_capture(u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, const char *path) {
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].img.image && !surfaces[i].depth && surfaces[i].guest == guest) src = &surfaces[i];
    if (!src) return 0;
    unsigned w, h;
    display_screen_size(&w, &h);
    static Img cap;
    static VkBuffer buf;
    static VkDeviceMemory buf_mem;
    static u8 *buf_map;
    if (!cap.image || cap.width != w || cap.height != h) {
        if (cap.image) {
            vk_flush(1);
            vkDestroyImage(vk.dev, cap.image, NULL);
            vkFreeMemory(vk.dev, cap.memory, NULL);
            vkDestroyBuffer(vk.dev, buf, NULL);
            vkFreeMemory(vk.dev, buf_mem, NULL);
        }
        if (!make_image(&cap, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
                        NULL, 0))
            return 0;
        cap.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        void *map;
        buf = vk_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &buf_mem, &map);
        buf_map = map;
    }
    int cropped = cw && ch && cx + cw <= src->width && cy + ch <= src->height;
    float s = src->scale;
    int32_t px = cropped ? (int32_t)(cx * s + 0.5f) : 0, py = cropped ? (int32_t)(cy * s + 0.5f) : 0;
    int32_t pw = cropped ? (int32_t)(cw * s + 0.5f) : (int32_t)src->pw, ph = cropped ? (int32_t)(ch * s + 0.5f) : (int32_t)src->ph;
    suspend_rendering();
    VkCommandBuffer cb = vk_cmd();
    use(&src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cap.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    cap.stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    cap.access = 0;
    use(&cap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    flush_barriers(cb);
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{px, py, 0}, {px + pw, py + ph, 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    use(&cap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    flush_barriers(cb);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, cap.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    vk_flush(1);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    u32 row = w * 3, pad = (4 - row % 4) % 4, data = (row + pad) * h;
    u8 hdr[54] = {'B', 'M'};
    *(u32 *)(hdr + 2) = 54 + data; *(u32 *)(hdr + 10) = 54; *(u32 *)(hdr + 14) = 40;
    *(i32 *)(hdr + 18) = (i32)w; *(i32 *)(hdr + 22) = -(i32)h; *(u16 *)(hdr + 26) = 1; *(u16 *)(hdr + 28) = 24;
    *(u32 *)(hdr + 34) = data;
    fwrite(hdr, 1, 54, f);
    u8 *line = malloc(row + pad + 4);
    for (u32 y = 0; y < h; y++) {
        const u8 *p = buf_map + (size_t)y * w * 4;
        for (u32 x = 0; x < w; x++) { line[x * 3] = p[x * 4 + 2]; line[x * 3 + 1] = p[x * 4 + 1]; line[x * 3 + 2] = p[x * 4]; }
        memset(line + row, 0, pad);
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
    return 1;
}

void vk_draw_describe(char *out, size_t n) {
    snprintf(out, n, "Vulkan: draws %llu  skipped %llu  uploads reused %llu\nshaders %llu  pipelines %llu  textures %u  surfaces %u", (unsigned long long)gfx_stat_drawn,
             (unsigned long long)gfx_stat_skipped, (unsigned long long)vk_stat_kept_reused, (unsigned long long)gfx_stat_shaders, (unsigned long long)vk_stat_pipelines, ntextures, nsurfaces);
}

/* The command stream wrote guest memory at [va, va + bytes): kept uploads there are taken again. */
void vk_gfx_stream_wrote(u64 va, u64 bytes) {
    if (!bytes) return;
    u64 epoch = __atomic_add_fetch(&stream_epoch, 1, __ATOMIC_ACQ_REL);
    for (u64 b = va >> 16; b <= (va + bytes - 1) >> 16; b++) block_written[KEPT_BLOCK(b)] = epoch;
}
void vk_gfx_stream_end(void) {}
int vk_gfx_draw_logging(void) { return draw_logging; }

/* F4 (gpu_vk_frame.c): the draws of the next two presents' worth - the game presents in the middle
 * of drawing a frame, so one would miss half - to the log */
void vk_draw_log_begin(void) {
    draw_logging = 2;
    fprintf(stderr, "[draw] -- frame start (F4)\n");
}
void vk_draw_log_present(u64 guest) {
    if (!draw_logging) return;
    fprintf(stderr, "[draw] -- frame presented from %#llx\n", (unsigned long long)guest);
    draw_logging--;
}
