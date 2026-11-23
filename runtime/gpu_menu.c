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
#include "gpu_overlay_quads.h"
#include "gpu_scale.h"
#include "net.h"
#include "net_discover.h"
#include "net_menu.h"
#include "pad_menu.h"

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
    case ROW_WINDOW:
        v = m->s.window == DISPLAY_WINDOWED ? "Windowed" : m->s.window == DISPLAY_MAXIMIZED ? "Windowed (Maximized)"
          : m->s.window == DISPLAY_BORDERLESS ? "Borderless" : "Fullscreen";
        break;
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
    case ROW_WINDOW: {   /* Windowed, Windowed (Maximized), Fullscreen (display.ini's borderless: as Fullscreen) */
        static const int ORDER[] = {DISPLAY_WINDOWED, DISPLAY_MAXIMIZED, DISPLAY_FULLSCREEN};
        int at = m->s.window == DISPLAY_WINDOWED ? 0 : m->s.window == DISPLAY_MAXIMIZED ? 1 : 2;
        m->s.window = ORDER[(at + dir + 3) % 3];
        break;
    }
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
static LONG head, tail;   /* (writers: the window's thread and, in the game, hle_pad.c's; one reader) */
static SRWLOCK queue_lock = SRWLOCK_INIT;
volatile int menu_device;
char menu_pad_name[96];
volatile int menu_axes[4], menu_listening, menu_sticks_still, menu_pad_kind_seen = MENU_PAD_XBOX;
volatile int menu_snapback[2], menu_axes_game[4], menu_snap_count[2];

void menu_input(int event, int x, int y) {
    AcquireSRWLockExclusive(&queue_lock);
    if (head - tail < 64) queue[head++ & 63] = (Input){event, x, y};
    ReleaseSRWLockExclusive(&queue_lock);
}

static int next_input(Input *in) {   /* the oldest input not taken yet: 0 when there is none */
    AcquireSRWLockExclusive(&queue_lock);
    int got = tail != head;
    if (got) *in = queue[tail++ & 63];
    ReleaseSRWLockExclusive(&queue_lock);
    return got;
}

static void drop_inputs(void) {
    AcquireSRWLockExclusive(&queue_lock);
    tail = head;
    ReleaseSRWLockExclusive(&queue_lock);
}

/* The keys or buttons the hints name: what plays (or applies), what goes back, what changes pages */
static void device_keys(const char **accept, const char **back, const char **pages) {
    switch (menu_device) {
    /* (a pad's buttons drawn: "^" its kind and the button's place, south or east - overlay_quads' pad_button;
     * pages: the two shoulders, '|' between) */
    case MENU_PAD_XBOX: *accept = "^XS"; *back = "^XE"; *pages = "LB|RB"; return;
    case MENU_PAD_PS: *accept = "^PS"; *back = "^PE"; *pages = "L1|R1"; return;
    case MENU_PAD_NINTENDO: *accept = "^NS"; *back = "^NE"; *pages = "L|R"; return;   /* (positional: the south button is B) */
    default: *accept = "Enter"; *back = "Esc"; *pages = "Tab"; return;
    }
}

/* ---- starting again ------------------------------------------------------------------------ */
#ifndef _WIN32
/* The program again, with the same arguments: for another renderer (chosen at its first frame, the
 * menu's; display.ini names the new one now) without the menu, or on the Network page with it (F8 in
 * the game). The environment goes along: the network the menu set up, the launcher's kept aside.
 * Returns only if it cannot. */
enum { AGAIN_RENDERER, AGAIN_NETWORK_PAGE, AGAIN_NETWORK };
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
    if (network == AGAIN_NETWORK_PAGE) {
        setenv("SSBU_MENU", "1", 1);
        setenv("SSBU_MENU_PAGE", "network", 1);
    } else setenv("SSBU_MENU", "0", 1);
    if (network) net_stop();   /* (the relay told, the router's port closed: exit's work, which execv skips) */
    fprintf(stderr, network == AGAIN_NETWORK_PAGE ? "[menu] F8: starting again on the Network page\n"
                    : network ? "[menu] the network changed to or from LAN mode: starting again\n" : "[menu] another renderer: starting again on it\n");
    fflush(NULL);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "[menu] cannot start again: %s\n", network ? "change the network at the next start" : "the renderer changes at the next start");
}
#endif

