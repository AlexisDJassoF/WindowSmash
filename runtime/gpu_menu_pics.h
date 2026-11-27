/* The start-up menu's pictures (gpu_menu_pics.c): portraits and icons as overlay textures, loaded in a
 * thread of their own and kept in a small cache (the least recently drawn let go first), so that the
 * menu never waits for them - what is not loaded yet is drawn without its picture.
 *
 * A picture's key names where it is: "arc:<path>" a file of the game's data.arc (ui/replace/...,
 * then ui/replace_patch/... when it is not there: the DLC's), "file:<path>" a file on disk (a mod's).
 * BNTX files, and PNG (a mod's preview.png, with third_party/stb's stb_image). */
#pragma once

/* The picture's texture (overlay_texture_add's id) and size, 0 while it loads (it is asked for
 * now), or 0 with *failed set when it cannot be had. */
int menu_pic(const char *key, unsigned *w, unsigned *h, int *failed);
/* The menu closed: every picture let go, the thread stopped, data.arc's table freed (before the game) */
void menu_pics_stop(void);
