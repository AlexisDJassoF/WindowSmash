/* The start-up settings menu.
 *
 * Every start (unless display.ini's menu = 0 or SSBU_MENU=0), before the shaders' screen
 * (gpu_precompile.c) and the game: Mode, Resolution scale, Upscaling (and Sharpness with FSR), VSync, Window
 * and Renderer, as display.ini has them, then Play. Up/down picks a row, left/right changes it, Enter / the
 * controller's south button plays, Esc / east starts with display.ini as it was; a click picks a
 * row, its value's left or right half steps it, Play plays. Untouched for 10 s, it starts as
 * display.ini was. Play writes what changed into display.ini (display_save: the user's comments
 * and other keys kept) and applies it to this start (display_apply). A setting an environment
 * variable fixes is shown, not changeable, and not written. The renderer is already drawing the
 * menu: another one chosen starts the program again (the menu not shown then), on it.
 *
 * Tab (a controller's shoulder buttons, or a click on the title) shows the other page, Network
 * (net_menu.c): how this start goes online - as the launcher chose, hosting a room or joining one -
 * saved in netplay.ini and set up before the game's network starts. F8 twice in the game starts
 * the program again on that page (menu_restart_network).
 *
 * The window's thread (linux/window.c) feeds menu_input; this thread draws with gfx_menu. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <windows.h>

#include "gpu_backend.h"
#include "gpu_menu.h"
#include "gpu_scale.h"
#include "net.h"
#include "net_discover.h"
#include "net_menu.h"

#define WAIT_S 10.0

volatile int menu_active, menu_typing;

/* ---- the model ------------------------------------------------------------------------------ */
static const Pacing VSYNC_ORDER[] = {PACING_VSYNC, PACING_CLOCK, PACING_VRR};
static const int SCALES[] = {50, 75, 100, 150, 200};
static const unsigned ROW_LOCK[ROWS] = {LOCK_MODE, LOCK_SCALE, LOCK_UPSCALE, LOCK_UPSCALE, LOCK_PACING, LOCK_WINDOW, LOCK_RENDERER, 0};
static const char *const ROW_VAR[ROWS] = {"SSBU_MODE", "SSBU_SCALE", "SSBU_UPSCALE", "SSBU_UPSCALE", "SSBU_PACING", "SSBU_WINDOW", "SSBU_RENDERER", ""};

void menu_init(Menu *m, const DisplaySettings *now, unsigned locked, double now_s) {
    memset(m, 0, sizeof *m);
    m->s = m->initial = *now;
    m->locked = locked;
    m->row = ROW_PLAY;   /* (Enter right away plays) */
    m->state = MENU_OPEN;
    m->deadline = now_s + WAIT_S;
}

int menu_visible(const Menu *m, int row) { return row != ROW_SHARPNESS || m->s.upscale == UPSCALE_FSR; }

const char *menu_label(int row) {
    static const char *const L[ROWS] = {"Mode", "Resolution scale", "Upscaling", "Sharpness", "VSync", "Window", "Renderer", "Play"};
    return row >= 0 && row < ROWS ? L[row] : "";
}

void menu_value(const Menu *m, int row, char *out, unsigned n) {
    const char *v = "";
    char num[32];
    switch (row) {
    case ROW_MODE: v = m->s.operation == OP_HANDHELD ? "Handheld (720p)" : "Docked (1080p)"; break;
    case ROW_SCALE: {   /* the percentage, and what it draws in the chosen mode */
        unsigned w = m->s.operation == OP_HANDHELD ? 1280 : 1920, h = m->s.operation == OP_HANDHELD ? 720 : 1080;
        float f = m->s.scale / 100.0f;
        snprintf(num, sizeof num, "%d%% (%u x %u)", m->s.scale, scaled_size(w, f), scaled_size(h, f));
        v = num;
        break;
    }
    case ROW_UPSCALE: v = m->s.upscale == UPSCALE_FSR ? "FSR" : "Linear"; break;
    case ROW_SHARPNESS: snprintf(num, sizeof num, "%.1f", m->s.sharpness); v = num; break;
    case ROW_VSYNC:
        v = m->s.pacing == PACING_VSYNC ? "On" : m->s.pacing == PACING_VRR ? "VRR" : m->s.pacing == PACING_DISPLAY ? "Display" : "Off";
        break;
    case ROW_WINDOW: v = m->s.window == 0 ? "Windowed" : m->s.window == 1 ? "Borderless" : "Fullscreen"; break;
    case ROW_RENDERER: v = m->s.renderer == RENDERER_VULKAN ? "Vulkan (experimental)" : "DXVK"; break;
    }
    if (row != ROW_PLAY && (m->locked & ROW_LOCK[row])) snprintf(out, n, "%s (set by %s)", v, ROW_VAR[row]);
    else snprintf(out, n, "%s", v);
}

