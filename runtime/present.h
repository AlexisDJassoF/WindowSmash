/* Presentation: frames leaving the display's buffer queue. */
#pragma once
#include "host.h"

/* Called at vsync with the graphic buffer description the driver registered for the slot
 * being shown (the flattened buffer, as received) and the crop rectangle it was queued with
 * (left, top, right, bottom; all zeros: the whole buffer). */
void present_frame(const u8 *graphic_buffer, u32 size, const i32 crop[4]);
u64 present_frames(void);
