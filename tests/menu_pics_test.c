/* runtime/gpu_menu_pics.c without the game or a renderer, for tests/test_menu_pics.py:
 *   menu_pics_test <key>...
 * asks for each picture until it is there or cannot be had (5 s at most) and prints, a line each,
 * "<key> tex=<0 or 1> <w>x<h> failed=<0|1>", then the pixels' first RGBA of an RGBA8 one ("rgba=r,g,b,a").
 * data.arc is not there ("arc:" keys fail); "file:" keys read the file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include "gpu_menu_pics.h"
#include "gpu_overlay_quads.h"

void *mod_arc_game_file(const char *path, uint64_t *size) { (void)path; *size = 0; return NULL; }
void mod_arc_game_done(void) {}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1; return 1; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = 0; return 1; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        unsigned w = 0, h = 0;
        int failed = 0, tex = 0;
        for (int k = 0; k < 500 && !tex && !failed; k++) {
            tex = menu_pic(argv[i], &w, &h, &failed);
            if (!tex && !failed) usleep(10000);
        }
        printf("%s tex=%d %ux%u failed=%d", argv[i], tex != 0, w, h, failed);
        int format;
        void *data;
        size_t size;
        if (tex && overlay_texture_take(tex, &format, &w, &h, &data, &size) && data) {
            const unsigned char *p = data;
            printf(" format=%d", format);
            if (format == OVERLAY_TEX_RGBA8) printf(" rgba=%u,%u,%u,%u", p[0], p[1], p[2], p[3]);
            free(data);
        }
        printf("\n");
    }
    menu_pics_stop();
    return 0;
}