static void step(Menu *m, int dir) {
    if (m->row == ROW_PLAY || (m->locked & ROW_LOCK[m->row])) return;
    switch (m->row) {
    case ROW_MODE: m->s.operation = m->s.operation == OP_HANDHELD ? OP_DOCKED : OP_HANDHELD; break;
    case ROW_UPSCALE: m->s.upscale = m->s.upscale == UPSCALE_FSR ? UPSCALE_LINEAR : UPSCALE_FSR; break;
    case ROW_SHARPNESS: {
        int tenths = (int)(m->s.sharpness * 10 + 0.5f) + dir;
        m->s.sharpness = (tenths < 0 ? 0 : tenths > 20 ? 20 : tenths) / 10.0f;
        break;
    }
    case ROW_VSYNC: {
        int i = 0, n = (int)(sizeof VSYNC_ORDER / sizeof VSYNC_ORDER[0]);
        while (i < n && VSYNC_ORDER[i] != m->s.pacing) i++;
        if (i == n) i = 1;   /* (display paces as clock does: stepped from Off) */
        m->s.pacing = VSYNC_ORDER[((i + dir) % n + n) % n];
        break;
    }
    case ROW_WINDOW: m->s.window = m->s.window == 0 ? 2 : 0; break;
    case ROW_RENDERER: m->s.renderer = m->s.renderer == RENDERER_VULKAN ? RENDERER_DXVK : RENDERER_VULKAN; break;
    case ROW_SCALE: {
        int i = 0, n = (int)(sizeof SCALES / sizeof SCALES[0]);
        while (i < n && SCALES[i] != m->s.scale) i++;
        if (i == n) i = 2;   /* (100) */
        i += dir;
        m->s.scale = SCALES[i < 0 ? 0 : i >= n ? n - 1 : i];   /* (no wrapping: 50 to 200) */
        break;
    }
    }
}

int menu_recommends_fsr(const Menu *m) { return m->s.scale < 100 && m->s.upscale != UPSCALE_FSR; }

static void move(Menu *m, int dir) {
    do m->row = (m->row + dir + ROWS) % ROWS; while (!menu_visible(m, m->row));
}

void menu_event(Menu *m, int event, double now_s) {
    (void)now_s;
    if (m->state != MENU_OPEN) return;
    m->touched = 1;
    switch (event) {
    case MENU_UP: move(m, -1); break;
    case MENU_DOWN: move(m, 1); break;
    case MENU_LEFT: step(m, -1); break;
    case MENU_RIGHT: step(m, 1); break;
    case MENU_ACCEPT: if (m->row == ROW_PLAY) m->state = MENU_PLAY; else move(m, 1); break;
    case MENU_BACK: m->state = MENU_KEEP; m->s = m->initial; break;
    }
}

void menu_click(Menu *m, const MenuRect rects[ROWS], float value_x, float x, float y, double now_s) {
    for (int row = 0; row < ROWS; row++) {
        const MenuRect *r = &rects[row];
        if (!menu_visible(m, row) || r->w <= 0 || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
        m->touched = 1;
        m->row = row;
        if (row == ROW_PLAY) menu_event(m, MENU_ACCEPT, now_s);
        else if (x >= value_x) menu_event(m, x < value_x + (r->x + r->w - value_x) / 2 ? MENU_LEFT : MENU_RIGHT, now_s);
        return;
    }
}

void menu_tick(Menu *m, double now_s) {
    if (m->state == MENU_OPEN && !m->touched && now_s >= m->deadline) m->state = MENU_KEEP;
}

/* ---- the window's input --------------------------------------------------------------------- */
typedef struct Input { int event, x, y; } Input;
static Input queue[64];
static volatile LONG head, tail;   /* (one writer, the window's thread; one reader, this one) */

void menu_input(int event, int x, int y) {
    LONG h = head;
    if (h - tail >= 64) return;
    queue[h & 63] = (Input){event, x, y};
    MemoryBarrier();
    head = h + 1;
}

/* ---- starting again ------------------------------------------------------------------------ */
#ifndef _WIN32
/* The program again, with the same arguments: for another renderer (chosen at its first frame, the
 * menu's; display.ini names the new one now) without the menu, or on the Network page with it (F8 in
 * the game). The environment goes along: the network the menu set up, the launcher's kept aside.
 * Returns only if it cannot. */
static void start_again(int network) {
    static char args[65536];
    char *argv[256];
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    int argc = 0;
    for (size_t at = 0; at < n && argc < 255; at += strlen(args + at) + 1) argv[argc++] = args + at;
    argv[argc] = NULL;
    if (!argc) return;
    if (network) {
        setenv("SSBU_MENU", "1", 1);
        setenv("SSBU_MENU_PAGE", "network", 1);
        net_stop();   /* (the relay told, the router's port closed: exit's work, which execv skips) */
    } else setenv("SSBU_MENU", "0", 1);
    fprintf(stderr, network ? "[menu] F8: starting again on the Network page\n" : "[menu] another renderer: starting again on it\n");
    fflush(NULL);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "[menu] cannot start again: %s\n", network ? "change the network at the next start" : "the renderer changes at the next start");
}
#endif

