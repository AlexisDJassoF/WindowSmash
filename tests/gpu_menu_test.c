/* runtime/gpu_menu.c's model without a window: tests/test_gpu_menu.py builds this with it.
 *   gpu_menu_test <script>   events, one letter each: u d l r a b (up down left right accept back),
 *                            t<seconds> (time passes), c<x>,<y> (a click; the rows are 40 px tall
 *                            from y = 100 - Mode, Resolution scale, Upscaling... Play at 380 - values from x = 300). Prints the state, row and settings, '|' between them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_menu.h"
#include "net.h"
#include "net_discover.h"

/* (what gpu_menu.c's screen loop calls, not used by the model) */
ULONGLONG GetTickCount64(void) { return 0; }
void Sleep(DWORD ms) { (void)ms; }
int display_menu_enabled(void) { return 1; }
void display_get_settings(DisplaySettings *now, DisplaySettings *file, unsigned *fixed) { (void)now; (void)file; (void)fixed; }
void display_apply(const DisplaySettings *s) { (void)s; }
int display_save(const DisplaySettings *s) { (void)s; return 1; }
void net_stop(void) {}
void net_discover_start(void) {}
void net_discover_stop(void) {}
void net_discover_refresh(void) {}
int net_switch(void) { return NET_SWITCH_DONE; }
void display_show_notice(const char *text, unsigned ms) { (void)text; (void)ms; }
void overlay_menu_over(int on) { (void)on; }
void overlay_menu_tabs(float *x0, float *w, int *count) { *x0 = *w = 0; *count = 0; }
void overlay_menu_dialog(const char *text, int selected) { (void)text; (void)selected; }
void overlay_menu_dialog_rects(float r[8]) { memset(r, 0, 8 * sizeof *r); }
const char *rt_data_dir = ".";
const char *pad_default_ini(void) { return "[device default]\n"; }
void overlay_build_menu(unsigned w, unsigned h, const char *title, int n, const char *const labels[], const char *const values[],
                        const int shown[], int selected, int button, const char *footer, float *rects, float *value_x) {
    (void)w; (void)h; (void)title; (void)n; (void)labels; (void)values; (void)shown; (void)selected; (void)button; (void)footer; (void)rects; (void)value_x;
}
int net_discover_found(NetChoice *out, int max) { (void)out; (void)max; return 0; }
int net_discover_watch(const char *addr) { (void)addr; return NET_WATCH_NONE; }
void net_discover_check_router(const char *port) { (void)port; }
void net_discover_probe(NetProbe *p) { memset(p, 0, sizeof *p); }
int gfx_menu(const char *t, int n, const char *const l[], const char *const v[], const int s[], int sel, int b, const char *f, float *r, float *x) {
    (void)t; (void)n; (void)l; (void)v; (void)s; (void)sel; (void)b; (void)f; (void)r; (void)x;
    return 0;
}

int main(int argc, char **argv) {
    DisplaySettings s = {0, OP_DOCKED, UPSCALE_LINEAR, 0.2f, PACING_CLOCK, 100};
    if (getenv("TEST_SCALE")) s.scale = atoi(getenv("TEST_SCALE"));
    unsigned locked = getenv("TEST_LOCKED") ? (unsigned)atoi(getenv("TEST_LOCKED")) : 0;
    if (getenv("TEST_PACING")) s.pacing = (Pacing)atoi(getenv("TEST_PACING"));
    Menu m;
    double t = 0;
    menu_init(&m, &s, locked, t);
    MenuRect rects[ROWS];
    for (int r = 0; r < ROWS; r++) rects[r] = (MenuRect){0, 100 + 40.0f * r, 600, 40};
    if (!menu_visible(&m, ROW_SHARPNESS)) rects[ROW_SHARPNESS].w = 0;
    for (const char *p = argc > 1 ? argv[1] : ""; *p; p++) {
        switch (*p) {
        case 'u': menu_event(&m, MENU_UP, t); break;
        case 'd': menu_event(&m, MENU_DOWN, t); break;
        case 'l': menu_event(&m, MENU_LEFT, t); break;
        case 'r': menu_event(&m, MENU_RIGHT, t); break;
        case 'a': menu_event(&m, MENU_ACCEPT, t); break;
        case 'b': menu_event(&m, MENU_BACK, t); break;
        case 't': t += strtod(p + 1, (char **)&p); p--; menu_tick(&m, t); break;
        case 'c': {
            char *e;
            float x = strtof(p + 1, &e), y = strtof(e + 1, &e);
            menu_click(&m, rects, 300, x, y, t);
            p = e - 1;
            break;
        }
        }
    }
    char v[ROWS][80];
    for (int r = 0; r < ROWS; r++) menu_value(&m, r, v[r], sizeof v[r]);
    printf("state=%d|row=%s|mode=%s|scale=%s|upscale=%s|sharpness=%s|vsync=%s|window=%s|renderer=%s|sharp_shown=%d|fsr_hint=%d\n", m.state,
           menu_label(m.row), v[ROW_MODE], v[ROW_SCALE], v[ROW_UPSCALE], v[ROW_SHARPNESS], v[ROW_VSYNC], v[ROW_WINDOW], v[ROW_RENDERER],
           menu_visible(&m, ROW_SHARPNESS), menu_recommends_fsr(&m));
    return 0;
}
