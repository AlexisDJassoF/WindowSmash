/* Presentation: the display's buffer queue hands over the buffer to show; the back end
 * draws the render target that lives at that buffer's memory.
 *
 * The buffer arrives as the driver flattened it: a parcel header (3 words), the Android
 * GraphicBuffer ("RFBG", width, height, stride, format, usage, ...) and then the native
 * handle the NVIDIA driver attaches - whose words include the nvmap handle of the memory
 * (word 32) and the buffer's byte offset in it (word 33), its width and height (26, 27). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "gpu_backend.h"
#include "present.h"

void present_frame(const u8 *graphic_buffer, u32 size, const i32 crop[4]) {
    const u32 *w = (const u32 *)graphic_buffer;
    if (size < 34 * 4 || w[3] != 0x47424652) {
        static int warned;
        if (!warned++) fprintf(stderr, "[present] unexpected graphic buffer layout (%u bytes)\n", size);
        return;
    }
    u32 handle = w[32], offset = w[33];
    if (!handle || handle >= NVMAP_MAX || !gpu_maps[handle].addr) return;
    /* the part to show: the crop, when it is a smaller rectangle inside the buffer; else (no
     * crop, or the whole buffer) all of it, given as 0 x 0 */
    u32 width = w[26], height = w[27], cx = 0, cy = 0, cw = 0, ch = 0;
    if (crop[0] >= 0 && crop[1] >= 0 && crop[2] > crop[0] && crop[3] > crop[1] && (u32)crop[2] <= width && (u32)crop[3] <= height &&
        (crop[0] || crop[1] || (u32)crop[2] < width || (u32)crop[3] < height)) {
        cx = (u32)crop[0]; cy = (u32)crop[1]; cw = (u32)(crop[2] - crop[0]); ch = (u32)(crop[3] - crop[1]);
    }
    static u32 last_w, last_h;   /* (the picture's size in the log when it changes: docked, handheld, F7) */
    u32 pw = cw ? cw : width, ph = ch ? ch : height;
    if (pw != last_w || ph != last_h) fprintf(stderr, "[present] the game's picture: %u x %u%s\n", pw, ph, cw ? " (cropped)" : "");
    last_w = pw;
    last_h = ph;
    gfx_present(gpu_maps[handle].addr + offset, cx, cy, cw, ch);
}
