/* The Vulkan renderer's frames: the swap chain on the window's surface, the frames in flight (a
 * command buffer and an acquire semaphore each, the timeline saying when the GPU is done with
 * one), the start-up's screens (the settings menu, the shaders' screen) and the game's presents.
 *
 * The swap chain is made again when the window's size changes (display_take_size), when Vulkan
 * says it is out of date, and when the frame asks for the other kind of presenting: a refresh
 * apart (FIFO; the start-up's screens, pacing = vsync) or as soon as it is ready (IMMEDIATE, else
 * MAILBOX), which the clock pacing of gpu_display.c wants. As many frames as display.ini's latency
 * may be queued on the GPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "gpu_vk.h"
#include <SDL.h>
#include <SDL_vulkan.h>

#include "debug_profile.h"
#include "display.h"
#include "gpu_overlay_quads.h"

void *rt_window(void);
void watchdog_present(void);                 /* debug_watchdog.c */
extern u64 gfx_stat_presented;   /* (gpu_d3d11.c's: the status lines read it) */
extern volatile LONG vk_hotkeys;             /* gpu_vulkan.c */

VkFormat vk_swapchain_format = VK_FORMAT_UNDEFINED;
static VkSwapchainKHR swapchain;
static int swapchain_vsync;
static VkExtent2D extent;
static uint32_t nimages;
static VkImage images[8];
static VkImageView views[8];
static VkSemaphore presented[8];   /* per image: the frame drawn into it is done (the present waits) */
static int stale;                  /* make the swap chain again before the next frame */

static struct {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkSemaphore acquired;
} frames[VK_FRAMES];
static u64 frame_value[VK_FRAMES];   /* the timeline value of each slot's last submission */
static u64 serial;                   /* frames begun */
static uint32_t image_index;

static void make_frames(void) {
    for (unsigned i = 0; i < VK_FRAMES; i++) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vk.family};
        vk_check(vkCreateCommandPool(vk.dev, &pi, NULL, &frames[i].pool), "a frame's command pool");
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, frames[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        vk_check(vkAllocateCommandBuffers(vk.dev, &ai, &frames[i].cb), "a frame's command buffer");
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        vk_check(vkCreateSemaphore(vk.dev, &si, NULL, &frames[i].acquired), "a frame's semaphore");
    }
}

static VkPresentModeKHR present_mode(int vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR modes[16];
    uint32_t n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.pd, vk.surface, &n, modes);
    int mailbox = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

/* The swap chain for the window as it is now: 0 if the window has no area (minimised). */
static int make_swapchain(int vsync) {
    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.pd, vk.surface, &caps), "the surface's capabilities");
    VkExtent2D want = caps.currentExtent;
    if (want.width == UINT32_MAX) {   /* (the surface takes the swap chain's size: the window's drawable) */
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(rt_window(), &w, &h);
        want.width = (uint32_t)w; want.height = (uint32_t)h;
        if (want.width < caps.minImageExtent.width) want.width = caps.minImageExtent.width;
        if (want.height < caps.minImageExtent.height) want.height = caps.minImageExtent.height;
        if (want.width > caps.maxImageExtent.width) want.width = caps.maxImageExtent.width;
        if (want.height > caps.maxImageExtent.height) want.height = caps.maxImageExtent.height;
    }
    if (!want.width || !want.height) return 0;

    if (vk_swapchain_format == VK_FORMAT_UNDEFINED) {   /* (one format for the renderer's life: the pipelines are made for it) */
        VkSurfaceFormatKHR formats[64];
        uint32_t n = 64;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.pd, vk.surface, &n, formats);
        vk_swapchain_format = n ? formats[0].format : VK_FORMAT_B8G8R8A8_UNORM;
        for (uint32_t i = 0; i < n; i++)   /* (plain 8-bit UNORM, as Direct3D's back buffer: the game's bytes are already sRGB) */
            if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                vk_swapchain_format = formats[i].format;
                break;
            }
    }
    uint32_t count = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, vk.surface, count, vk_swapchain_format,
                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, want, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                   caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, present_mode(vsync), VK_TRUE, old};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    vkDeviceWaitIdle(vk.dev);   /* (the old images and views go: nothing may still draw into them) */
    VkResult r = vkCreateSwapchainKHR(vk.dev, &ci, NULL, &swapchain);
    for (uint32_t i = 0; i < nimages; i++) {
        vkDestroyImageView(vk.dev, views[i], NULL);
        vkDestroySemaphore(vk.dev, presented[i], NULL);
    }
    nimages = 0;
    if (old) vkDestroySwapchainKHR(vk.dev, old, NULL);
    if (r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        fprintf(stderr, "[gfx] Vulkan: cannot make the swap chain (%ux%u): %s\n", want.width, want.height, vk_result(r));
        return 0;
    }
    nimages = 8;
    vkGetSwapchainImagesKHR(vk.dev, swapchain, &nimages, images);
    for (uint32_t i = 0; i < nimages; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, images[i], VK_IMAGE_VIEW_TYPE_2D, vk_swapchain_format,
                                    {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vk_check(vkCreateImageView(vk.dev, &vi, NULL, &views[i]), "a swap chain image's view");
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        vk_check(vkCreateSemaphore(vk.dev, &si, NULL, &presented[i]), "a swap chain image's semaphore");
    }
    extent = want;
    swapchain_vsync = vsync;
    stale = 0;
    static const char *const names[] = {"immediate", "mailbox", "FIFO", "FIFO relaxed"};
    fprintf(stderr, "[gfx] Vulkan swap chain: %ux%u, %u images, %s\n", want.width, want.height, nimages,
            ci.presentMode <= 3 ? names[ci.presentMode] : "?");
    return 1;
}