void menu_restart_network(void) {
#ifndef _WIN32
    start_again(1);
#endif
}

/* ---- the screen ----------------------------------------------------------------------------- */
static double seconds(void) { return GetTickCount64() / 1000.0; }

#ifndef _WIN32
/* A click on the Network page: a row picked, its value's halves stepping it (as menu_click does) */
static int net_click(NetMenu *nm, const MenuRect rects[NROWS], float value_x, float x, float y) {
    for (int row = 0; row < NROWS; row++) {
        const MenuRect *r = &rects[row];
        if (!net_menu_visible(nm, row) || r->w <= 0 || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
        if (!net_menu_selectable(nm, row)) return NET_EV_NONE;
        nm->row = row;
        if (row == NROW_PLAY) return net_menu_event(nm, MENU_ACCEPT, 0);
        if (x >= value_x) return net_menu_event(nm, x < value_x + (r->x + r->w - value_x) / 2 ? MENU_LEFT : MENU_RIGHT, 0);
        return NET_EV_NONE;
    }
    return NET_EV_NONE;
}

/* A click just above the first row (on the title): the other page */
static int title_click(const MenuRect *rects, int n, float x, float y) {
    for (int i = 0; i < n; i++)
        if (rects[i].w > 0) return x >= rects[i].x && x < rects[i].x + rects[i].w && y < rects[i].y && y >= rects[i].y - 60;
    return 0;
}

static void net_refresh(NetMenu *nm) {
    NetChoice found[NET_CHOICES];
    int n = nm->s.mode == NET_MODE_JOIN ? net_discover_found(found, NET_CHOICES) : 0;
    net_menu_set_found(nm, found, n);
    if (nm->s.mode == NET_MODE_HOST) net_discover_check_router(nm->s.port[0] ? nm->s.port : "39420");
    NetProbe p;
    net_discover_probe(&p);
    net_menu_describe(nm, &p);
}
#endif

void menu_run(void) {
#ifdef _WIN32
    return;   /* (no start-up phase on Windows: display.ini is edited there) */
#else
    net_launch_snapshot();   /* (what the launcher asked for, kept for "As launched") */
    NetSettings ns;
    net_settings_load(&ns);
    const char *page_env = getenv("SSBU_MENU_PAGE");
    int page = page_env && !strcmp(page_env, "network");   /* 0 Display, 1 Network */
    unsetenv("SSBU_MENU_PAGE");
    if (!display_menu_enabled()) {
        net_settings_apply(&ns);
        return;
    }
    DisplaySettings now;
    unsigned locked;
    display_get_settings(&now, NULL, &locked);
    Menu m;
    menu_init(&m, &now, locked, seconds());
    NetMenu nm;
    net_menu_init(&nm, &ns);
    net_launch_describe(nm.launched, sizeof nm.launched);
    if (page) { m.touched = 1; nm.row = NROW_MODE; }   /* (F8: here to change the network - no countdown) */
    int searching = 0;
    double refreshed = 0;
    MenuRect rects[ROWS], nrects[NROWS];
    float value_x = 0, nvalue_x = 0;
    memset(rects, 0, sizeof rects);
    memset(nrects, 0, sizeof nrects);
    tail = head;
    menu_active = 1;
    while (m.state == MENU_OPEN) {
        while (tail != head) {
            Input in = queue[tail & 63];
            MemoryBarrier();
            tail++;
            int ev = in.event;
            if (ev == MENU_CLICK && title_click(page ? nrects : rects, page ? NROWS : ROWS, (float)in.x, (float)in.y)) ev = MENU_TAB;
            if (ev == MENU_TAB) { page = !page; m.touched = 1; refreshed = 0; continue; }
            if (ev == MENU_BACK) { menu_event(&m, MENU_BACK, seconds()); nm.s = nm.initial; break; }
            if (!page) {
                if (ev == MENU_CLICK) menu_click(&m, rects, value_x, (float)in.x, (float)in.y, seconds());
                else if (ev != MENU_CHAR && ev != MENU_ERASE) menu_event(&m, ev, seconds());
                continue;
            }
            m.touched = 1;
            int mode = nm.s.mode;
            int r = ev == MENU_CLICK ? net_click(&nm, nrects, nvalue_x, (float)in.x, (float)in.y) : net_menu_event(&nm, ev, in.x);
            if (r == NET_EV_PLAY) m.state = MENU_PLAY;
            if (nm.s.mode != mode) refreshed = 0;
        }
        menu_tick(&m, seconds());
        /* Play from the Display page with Join and no address: the Network page first */
        if (m.state == MENU_PLAY && nm.s.mode == NET_MODE_JOIN && !nm.s.host[0]) { m.state = MENU_OPEN; page = 1; nm.row = NROW_HOST; }
        if (m.state != MENU_OPEN) break;
        if (page && !searching) { net_discover_start(); searching = 1; }
        if (page && seconds() - refreshed >= 0.5) { refreshed = seconds(); net_refresh(&nm); }
        menu_typing = page && net_menu_text_row(&nm);
        char footer[128];
        if (!m.touched) snprintf(footer, sizeof footer, "Starting in %.0f s - press any key to change settings", m.deadline - seconds() + 0.5);
        else if (page) snprintf(footer, sizeof footer, "Type to edit   < >: change   Enter: play   Esc: no changes");
        else if (menu_recommends_fsr(&m)) snprintf(footer, sizeof footer, "FSR recommended below 100%%   -   Enter: play   Esc: no changes");
        else snprintf(footer, sizeof footer, "Enter: play    Esc: play without changes");
        if (!page) {
            const char *labels[ROWS], *vp[ROWS];
            char values[ROWS][64];
            int shown[ROWS];
            for (int r = 0; r < ROWS; r++) {
                labels[r] = menu_label(r);
                menu_value(&m, r, values[r], sizeof values[r]);
                vp[r] = values[r];
                shown[r] = menu_visible(&m, r);
            }
            if (!gfx_menu("[ Display ]   Network  (Tab)", ROWS, labels, vp, shown, m.row, ROW_PLAY, footer, (float *)rects, &value_x)) Sleep(30);
        } else {
            const char *labels[NROWS], *vp[NROWS];
            char values[NROWS][160];
            int shown[NROWS];
            for (int r = 0; r < NROWS; r++) {
                labels[r] = net_menu_label(r);
                net_menu_value(&nm, r, values[r], sizeof values[r]);
                vp[r] = values[r];
                shown[r] = net_menu_visible(&nm, r);
            }
            if (!gfx_menu("Display  (Tab)   [ Network ]", NROWS, labels, vp, shown, nm.row, NROW_PLAY, footer, (float *)nrects, &nvalue_x)) Sleep(30);
        }
        Sleep(16);
    }
    menu_typing = 0;
    menu_active = 0;
    if (searching) net_discover_stop();
    if (m.state == MENU_PLAY) {
        display_save(&m.s);
        display_apply(&m.s);
        net_settings_remember(&nm.s);
        NetSettings keep = nm.s;   /* (a launcher's own choice, As launched untouched, leaves netplay.ini's mode as it was) */
        if (keep.mode == nm.initial.mode) keep.mode = keep.file_mode;
        if (!net_settings_save(&keep)) fprintf(stderr, "[menu] cannot write netplay.ini\n");
    }
    net_settings_apply(&nm.s);   /* (Esc: as netplay.ini was; untouched: as it is) */
    fprintf(stderr, "[menu] %s: %s at %d%%, upscale %s, pacing %d, window %d, renderer %s\n", m.state == MENU_PLAY ? "play" : "as display.ini was",
            m.s.operation == OP_HANDHELD ? "handheld" : "docked", m.s.scale, m.s.upscale == UPSCALE_FSR ? "fsr" : "linear", (int)m.s.pacing,
            m.s.window, m.s.renderer == RENDERER_VULKAN ? "vulkan" : "dxvk");
    char launched[96];
    net_launch_describe(launched, sizeof launched);
    fprintf(stderr, "[menu] network: %s\n", nm.s.mode == NET_MODE_HOST ? "hosting a room" : nm.s.mode == NET_MODE_JOIN ? "joining a room" : launched);
    if (m.state == MENU_PLAY && m.s.renderer != m.initial.renderer) start_again(0);
#endif
}
