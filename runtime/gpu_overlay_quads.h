/* The overlay's drawing, as quads - the same for every renderer (gpu_overlay_quads.c): text
 * from a glyph atlas (a monospaced font, printable ASCII, plus a solid cell for rectangles),
 * every quad two triangles of OverlayVertex in pixels of the target. A renderer draws them with
 * the atlas as a one-channel texture: colour from the vertex, alpha times the atlas's coverage,
 * blended over what is there (gpu_overlay.c for Direct3D 11, linux/gpu_vk_overlay.c for Vulkan). */
#pragma once
#include <stddef.h>

#define OVERLAY_ATLAS_W (12 * 16)
#define OVERLAY_ATLAS_H (24 * 6)

typedef struct OverlayVertex { float x, y, u, v; unsigned color; } OverlayVertex;   /* color: RGBA8, red in the low byte */

/* Pictures (the Mods page's portraits and icons): textures registered from any thread, drawn as
 * image quads. Their vertices sample the texture (u, v across it, 0 to 1), times the vertex colour.
 * The quads come in batches: a range of vertices and what they sample - the glyph atlas (texture 0)
 * or a picture, as it is or in greys. */
enum { OVERLAY_TEX_RGBA8, OVERLAY_TEX_BC1, OVERLAY_TEX_BC3, OVERLAY_TEX_BC7 };
enum { OVERLAY_GLYPHS, OVERLAY_IMAGE, OVERLAY_GREY };   /* a batch's mode (the shader's) */
typedef struct OverlayBatch { unsigned first, count; int texture, mode; } OverlayBatch;

/* A picture: data (RGBA rows, or rows of 4x4 blocks) copied; its id (> 0), 0 when it cannot be kept */
int overlay_texture_add(int format, unsigned w, unsigned h, const void *data, size_t size);
void overlay_texture_free(int id);   /* (drawn no more: the renderer lets it go) */
/* For the renderers. The batches the last overlay_build_* made (at least one when there are quads);
 * a picture's data to upload the first time it is drawn - 1 if it is alive, its data handed over in
 * *data (the caller frees it; NULL when taken already); and the ids freed since, one at a time (0: none) */
const OverlayBatch *overlay_batches(unsigned *count);
int overlay_texture_take(int id, int *format, unsigned *w, unsigned *h, void **data, size_t *size);
int overlay_texture_dead(void);

/* Where the menu's pictures come from (gpu_menu_pics.h's menu_pic: a key's texture and size, 0 while it
 * loads, *failed when it cannot be had); NULL, as at first: the menu is drawn without them */
void overlay_menu_pictures(int (*pic)(const char *key, unsigned *w, unsigned *h, int *failed));

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

/* The last menu's tabs, for clicks: the first one's left end, each one's width, how many (0: none). */
void overlay_menu_tabs(float *x0, float *w, int *count);

/* The Mods page's grid of fighters ('\23' rows, mod_menu.c's grid_row) as the last menu drew it: up
 * to n tiles' places (x, y, w, h each), how many it drew, and its columns into *cols (0: none drawn) */
int overlay_menu_grid(float *rects, int n, int *cols);
/* The Mods page's detail: its actions' hints ("key=what" each, in order) and a page's eight colors' boxes,
 * as the last menu drew them: their places (x, y, w, h each), how many */
int overlay_menu_actions(float r[32]);
int overlay_menu_colors(float r[32]);

/* A question over the next menus built (the Mods page's delete, the recovery's): its title, '\t', the
 * text, and optionally '\t' and its buttons' labels with '|' between them (none: Cancel and Delete; three
 * or four: one under the other); selected: which. NULL: none. Its buttons' places (x, y, w, h each, up
 * to four) from the last frame. */
void overlay_menu_dialog(const char *text, int selected);
void overlay_menu_dialog_rects(float r[16]);

/* A room's mods' panel (mod_room.h's RoomView) over the next menus built: NULL none. Its buttons' places
 * (x, y, w, h each, three) and its mode row's (the fourth) from the last frame. */
struct RoomView;
void overlay_menu_room(const struct RoomView *v);
void overlay_menu_room_rects(float r[16]);
/* The same over the game's picture alone (the online lobby), w x h; show 2: a line with its progress */
void overlay_build_room(unsigned w, unsigned h, const struct RoomView *v);

/* The next menus built drawn over the game's picture (dimming it), not over a screen of their own. */
void overlay_menu_over(int on);

/* F7's notice at the top of a w x h picture. */
void overlay_build_notice(unsigned w, unsigned h, const char *note);

/* The mode bar across the top of a w x h window, bar px tall; its button's place into button. */
void overlay_build_bar(unsigned w, unsigned h, unsigned bar, const char *mode, int button[4]);