void menu_restart_network(void) {
#ifndef _WIN32
    start_again(AGAIN_NETWORK_PAGE);
#endif
}

/* ---- the screen ----------------------------------------------------------------------------- */
static double seconds(void) { return GetTickCount64() / 1000.0; }

#ifndef _WIN32
/* A click on the Network page: a row picked, its value's halves stepping it (as menu_click does);
 * Join's list: a room's button joins it, the heading's button and Refresh look again, the pages' halves turn them */
static int net_click(NetMenu *nm, const MenuRect rects[NROWS], float value_x, float x, float y) {
    for (int row = 0; row < NROWS; row++) {
        const MenuRect *r = &rects[row];
        if (!net_menu_visible(nm, row) || r->w <= 0 || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
        if (row == NROW_LAN) return x >= r->x + r->w - MENU_ICON_BUTTON ? NET_EV_REFRESH : NET_EV_NONE;
        if (!net_menu_selectable(nm, row)) return NET_EV_NONE;
        nm->row = row;
        if (row == NROW_PLAY) return net_menu_event(nm, MENU_ACCEPT, 0);
        if (row >= NROW_LIST && row <= NROW_LIST_END) return x >= r->x + r->w - MENU_JOIN_BUTTON ? net_menu_event(nm, MENU_ACCEPT, 0) : NET_EV_NONE;
        if (row == NROW_PAGER && x < value_x) return NET_EV_REFRESH;
        if (x >= value_x) return net_menu_event(nm, x < value_x + (r->x + r->w - value_x) / 2 ? MENU_LEFT : MENU_RIGHT, 0);
        return NET_EV_NONE;
    }
    return NET_EV_NONE;
}

/* A click on the tabs, just above the first row (where overlay_build_menu drew them last): the page
 * clicked, 1 + its number; else 0 */
static int title_click(const MenuRect *rects, int n, float x, float y) {
    float x0, w;
    int count;
    overlay_menu_tabs(&x0, &w, &count);
    for (int i = 0; i < n; i++)
        if (rects[i].w > 0) {
            if (!count || w <= 0 || y >= rects[i].y || y < rects[i].y - 64 || x < x0 || x >= x0 + w * count) return 0;
            return 1 + (int)((x - x0) / w);
        }
    return 0;
}

/* A click on the Controls page: a row picked, its value's halves stepping it; a button row's value
 * clicked again (selected already) waits for a control pressed */
static int pad_click(PadMenu *pm, const MenuRect rects[CROWS], float value_x, float x, float y) {
    for (int row = 0; row < CROWS; row++) {
        const MenuRect *r = &rects[row];
        if (!pad_menu_visible(pm, row) || r->w <= 0 || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
        if (!pad_menu_selectable(pm, row)) return PAD_EV_NONE;
        int again = pm->row == row;
        pm->row = row;
        if (row == CROW_PLAY || row == CROW_RESET || row == CROW_DEADZONE || row == CROW_DZ_TEST || row == CROW_DZ_BACK ||
            (again && pad_menu_row_button(row) >= 0 && x < value_x))
            return pad_menu_event(pm, MENU_ACCEPT, 0);
        if (x >= value_x) return pad_menu_event(pm, x < value_x + (r->x + r->w - value_x) / 2 ? MENU_LEFT : MENU_RIGHT, 0);
        return PAD_EV_NONE;
    }
    return PAD_EV_NONE;
}

static int pad_kind_of(int device) { return device == MENU_PAD_PS ? PK_PS : device == MENU_PAD_NINTENDO ? PK_NINTENDO : PK_XBOX; }

static void net_refresh(NetMenu *nm) {
    NetProbe p;
    net_discover_probe(&p);
    net_menu_set_self(nm, &p);   /* (this PC's addresses: never offered to join) */
    NetChoice found[NET_CHOICES];
    int join = nm->s.mode == NET_MODE_JOIN;
    int n = join ? net_discover_found(found, NET_CHOICES) : 0;
    net_menu_set_found(nm, found, n);
    if (join) {   /* the address asked directly; Join's own choice while it is not typed or picked */
        int self = net_menu_is_self(nm, nm->s.host);
        net_menu_auto_host(nm, net_discover_watch(self ? "" : nm->s.host));
        if (!net_menu_is_self(nm, nm->s.host)) net_discover_watch(nm->s.host);   /* (the address it chose now) */
    } else net_discover_watch("");
    if (nm->s.mode == NET_MODE_HOST) net_discover_check_router(nm->s.port[0] ? nm->s.port : "39420");
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
    enum { P_DISPLAY, P_NETWORK, P_CONTROLS, PAGES };
    int page = page_env && !strcmp(page_env, "network") ? P_NETWORK : P_DISPLAY;
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
    nm.no_play = 1;   /* (Play is the Display page's alone) */
    nm.row = NROW_MODE;
    net_launch_describe(nm.launched, sizeof nm.launched);
    if (page) { m.touched = 1; nm.row = NROW_MODE; }   /* (F8: here to change the network - no countdown) */
    PadSettings ps;
    pad_settings_load(&ps);
    PadMenu pm;
    pad_menu_init(&pm, &ps);
    pm.no_play = 1;
    int searching = 0;
    double refreshed = 0;
    MenuRect rects[ROWS], nrects[NROWS], prects[CROWS];
    float value_x = 0, nvalue_x = 0, pvalue_x = 0;
    memset(rects, 0, sizeof rects);
    memset(nrects, 0, sizeof nrects);
    memset(prects, 0, sizeof prects);
    drop_inputs();
    menu_active = 1;
    while (m.state == MENU_OPEN) {
        Input in;
        while (next_input(&in)) {
            int ev = in.event;
            if (page == P_CONTROLS && pm.listening) {   /* (waiting for a control: that, or going back, only) */
                m.touched = 1;
                if (ev == MENU_PADKEY || ev == MENU_BACK || ev == MENU_ACCEPT || ev == MENU_CLICK) pad_menu_event(&pm, ev == MENU_CLICK ? MENU_BACK : ev, in.x);
                continue;
            }
            if (ev == MENU_PADKEY) continue;
            if (ev == MENU_CLICK) {
                int tab = page == P_DISPLAY ? title_click(rects, ROWS, (float)in.x, (float)in.y)
                        : page == P_NETWORK ? title_click(nrects, NROWS, (float)in.x, (float)in.y) : title_click(prects, CROWS, (float)in.x, (float)in.y);
                if (tab) { m.touched = 1; refreshed = 0; page = (tab - 1) % PAGES; continue; }
            }
            if (ev == MENU_TAB || ev == MENU_TAB_PREV) { page = (page + (ev == MENU_TAB ? 1 : PAGES - 1)) % PAGES; m.touched = 1; refreshed = 0; continue; }
            if (ev == MENU_BACK && page == P_CONTROLS && pad_menu_takes_back(&pm)) { m.touched = 1; pad_menu_event(&pm, MENU_BACK, 0); continue; }
            if (ev == MENU_BACK) { menu_event(&m, MENU_BACK, seconds()); nm.s = nm.initial; pm.s = pm.initial; break; }
            if (page == P_CONTROLS) {
                m.touched = 1;
                if (ev == MENU_CHAR || ev == MENU_ERASE) continue;
                int r = ev == MENU_CLICK ? pad_click(&pm, prects, pvalue_x, (float)in.x, (float)in.y) : pad_menu_event(&pm, ev, in.x);
                if (r == PAD_EV_PLAY) m.state = MENU_PLAY;
                continue;
            }
            if (page == P_DISPLAY) {
                if (ev == MENU_CLICK) menu_click(&m, rects, value_x, (float)in.x, (float)in.y, seconds());
                else if (ev != MENU_CHAR && ev != MENU_ERASE) menu_event(&m, ev, seconds());
                continue;
            }
            m.touched = 1;
            int mode = nm.s.mode;
            int r = ev == MENU_CLICK ? net_click(&nm, nrects, nvalue_x, (float)in.x, (float)in.y) : net_menu_event(&nm, ev, in.x);
            if (r == NET_EV_PLAY) m.state = MENU_PLAY;
            if (r == NET_EV_REFRESH) { net_discover_refresh(); refreshed = 0; }
            if (nm.s.mode != mode) refreshed = 0;
        }
        menu_tick(&m, seconds());
        /* Play from the Display page with Join and no address: the Network page first */
        if (m.state == MENU_PLAY && nm.s.mode == NET_MODE_JOIN && !nm.s.host[0]) {
            m.state = MENU_OPEN;
            page = P_NETWORK;
            nm.row = nm.nchoices ? NROW_LIST : NROW_HOST;
        }
        if (m.state != MENU_OPEN) break;
        /* (Join looks for rooms from the start, on either page: its address chosen before Play) */
        int network = page == P_NETWORK || nm.s.mode == NET_MODE_JOIN;
        if (network && !searching) { net_discover_start(); searching = 1; }
        if (network && seconds() - refreshed >= 0.5) { refreshed = seconds(); net_refresh(&nm); }
        menu_typing = page == P_NETWORK && net_menu_text_row(&nm);
        pm.kind = pad_kind_of(menu_device == MENU_KEYS ? menu_pad_kind_seen : menu_device);   /* (the pad drawn: the one used, or seen, last) */
        snprintf(pm.pad, sizeof pm.pad, "%s", menu_pad_name);
        for (int a = 0; a < 4; a++) pm.axes[a] = menu_axes[a], pm.game_axes[a] = menu_axes_game[a];
        menu_snapback[0] = pm.s.snapback_left, menu_snapback[1] = pm.s.snapback_right;
        static int counted[2];   /* (a bounce ignored: said for a second) */
        static double flashed[2];
        for (int side = 0; side < 2; side++) {
            if (menu_snap_count[side] != counted[side]) { counted[side] = menu_snap_count[side]; flashed[side] = seconds(); }
            pm.snap_flash[side] = flashed[side] > 0 && seconds() - flashed[side] < 1.0;
        }
        menu_listening = page == P_CONTROLS && pm.listening;
        menu_sticks_still = page == P_CONTROLS && pm.dead_zones;
        /* the footer, for overlay_build_menu: '\t' between hints, "key=what" a key's, '#' a countdown's fraction */
        char footer[160], tabs[80];
        const char *accept, *back, *pages;
        device_keys(&accept, &back, &pages);
        int keys = menu_device == MENU_KEYS;
        double left = m.deadline - seconds();
        if (!m.touched) snprintf(footer, sizeof footer, "#%.3f\tStarting in %.0f s - press %s to change settings", left / WAIT_S, left + 0.5,
                                 keys ? "any key" : "a button");
        else if (page == P_CONTROLS && pm.listening) snprintf(footer, sizeof footer, "!Press a button on the pad\t%s=cancel", keys ? "Esc" : back);
        else if (page == P_CONTROLS && pm.testing)
            snprintf(footer, sizeof footer, "%s=dead zone\t%s=stop the test", keys ? "< >" : "D-pad", back);
        else if (page == P_CONTROLS && pm.dead_zones)
            snprintf(footer, sizeof footer, "%s%s=select\t%s=back to Controls", keys ? "" : "D-pad=move\t", accept, back);
        else if (page == P_CONTROLS && pad_menu_row_button(pm.row) >= 0)
            snprintf(footer, sizeof footer, "%s=assign by pressing\t%s=play without changes", accept, back);
        else if (page == P_CONTROLS) snprintf(footer, sizeof footer, "%s=select\t%s=play without changes", accept, back);
        else if (page == P_NETWORK) snprintf(footer, sizeof footer, "%s%s=select\t%s=play without changes", keys ? "Type=edit\t" : "", accept, back);
        else if (menu_recommends_fsr(&m)) snprintf(footer, sizeof footer, "!FSR recommended below 100%%\t%s=play\t%s=no changes", accept, back);
        else snprintf(footer, sizeof footer, "%s=play\t%s=play without changes", accept, back);
        snprintf(tabs, sizeof tabs, "%sDisplay\t%sNetwork\t%sControls\t@%s", page == P_DISPLAY ? "*" : "", page == P_NETWORK ? "*" : "",
                 page == P_CONTROLS ? "*" : "", pages);
        if (page == P_CONTROLS) {
            const char *labels[CROWS], *vp[CROWS];
            char values[CROWS][512];
            int shown[CROWS];
            for (int r = 0; r < CROWS; r++) {
                labels[r] = pad_menu_label(r);
                pad_menu_value(&pm, r, values[r], sizeof values[r]);
                vp[r] = values[r];
                shown[r] = pad_menu_visible(&pm, r);
            }
            if (!gfx_menu(tabs, CROWS, labels, vp, shown, pm.row, CROW_PLAY, footer, (float *)prects, &pvalue_x)) Sleep(30);
        } else if (page == P_DISPLAY) {
            const char *labels[ROWS], *vp[ROWS];
            char values[ROWS][64];
            int shown[ROWS];
            for (int r = 0; r < ROWS; r++) {
                labels[r] = menu_label(r);
                menu_value(&m, r, values[r], sizeof values[r]);
                vp[r] = values[r];
                shown[r] = menu_visible(&m, r);
            }
            if (!gfx_menu(tabs, ROWS, labels, vp, shown, m.row, ROW_PLAY, footer, (float *)rects, &value_x)) Sleep(30);
        } else {
            const char *labels[NROWS], *vp[NROWS];
            char values[NROWS][200];
            int shown[NROWS];
            for (int r = 0; r < NROWS; r++) {
                labels[r] = net_menu_label(r);
                net_menu_value(&nm, r, values[r], sizeof values[r]);
                vp[r] = values[r];
                shown[r] = net_menu_visible(&nm, r);
            }
            if (!gfx_menu(tabs, NROWS, labels, vp, shown, nm.row, NROW_PLAY, footer, (float *)nrects, &nvalue_x)) Sleep(30);
        }
        Sleep(16);
    }
    menu_typing = 0;
    menu_listening = 0;
    menu_sticks_still = 0;
    menu_active = 0;
    if (searching) net_discover_stop();
    if (m.state == MENU_PLAY && memcmp(&pm.s, &pm.initial, sizeof pm.s)) {   /* (before the game reads controllers.ini) */
        if (!pad_settings_save(&pm.s)) fprintf(stderr, "[menu] cannot write controllers.ini\n");
        else fprintf(stderr, "[menu] controllers.ini: layout %s, dead zones %d%% / %d%%, rumble %d%%\n", pm.s.layout ? "labels" : "positional",
                     pm.s.deadzone_left, pm.s.deadzone_right, pm.s.rumble);
    }
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
    if (m.state == MENU_PLAY && m.s.renderer != m.initial.renderer) start_again(AGAIN_RENDERER);
#endif
}

/* ---- in the game: F9's Network window ------------------------------------------------------ *
 * The Network page over the game's picture (the renderer draws it, menu_modal_build), with Apply for
 * Play: the settings saved as the start-up menu saves them, then put to work at once (net_switch) -
 * another room joined, this PC's room started or stopped. LAN mode, to or from, starts the program
 * again (without the menu: the settings are made); a local wireless network the game is in now is
 * not cut off - the window says to leave it first. Esc, the pad's east button or F9 closes it. */
volatile int menu_modal;

void menu_modal_toggle(void) {
#ifndef _WIN32
    if (!menu_modal) drop_inputs();   /* (nothing typed before it opened) */
    menu_modal = !menu_modal;
#endif
}

#ifndef _WIN32
static struct {
    int open;               /* (the render thread's: set up, the search for rooms going) */
    NetMenu nm;
    MenuRect rects[NROWS];
    float value_x;
    double refreshed;
    char note[128];         /* why Apply did nothing, shown in the footer */
} modal;

static void modal_open(void) {
    NetSettings ns;
    net_settings_load(&ns);   /* (the mode in use: SSBU_NET_MENU_MODE, net_settings_apply's) */
    net_menu_init(&modal.nm, &ns);
    net_launch_describe(modal.nm.launched, sizeof modal.nm.launched);
    modal.nm.row = NROW_MODE;
    memset(modal.rects, 0, sizeof modal.rects);
    modal.value_x = 0;
    modal.refreshed = 0;
    modal.note[0] = 0;
    net_discover_start();
    modal.open = 1;
    fprintf(stderr, "[menu] F9: the Network window\n");
}

static void modal_close(void) {
    net_discover_stop();
    modal.open = 0;
    menu_modal = 0;
    menu_typing = 0;
}

/* Apply: the settings saved and put to work */
static void modal_apply(void) {
    NetMenu *nm = &modal.nm;
    NetSettings s = nm->s;
    net_settings_remember(&s);
    net_settings_apply(&s);
    int r = net_switch();
    if (r == NET_SWITCH_BUSY) {
        net_settings_apply(&nm->initial);   /* (as it was: nothing changed) */
        snprintf(modal.note, sizeof modal.note, "Leave the Local Wireless room in the game first");
        return;
    }
    NetSettings keep = s;   /* (as the start-up menu saves: an unchanged mode leaves netplay.ini's) */
    if (keep.mode == nm->initial.mode) keep.mode = keep.file_mode;
    if (!net_settings_save(&keep)) fprintf(stderr, "[menu] cannot write netplay.ini\n");
    if (r == NET_SWITCH_RESTART) {
        display_show_notice("LAN mode changes: starting again...", 3000);
        start_again(AGAIN_NETWORK);
        snprintf(modal.note, sizeof modal.note, "Cannot start again: the change comes at the next start");
        return;
    }
    char note[160], launched[96];
    if (s.mode == NET_MODE_HOST) snprintf(note, sizeof note, "Hosting room %s on port %s", s.room, s.port);
    else if (s.mode == NET_MODE_JOIN) snprintf(note, sizeof note, "Joining room %s at %s", s.room, s.host);
    else { net_launch_describe(launched, sizeof launched); snprintf(note, sizeof note, "Network: %s", launched); }
    display_show_notice(note, 4000);
    modal_close();
}

int menu_modal_build(unsigned w, unsigned h) {
    if (!menu_modal) {
        if (modal.open) modal_close();
        return 0;
    }
    if (!modal.open) modal_open();
    NetMenu *nm = &modal.nm;
    Input in;
    while (menu_modal && next_input(&in)) {
        int ev = in.event, mode = nm->s.mode;
        if (ev == MENU_BACK) { modal_close(); return 0; }
        if (ev == MENU_TAB) continue;
        int r = ev == MENU_CLICK ? net_click(nm, modal.rects, modal.value_x, (float)in.x, (float)in.y) : net_menu_event(nm, ev, in.x);
        if (ev != MENU_CLICK || r != NET_EV_NONE) modal.note[0] = 0;
        if (r == NET_EV_REFRESH) { net_discover_refresh(); modal.refreshed = 0; }
        if (nm->s.mode != mode) modal.refreshed = 0;
        if (r == NET_EV_PLAY) { modal_apply(); if (!modal.open) return 0; }
    }
    if (seconds() - modal.refreshed >= 0.5) { modal.refreshed = seconds(); net_refresh(nm); }
    menu_typing = net_menu_text_row(nm);

    const char *labels[NROWS], *vp[NROWS];
    char values[NROWS][200];
    int shown[NROWS];
    for (int r = 0; r < NROWS; r++) {
        labels[r] = r == NROW_PLAY ? "Apply" : net_menu_label(r);
        net_menu_value(nm, r, values[r], sizeof values[r]);
        vp[r] = values[r];
        shown[r] = net_menu_visible(nm, r);
    }
    const char *accept, *back, *pages;
    device_keys(&accept, &back, &pages);
    char footer[200];
    if (modal.note[0]) snprintf(footer, sizeof footer, "!%s\t%s=close", modal.note, menu_device == MENU_KEYS ? "F9 / Esc" : back);
    else if (menu_device == MENU_KEYS) snprintf(footer, sizeof footer, "Type=edit\tEnter=apply\tF9 / Esc=close");
    else snprintf(footer, sizeof footer, "%s=select\t%s=close", accept, back);
    overlay_menu_over(1);
    overlay_build_menu(w, h, "Network - change it without leaving the game", NROWS, labels, vp, shown, nm->row, NROW_PLAY, footer,
                       (float *)modal.rects, &modal.value_x);
    overlay_menu_over(0);
    return 1;
}
#else
int menu_modal_build(unsigned w, unsigned h) { (void)w; (void)h; return 0; }
#endif