/* ---- the frame's command buffer ------------------------------------------------------------ */
/* Everything of a frame is recorded into one command buffer, from the first draw after the last
 * present to the next present, which submits it. vk_cmd begins it in the next frame slot, once the
 * GPU is done with what that slot held. */
static int recording;
static unsigned cur_slot;

VkCommandBuffer vk_cmd(void) {
    if (!frames[0].pool) make_frames();
    if (!recording) {
        unsigned slot = (unsigned)(serial % VK_FRAMES);
        /* at most display.ini's latency frames queued: wait for the one that many before this */
        unsigned latency = (unsigned)display_latency();
        if (latency > VK_FRAMES) latency = VK_FRAMES;
        if (serial >= latency) vk_wait(frame_value[(serial - latency) % VK_FRAMES]);
        vk_wait(frame_value[slot]);
        vkResetCommandPool(vk.dev, frames[slot].pool, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        vkBeginCommandBuffer(frames[slot].cb, &bi);
        recording = 1;
        cur_slot = slot;
        vk_draw_slot_start(slot);
    }
    return frames[cur_slot].cb;
}

unsigned vk_slot(void) { vk_cmd(); return cur_slot; }
u64 vk_frame_serial(void) { return serial + 1; }

u64 vk_stat_flushes, vk_stat_flush_waits;   /* (SSBU_LOG_STATS: submissions in the middle of a frame) */
void vk_flush(int wait) {
    if (!recording) return;
    vk_stat_flushes++;
    vk_stat_flush_waits += wait != 0;
    vk_draw_suspend();
    vkEndCommandBuffer(frames[cur_slot].cb);
    u64 value = ++vk.submitted;
    frame_value[cur_slot] = value;
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, frames[cur_slot].cb, 0};
    VkSemaphoreSubmitInfo done = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vk.timeline, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 1, &done};
    vk_check(vkQueueSubmit2(vk.queue, 1, &si, VK_NULL_HANDLE), "submitting work");
    recording = 0;
    serial++;
    if (wait) vk_wait(value);
}

int vk_frame_acquire(VkFrameTarget *t, int vsync) {
    VkCommandBuffer cb = vk_cmd();
    unsigned bw, bh;
    if (display_take_size(&bw, &bh)) stale = 1;
    if ((!swapchain || stale || vsync != swapchain_vsync) && !make_swapchain(vsync)) return 0;
    VkResult r = vkAcquireNextImageKHR(vk.dev, swapchain, UINT64_MAX, frames[cur_slot].acquired, VK_NULL_HANDLE, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!make_swapchain(vsync)) return 0;
        r = vkAcquireNextImageKHR(vk.dev, swapchain, UINT64_MAX, frames[cur_slot].acquired, VK_NULL_HANDLE, &image_index);
    }
    if (r == VK_SUBOPTIMAL_KHR) stale = 1;
    else if (r != VK_SUCCESS) { stale = 1; return 0; }
    vk_draw_suspend();
    *t = (VkFrameTarget){cb, images[image_index], views[image_index], extent.width, extent.height, cur_slot, serial + 1};
    return 1;
}

