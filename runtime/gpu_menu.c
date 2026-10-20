/* The start-up settings menu.
 *
 * Every start (unless display.ini's menu = 0 or SSBU_MENU=0), before the shaders' screen
 * (gpu_precompile.c) and the game: Mode, Resolution scale, Upscaling (and Sharpness with FSR), VSync and Window,
 * as display.ini has them, then Play. Up/down picks a row, left/right changes it, Enter / the
 * controller's south button plays, Esc / east starts with display.ini as it was; a click picks a
 * row, its value's left or right half steps it, Play plays. Untouched for 10 s, it starts as
 * display.ini was. Play writes what changed into display.ini (display_save: the user's comments
 * and other keys kept) and applies it to this start (display_apply). A setting an environment
 * variable fixes is shown, not changeable, and not written.
 *
 * The window's thread (linux/window.c) feeds menu_input; this thread draws with gfx_menu. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "gpu_backend.h"
#include "gpu_menu.h"
#include "gpu_scale.h"

#define WAIT_S 10.0

volatile int menu_active;

/* ---- the model ------------------------------------------------------------------------------ */
static const Pacing VSYNC_ORDER[] = {PACING_VSYNC, PACING_CLOCK, PACING_VRR};
static const int SCALES[] = {50, 75, 100, 150, 200};
static const unsigned ROW_LOCK[ROWS] = {LOCK_MODE, LOCK_SCALE, LOCK_UPSCALE, LOCK_UPSCALE, LOCK_PACING, LOCK_WINDOW, 0};
static const char *const ROW_VAR[ROWS] = {"SSBU_MODE", "SSBU_SCALE", "SSBU_UPSCALE", "SSBU_UPSCALE", "SSBU_PACING", "SSBU_WINDOW", ""};

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
    static const char *const L[ROWS] = {"Mode", "Resolution scale", "Upscaling", "Sharpness", "VSync", "Window", "Play"};
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

/* ---- the screen ----------------------------------------------------------------------------- */
static double seconds(void) { return GetTickCount64() / 1000.0; }

void menu_run(void) {
#ifdef _WIN32
    return;   /* (no start-up phase on Windows: display.ini is edited there) */
#else
    if (!display_menu_enabled()) return;
    DisplaySettings now;
    unsigned locked;
    display_get_settings(&now, NULL, &locked);
    Menu m;
    menu_init(&m, &now, locked, seconds());
    MenuRect rects[ROWS];
    float value_x = 0;
    memset(rects, 0, sizeof rects);
    tail = head;
    menu_active = 1;
    while (m.state == MENU_OPEN) {
        while (tail != head) {
            Input in = queue[tail & 63];
            MemoryBarrier();
            tail++;
            if (in.event == MENU_CLICK) menu_click(&m, rects, value_x, (float)in.x, (float)in.y, seconds());
            else menu_event(&m, in.event, seconds());
        }
        menu_tick(&m, seconds());
        if (m.state != MENU_OPEN) break;
        const char *labels[ROWS];
        char values[ROWS][64], footer[96];
        int shown[ROWS];
        for (int r = 0; r < ROWS; r++) {
            labels[r] = menu_label(r);
            menu_value(&m, r, values[r], sizeof values[r]);
            shown[r] = menu_visible(&m, r);
        }
        if (!m.touched) snprintf(footer, sizeof footer, "Starting in %.0f s - press any key to change settings", m.deadline - seconds() + 0.5);
        else if (menu_recommends_fsr(&m)) snprintf(footer, sizeof footer, "FSR recommended below 100%%   -   Enter: play   Esc: no changes");
        else snprintf(footer, sizeof footer, "Enter: play    Esc: play without changes");
        const char *vp[ROWS];
        for (int r = 0; r < ROWS; r++) vp[r] = values[r];
        if (!gfx_menu("Settings", ROWS, labels, vp, shown, m.row, ROW_PLAY, footer, (float *)rects, &value_x)) Sleep(30);
        Sleep(16);
    }
    menu_active = 0;
    if (m.state == MENU_PLAY) {
        display_save(&m.s);
        display_apply(&m.s);
    }
    fprintf(stderr, "[menu] %s: %s at %d%%, upscale %s, pacing %d, window %d\n", m.state == MENU_PLAY ? "play" : "as display.ini was",
            m.s.operation == OP_HANDHELD ? "handheld" : "docked", m.s.scale, m.s.upscale == UPSCALE_FSR ? "fsr" : "linear", (int)m.s.pacing,
            m.s.window);
#endif
}
