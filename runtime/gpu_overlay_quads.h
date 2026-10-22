/* The overlay's drawing, as quads - the same for every renderer (gpu_overlay_quads.c): text
 * from a glyph atlas (a monospaced font, printable ASCII, plus a solid cell for rectangles),
 * every quad two triangles of OverlayVertex in pixels of the target. A renderer draws them with
 * the atlas as a one-channel texture: colour from the vertex, alpha times the atlas's coverage,
 * blended over what is there (gpu_overlay.c for Direct3D 11, linux/gpu_vk_overlay.c for Vulkan). */
#pragma once

#define OVERLAY_ATLAS_W (12 * 16)
#define OVERLAY_ATLAS_H (24 * 6)

typedef struct OverlayVertex { float x, y, u, v; unsigned color; } OverlayVertex;   /* color: RGBA8, red in the low byte */

/* The glyph atlas's coverage, a byte a texel, row after row. 0 if it cannot be made. */
int overlay_atlas(unsigned char coverage[OVERLAY_ATLAS_W * OVERLAY_ATLAS_H]);

/* The quads the last overlay_build_* made, *count vertices of them. */
const OverlayVertex *overlay_quads(unsigned *count);

/* Once per presented frame: notes the frame's time and, if show, builds the overlay (frame rate,
 * a frame time graph, detail under it: lines separated by '\n', or NULL). 1 if there is
 * something to draw. */
int overlay_build_present(int show, const char *detail);

/* The start-up's shader screen, centred in a w x h target: see overlay_progress (gpu_overlay.h). */
void overlay_build_progress(unsigned w, unsigned h, const char *title, unsigned done, unsigned total, const char *status);

/* The start-up menu, centred in a w x h target: see overlay_menu (gpu_overlay.h). */
void overlay_build_menu(unsigned w, unsigned h, const char *title, int n, const char *const labels[], const char *const values[],
                        const int shown[], int selected, int button, const char *footer, float *rects, float *value_x);

/* F7's notice at the top of a w x h picture. */
void overlay_build_notice(unsigned w, unsigned h, const char *note);

/* The mode bar across the top of a w x h window, bar px tall; its button's place into button. */
void overlay_build_bar(unsigned w, unsigned h, unsigned bar, const char *mode, int button[4]);
