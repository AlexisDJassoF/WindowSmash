/* The Vulkan renderer's own declarations, shared by its files (linux/gpu_vulkan.c: instance,
 * device and the gfx_* table; gpu_vk_frame.c: the swap chain, frames and the start-up screens;
 * gpu_vk_shaders.c: the built-in passes' shaders; gpu_vk_overlay.c: the overlay). */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <pthread.h>

#include "host.h"

/* ---- functions, loaded from the instance and the device ----------------------------- */
#define VK_INSTANCE_LEVEL_FNS(X) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE_FNS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueWaitIdle) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdCopyBufferToImage) X(vkCmdClearColorImage) X(vkCmdClearAttachments) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdBlitImage) X(vkCmdClearDepthStencilImage) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetBlendConstants) \
    X(vkCmdSetDepthBoundsTestEnable) X(vkResetDescriptorPool) X(vkCreatePipelineCache) X(vkCmdCopyBuffer) X(vkCreateComputePipelines) \
    X(vkCmdDispatch) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) X(vkGetQueryPoolResults) \
    X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) X(vkCmdSetColorWriteMaskEXT) X(vkCmdCopyImageToBuffer) X(vkCmdSetVertexInputEXT) X(vkCmdBindVertexBuffers2)

#define VK_EXTERN(name) extern PFN_##name name;
VK_INSTANCE_LEVEL_FNS(VK_EXTERN)
VK_DEVICE_FNS(VK_EXTERN)

/* ---- the device --------------------------------------------------------------------- */
typedef struct Vk {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice pd;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkSemaphore timeline;      /* counts the submissions the GPU has finished */
    u64 submitted;             /* the timeline's value of the last submission */
    struct { int eds3, gpl, unified_layouts, descriptor_buffer, custom_border, swapchain_maintenance, vertex_input; } has;
} Vk;
extern Vk vk;

/* The renderer's lock (recursive): every gfx_* call that touches the device holds it. */
extern pthread_mutex_t vk_lock;
extern volatile DWORD vk_lock_thread, vk_last_enter_thread;
void vk_enter(void);
void vk_leave(void);

/* VkResult as text, and a failure that leaves the renderer unusable (the log says what, the
 * program ends: the device is lost or out of memory). */
const char *vk_result(VkResult r);
void vk_check(VkResult r, const char *what);

/* Memory of one of the types in type_bits with these properties (0 for none), its size given. */
VkDeviceMemory vk_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want);

/* A buffer of size bytes for usage, in memory with want; *mapped its address when the memory is
 * host-visible (else NULL). */
VkBuffer vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkDeviceMemory *memory, void **mapped);

/* Commands run now and waited for (start-up uploads): begin returns a command buffer to record
 * into, end submits it and waits until the GPU has done it. */
VkCommandBuffer vk_once_begin(void);
void vk_once_end(VkCommandBuffer cb);

/* The timeline: wait until the GPU has finished submission value. */
void vk_wait(u64 value);

/* One image barrier (synchronization2), for the whole colour image. */
void vk_image_barrier(VkCommandBuffer cb, VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                      VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to);

/* ---- the built-in passes' shaders (gpu_vk_shaders.c) ---------------------------------- */
/* The viewport for a built-in pass's rectangle (x, y, w x h). Their shaders are Direct3D's, whose
 * clip space has y up; Vulkan's has it down: the viewport turns it over (a negative height). */
static inline VkViewport vk_viewport(unsigned x, unsigned y, unsigned w, unsigned h) {
    return (VkViewport){(float)x, (float)(y + h), (float)w, -(float)h, 0, 1};
}
/* HLSL (Shader Model 5) to a Vulkan shader module, by vkd3d-shader. Registers map as follows:
 * cbuffer b0 to push constants (bytes [0, push_size)), Texture t<i> to set 0 binding 2i (sampled
 * image), SamplerState s<i> to set 0 binding 2i + 1. NULL (and why, in the log) if it fails. */
VkShaderModule vk_hlsl(const char *src, const char *entry, const char *profile, unsigned push_size, const char *what);

