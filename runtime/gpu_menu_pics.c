/* The start-up menu's pictures: see gpu_menu_pics.h. */
#include "gpu_menu_pics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "gpu_bntx.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../third_party/stb/stb_image.h"
#include "gpu_overlay_quads.h"
#include "mod_arc.h"
#include "mod_thread.h"

#define PICS 192   /* (the grid's 87 portraits, a page's 8 icons and cards, the preview's two) */

enum { P_EMPTY, P_QUEUED, P_LOADING, P_READY, P_FAILED };
static struct Pic { char key[400]; int state, tex; unsigned w, h; uint64_t used, asked; } pics[PICS];   /* (asked: when it was first asked for) */
static uint64_t pic_clock;   /* (every call: the most recent is the highest) */
static ModLock lock = MOD_LOCK_INIT;
static ModWake wake = MOD_WAKE_INIT;
static int running, stopping;

static void *read_file(const char *path, uint64_t *size) {
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *size = (uint64_t)n;
    return d;
}

/* A key's picture as a texture: its id, 0 not */
static int load(const char *key, unsigned *w, unsigned *h) {
    uint64_t size = 0;
    void *data = NULL;
    if (!strncmp(key, "arc:", 4)) {
        static int said;
        data = mod_arc_game_file(key + 4, &size);
        if (!data && !strncmp(key + 4, "ui/replace/", 11)) {   /* (the DLC's: in replace_patch alone) */
            char other[420];
            snprintf(other, sizeof other, "ui/replace_patch/%s", key + 4 + 11);
            data = mod_arc_game_file(other, &size);
        }
        if (!data && !said) {
            uint64_t probe;
            void *any = mod_arc_game_file("ui/replace/chara/chara_2/chara_2_mario_00.bntx", &probe);
            if (!any) { said = 1; fprintf(stderr, "[mods] the menu's portraits: cannot read data.arc - initials instead\n"); }
            free(any);
        }
    } else if (!strncmp(key, "file:", 5)) data = read_file(key + 5, &size);
    if (!data) return 0;
    BntxImage img;
    char err[200];
    int tex = 0;
    size_t kl = strlen(key);
    int png = kl > 4 && key[kl - 4] == '.' && tolower((unsigned char)key[kl - 3]) == 'p' && tolower((unsigned char)key[kl - 2]) == 'n' &&
              tolower((unsigned char)key[kl - 1]) == 'g';
    if (png) {   /* (a mod's preview.png) */
        int pw, ph, n;
        unsigned char *px = size < 0x7fffffff ? stbi_load_from_memory(data, (int)size, &pw, &ph, &n, 4) : NULL;
        if (px && pw > 0 && ph > 0 && pw <= 8192 && ph <= 8192) {
            tex = overlay_texture_add(OVERLAY_TEX_RGBA8, (unsigned)pw, (unsigned)ph, px, (size_t)pw * ph * 4);
            *w = (unsigned)pw, *h = (unsigned)ph;
        } else fprintf(stderr, "[mods] %s: %s\n", key + 5, px ? "too big" : stbi_failure_reason());
        stbi_image_free(px);
        free(data);
        return tex;
    }
    if (bntx_read(data, (size_t)size, &img, err, sizeof err)) {
        tex = overlay_texture_add(img.format, img.width, img.height, img.data, img.size);
        *w = img.width, *h = img.height;
        free(img.data);
    } else fprintf(stderr, "[mods] %s: %s\n", key + (key[0] == 'a' ? 4 : 5), err);
    free(data);
    return tex;
}

static void loader(void *arg) {
    (void)arg;
    mod_lock(&lock);
    while (!stopping) {
        /* the next: of those drawn lately (the screen now, not one left behind), the first asked for - the
         * grid's portraits in the order of its tiles, a page's new cards after the ones already there */
        struct Pic *next = NULL;
        for (int i = 0; i < PICS; i++)
            if (pics[i].state == P_QUEUED && pics[i].used + 2 * PICS >= pic_clock && (!next || pics[i].asked < next->asked)) next = &pics[i];
        for (int i = 0; i < PICS && !next; i++)   /* (else the most recent of the rest) */
            if (pics[i].state == P_QUEUED && (!next || pics[i].used > next->used)) next = &pics[i];
        if (!next) { mod_wait(&wake, &lock, 500); continue; }
        char key[400];
        snprintf(key, sizeof key, "%s", next->key);
        next->state = P_LOADING;
        mod_unlock(&lock);
        unsigned w = 0, h = 0;
        int tex = load(key, &w, &h);
        mod_lock(&lock);
        if (next->state == P_LOADING && !strcmp(next->key, key)) {
            next->tex = tex, next->w = w, next->h = h;
            next->state = tex ? P_READY : P_FAILED;
        } else if (tex) overlay_texture_free(tex);   /* (let go meanwhile) */
    }
    mod_arc_game_done();   /* (data.arc's table: not needed by the game) */
    running = 0;
    mod_wake(&wake);
    mod_unlock(&lock);
}

static void let_go(struct Pic *p) {
    if (p->state == P_READY) overlay_texture_free(p->tex);
    memset(p, 0, sizeof *p);
}

int menu_pic(const char *key, unsigned *w, unsigned *h, int *failed) {
    *failed = 0, *w = *h = 0;
    if (!key || !*key) { *failed = 1; return 0; }
    mod_lock(&lock);
    struct Pic *p = NULL, *spare = NULL;
    for (int i = 0; i < PICS && !p; i++) {
        if (!strcmp(pics[i].key, key)) p = &pics[i];
        else if (!spare || pics[i].used < spare->used) spare = &pics[i];   /* (the least recently drawn; empty ones are 0) */
    }
    if (!p) {
        if (spare->state == P_LOADING) spare->state = P_EMPTY;   /* (its texture freed when it comes) */
        let_go(spare);
        p = spare;
        snprintf(p->key, sizeof p->key, "%s", key);
        p->state = P_QUEUED;
        p->asked = pic_clock + 1;
        if (!running && !stopping) running = mod_thread(loader, NULL);
        mod_wake(&wake);
    }
    p->used = ++pic_clock;
    int tex = p->state == P_READY ? p->tex : 0;
    if (tex) *w = p->w, *h = p->h;
    *failed = p->state == P_FAILED;
    mod_unlock(&lock);
    return tex;
}

void menu_pics_stop(void) {
    mod_lock(&lock);
    stopping = 1;
    mod_wake(&wake);
    while (running) mod_wait(&wake, &lock, 100);
    for (int i = 0; i < PICS; i++) let_go(&pics[i]);
    stopping = 0;
    mod_unlock(&lock);
}