void vk_frame_render(VkFrameTarget *t, const float clear[4]) {
    if (clear)
        vk_image_barrier(t->cb, t->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    else   /* (after transfers into it: kept) */
        vk_image_barrier(t->cb, t->image, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, t->view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                       clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
    if (clear) memcpy(color.clearValue.color.float32, clear, 4 * sizeof(float));
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {t->w, t->h}}, 1, 0, 1, &color, NULL, NULL};
    vkCmdBeginRendering(t->cb, &ri);
}

int vk_frame_begin(VkFrameTarget *t, int vsync, const float clear[4]) {
    if (!vk_frame_acquire(t, vsync)) return 0;
    vk_frame_render(t, clear);
    return 1;
}

void vk_frame_end(VkFrameTarget *t) {
    vkCmdEndRendering(t->cb);
    vk_image_barrier(t->cb, t->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(t->cb);
    u64 value = ++vk.submitted;
    frame_value[t->slot] = value;
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, frames[t->slot].acquired, 0,
                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSemaphoreSubmitInfo signal[2] = {
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, presented[image_index], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vk.timeline, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
    };
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, t->cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &wait, 1, &cbi, 2, signal};
    vk_check(vkQueueSubmit2(vk.queue, 1, &si, VK_NULL_HANDLE), "submitting a frame");
    recording = 0;
    serial++;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &presented[image_index], 1, &swapchain, &image_index, NULL};
    VkResult r = vkQueuePresentKHR(vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) stale = 1;
    else if (r != VK_SUCCESS) vk_check(r, "presenting a frame");
}

/* ---- the start-up's screens ------------------------------------------------------------- */
static const float dark[4] = {0.06f, 0.06f, 0.07f, 1};

int vk_gfx_progress(const char *title, unsigned done, unsigned total, const char *status) {
    VkFrameTarget t;
    vk_enter();
    if (!vk_frame_begin(&t, 1, dark)) { vk_leave(); return 0; }
    overlay_build_progress(t.w, t.h, title, done, total, status);
    vk_overlay_draw(&t, 0, 0, t.w, t.h);
    vk_frame_end(&t);
    vk_leave();
    return 1;
}

int vk_gfx_menu(const char *title, int n, const char *const labels[], const char *const values[], const int shown[], int selected, int button,
                const char *footer, float *rects, float *value_x) {
    VkFrameTarget t;
    vk_enter();
    if (!vk_frame_begin(&t, 1, dark)) { vk_leave(); return 0; }
    overlay_build_menu(t.w, t.h, title, n, labels, values, shown, selected, button, footer, rects, value_x);
    vk_overlay_draw(&t, 0, 0, t.w, t.h);
    vk_frame_end(&t);
    vk_leave();
    return 1;
}

/* ---- the game's presents ------------------------------------------------------------------- */
/* The game's picture (its surface at guest, the crop when it is one) fitted into the window, 16:9
 * and centred, black around it; grey where the picture would be when there is none yet. Then the
 * overlay, F7's notice and a window's mode bar. */
/* SSBU_CAPTURE=<dir>: the picture every SSBU_CAPTURE_EVERY seconds (3), at most SSBU_CAPTURE_MAX (40),
 * as frame_<n>.bmp - gpu_d3d11.c's captures */
static void capture(u64 guest, u32 cx, u32 cy, u32 cw, u32 ch) {
    static const char *dir;
    static int checked, count, most = 40;
    static u64 last, every = 3000000000ull;
    if (!checked++) {
        dir = getenv("SSBU_CAPTURE");
        if (getenv("SSBU_CAPTURE_MAX")) most = atoi(getenv("SSBU_CAPTURE_MAX"));
        if (getenv("SSBU_CAPTURE_EVERY")) every = (u64)(atof(getenv("SSBU_CAPTURE_EVERY")) * 1e9);
    }
    if (!dir || count >= most || rt_now_ns() - last < every) return;
    last = rt_now_ns();
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%03d.bmp", dir, count);
    if (vk_draw_capture(guest, cx, cy, cw, ch, path)) count++;
}