/* ---- the swap chain and frames (gpu_vk_frame.c) --------------------------------------- */
#define VK_FRAMES 3
typedef struct VkFrameTarget {
    VkCommandBuffer cb;
    VkImage image;             /* the swap chain's image this frame draws into */
    VkImageView view;
    unsigned w, h;
    unsigned slot;             /* which of the VK_FRAMES frames in flight */
    u64 serial;                /* frames begun so far, this one included */
} VkFrameTarget;
extern VkFormat vk_swapchain_format;

/* The frame's command buffer, recording: from the first use after a present to the next present,
 * in the next frame slot (vk_slot), begun once the GPU is done with what that slot held. */
VkCommandBuffer vk_cmd(void);
unsigned vk_slot(void);
u64 vk_frame_serial(void);      /* the frame being recorded: unique, rising */
/* What is recorded, submitted without a present (wait: and waited for); recording goes on in a new
 * command buffer. */
void vk_flush(int wait);

/* A frame into the window: 0 if there is none to draw (the window is minimised, or the swap chain
 * cannot be made). vsync: presented at the next refresh (FIFO) or as soon as it is ready.
 * acquire gives the image (undefined contents, nothing begun), render begins rendering into it:
 * cleared to clear, or (clear NULL) keeping what transfers wrote (TRANSFER_DST layout). begin is
 * both, with a clear. */
int vk_frame_acquire(VkFrameTarget *t, int vsync);
void vk_frame_render(VkFrameTarget *t, const float clear[4]);
int vk_frame_begin(VkFrameTarget *t, int vsync, const float clear[4]);
/* Ends rendering, submits the frame and presents it. */
void vk_frame_end(VkFrameTarget *t);

/* ---- the game's drawing (gpu_vk_draw.c) -------------------------------------------------- */
void vk_draw_slot_start(unsigned slot);   /* a frame slot starts again: its memory is free */
void vk_draw_suspend(void);               /* the game's rendering ends (before other work in the command buffer) */
void vk_draw_presenting(void);            /* at each present, before the frame: CPU writes noted, old objects freed */
/* The game's surface at guest (its crop, if any) into the image (TRANSFER_DST) at the fit
 * rectangle: 0 if there is no such surface. */
int vk_draw_picture(VkCommandBuffer cb, VkImage dst, u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, u32 fx, u32 fy, u32 fw, u32 fh);
void vk_draw_describe(char *out, size_t n);   /* the overlay's details */

/* ---- the overlay (gpu_vk_overlay.c) ---------------------------------------------------- */
/* The quads gpu_overlay_quads.c built last, drawn over the frame's image at (x, y, w x h)
 * (rendering into it has begun). */
void vk_overlay_draw(VkFrameTarget *t, unsigned x, unsigned y, unsigned w, unsigned h);

/* ---- FSR (gpu_vk_fsr.c) -------------------------------------------------------------------- */
/* EASU, before the window's rendering begins: the picture at guest (crop: as gfx_present's) into
 * FSR's texture at fit_w x fit_h. 0 if FSR cannot (or need not: not enlarged): the picture is then
 * copied as without FSR. RCAS, in the window's rendering: that texture, sharpened, at the fit. */
int vk_fsr_easu(VkFrameTarget *t, u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, unsigned fit_w, unsigned fit_h);
void vk_fsr_rcas(VkFrameTarget *t, unsigned fit_x, unsigned fit_y, unsigned fit_w, unsigned fit_h);
/* (gpu_vk_draw.c) The picture's surface for a pass that samples it: its plain view, readable from
 * the fragment stage, its size, and the part that is the picture (x, y, w, h). 0 if there is none. */
/* (gpu_vk_draw.c) The picture (no overlay) at the game's resolution into a BMP; F4's draw log, begun
 * and counted down at each present */
int vk_draw_capture(u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, const char *path);
void vk_draw_log_begin(void);
void vk_draw_log_present(u64 guest);
int vk_draw_picture_view(VkCommandBuffer cb, u64 guest, u32 cx, u32 cy, u32 cw, u32 ch, VkImageView *view, u32 size[2], u32 pic[4]);