void vk_gfx_present(u64 guest, u32 crop_x, u32 crop_y, u32 crop_w, u32 crop_h) {
    static const float black[4] = {0, 0, 0, 1};
    VkFrameTarget t;
    vk_enter();
    watchdog_present();
    hitch_present();
    gfx_stat_presented++;
    vk_draw_log_present(guest);
    vk_draw_presenting();
    /* the debug keys that save the picture, before the window's frame: F3 a screenshot, F4 the next
     * frame's draws to the log (its picture beside); and SSBU_CAPTURE's every few seconds */
    LONG keys = InterlockedExchange(&vk_hotkeys, 0);
    static double f4_at = -1;   /* SSBU_F4_AT: the second to press F4 at (debug runs) */
    if (f4_at == -1) f4_at = getenv("SSBU_F4_AT") ? atof(getenv("SSBU_F4_AT")) : -2;
    if (f4_at >= 0 && rt_now_ns() > (u64)(f4_at * 1e9)) { f4_at = -2; keys |= 8; }
    if (keys & (4 | 8)) {
        char path[160], stamp[40];
        time_t now = time(NULL);
        strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&now));
        mkdir("screenshots", 0755);
        if (keys & 4) {
            snprintf(path, sizeof path, "screenshots/ssbu_%s.bmp", stamp);
            if (vk_draw_capture(guest, crop_x, crop_y, crop_w, crop_h, path)) fprintf(stderr, "[gfx] screenshot: %s\n", path);
        }
        if (keys & 8) {
            snprintf(path, sizeof path, "screenshots/frame_%s", stamp);
            mkdir(path, 0755);
            strncat(path, "/screen.bmp", sizeof path - strlen(path) - 1);
            vk_draw_capture(guest, crop_x, crop_y, crop_w, crop_h, path);
            vk_draw_log_begin();
        }
    }
    capture(guest, crop_x, crop_y, crop_w, crop_h);
    if (!vk_frame_acquire(&t, display_sync_interval() != 0)) {   /* (no window to show it in: the work still goes) */
        vk_flush(0);
        vk_leave();
        return;
    }
    vk_image_barrier(t.cb, t.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue cv;
    memcpy(cv.float32, black, sizeof black);
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(t.cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &all);
    unsigned fit_x, fit_y, fit_w, fit_h;
    display_fit(t.w, t.h, &fit_x, &fit_y, &fit_w, &fit_h);
    vk_image_barrier(t.cb, t.image, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int fsr = display_upscale() == UPSCALE_FSR && vk_fsr_easu(&t, guest, crop_x, crop_y, crop_w, crop_h, fit_w, fit_h);
    int pictured = fsr || (fit_w && fit_h && vk_draw_picture(t.cb, t.image, guest, crop_x, crop_y, crop_w, crop_h, fit_x, fit_y, fit_w, fit_h));
    vk_frame_render(&t, NULL);
    if (fsr) vk_fsr_rcas(&t, fit_x, fit_y, fit_w, fit_h);
    if (!pictured && fit_w && fit_h) {
        VkClearAttachment grey = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0.1f, 0.1f, 0.12f, 1}}}};
        VkClearRect where = {{{(int32_t)fit_x, (int32_t)fit_y}, {fit_w, fit_h}}, 0, 1};
        vkCmdClearAttachments(t.cb, 1, &grey, 1, &where);
    }

    /* debug keys (F1 the overlay, F2 its details) and the overlay */
    static int overlay = -1, details;
    if (overlay < 0) overlay = !getenv("SSBU_OVERLAY") || atoi(getenv("SSBU_OVERLAY")) != 0;
    if (keys & 1) overlay = !overlay;
    if (keys & 2) { details = !details; overlay = 1; }
    static double profile_at = -1;   /* SSBU_PROFILE_AT: the second to start a profile at (debug runs) */
    if (profile_at == -1) profile_at = getenv("SSBU_PROFILE_AT") ? atof(getenv("SSBU_PROFILE_AT")) : -2;
    if (profile_at >= 0 && rt_now_ns() > (u64)(profile_at * 1e9)) { profile_at = -2; keys |= 32; }
    if (keys & 32) profile_start(10.0);   /* F6: 10 seconds' sampling profile */
    char detail[512];
    vk_draw_describe(detail, sizeof detail);
    if (overlay_build_present(overlay, details ? detail : "F2 details")) vk_overlay_draw(&t, fit_x, fit_y, fit_w, fit_h);
    char note[48];
    if (display_notice(note, sizeof note)) {   /* (F7's mode) */
        overlay_build_notice(fit_w, fit_h, note);
        vk_overlay_draw(&t, fit_x, fit_y, fit_w, fit_h);
    }
    unsigned bar = display_bar_height();
    if (bar && bar < t.h) {   /* (the mode bar of a window) */
        int button[4];
        overlay_build_bar(t.w, t.h, bar, display_operation_mode() == OP_HANDHELD ? "Handheld (720p)" : "Docked (1080p)", button);
        vk_overlay_draw(&t, 0, 0, t.w, t.h);
        display_note_bar_button(button[0], button[1], button[2], button[3]);
    }
    vk_frame_end(&t);
    vk_leave();
}
