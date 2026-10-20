/* The game's window on Linux: SDL2, on the process's main thread.
 *
 * On Windows the renderer makes a Win32 window on a thread of its own and reads the keyboard
 * with GetAsyncKeyState. Here the main thread creates an SDL window at start-up (DXVK presents
 * into it: its HWND is the SDL_Window) and then runs the event loop for good, keeping what the
 * rest of the runtime asks for up to date: the keyboard (behind GetAsyncKeyState), focus, the
 * debug keys F1-F6 and the window's close button. Game controllers are hle_pad.c's: its input
 * thread reads them (SDL starts them here, with their events off).
 *
 * display.ini (gpu_display.c) sizes the window and picks its mode: windowed, or SDL's desktop
 * full screen for borderless and fullscreen alike (the monitor keeps its mode). F11 or Alt+Enter
 * switches between the two, as on Windows. The window's inside in pixels goes to the renderer,
 * whose swap chain follows it (the picture then 16:9 within); the pointer hides after 2 s
 * without moving (hide_cursor). */
#define _GNU_SOURCE 1
#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "../display.h"
#include "../gpu_menu.h"
#include "../gpu_precompile.h"

static SDL_Window *window;
static volatile int focused;
static const Uint8 *keys;
static int nkeys;
static int mode;                    /* the window's mode now (DISPLAY_*) */
static int hide_cursor, cursor_hidden;
static Uint64 mouse_moved_at;

void gfx_hotkey(int n);   /* (gpu_d3d11.c) F1 + n was pressed */

/* ---- display.ini's window ----------------------------------------------------------------- */
static void note_size(void) {   /* the window's inside, in pixels, for the swap chain */
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    if (w > 0 && h > 0) display_note_size((unsigned)w, (unsigned)h);
}

static void note_refresh(void) {   /* the refresh rate of the window's monitor */
    SDL_DisplayMode dm;
    int i = SDL_GetWindowDisplayIndex(window);
    if (i >= 0 && !SDL_GetCurrentDisplayMode(i, &dm)) display_note_refresh(dm.refresh_rate);
}

static const char *mode_name(int m) { return m == DISPLAY_WINDOWED ? "windowed" : m == DISPLAY_BORDERLESS ? "borderless" : "fullscreen"; }

#define BAR_PX 32   /* the mode bar at the top of a window (gpu_d3d11.c draws it; its button is F7) */

static void set_mode(int m) {
    if (m == mode) return;
    mode = m;
    display_note_bar(m == DISPLAY_WINDOWED ? BAR_PX : 0);
    if (SDL_SetWindowFullscreen(window, m == DISPLAY_WINDOWED ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP))
        fprintf(stderr, "[display] cannot switch to %s: %s\n", mode_name(m), SDL_GetError());
    else fprintf(stderr, "[display] %s\n", mode_name(m));
}

/* (every turn of the event loop) the pointer hides over the focused window after 2 s still */
static void idle_pointer(Uint64 now) {
    if (hide_cursor && !cursor_hidden && focused && SDL_GetMouseFocus() == window && now - mouse_moved_at > 2000) {
        SDL_ShowCursor(SDL_DISABLE);
        cursor_hidden = 1;
    }
}

/* ---- the keyboard ------------------------------------------------------------------- */
static SDL_Scancode scancode_of(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F1 + 11) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_SHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_CONTROL: return SDL_SCANCODE_LCTRL;
    case VK_MENU: return SDL_SCANCODE_LALT;
    }
    return SDL_SCANCODE_UNKNOWN;
}

SHORT GetAsyncKeyState(int vk) {
    if (!keys) return 0;
    SDL_Scancode s = scancode_of(vk);
    int down = s != SDL_SCANCODE_UNKNOWN && (int)s < nkeys && keys[s];
    if (vk == VK_SHIFT) down |= keys[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) down |= keys[SDL_SCANCODE_RCTRL];
    if (vk == VK_MENU) down |= keys[SDL_SCANCODE_RALT];
    return down ? (SHORT)0x8000 : 0;
}

HWND GetForegroundWindow(void) { return focused ? (HWND)window : NULL; }
void *rt_window(void) { return window; }

/* ---- start-up and the event loop ---------------------------------------------------- */
int rt_window_init(void) {
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);   /* (DXVK's native build asks which window system) */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");   /* positional, as hle_pad.c expects */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* (controllers.ini's background = ... decides) */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");              /* hle_pad.c's input thread updates the pads */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "Super Smash Bros. Ultimate");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[gfx] SDL could not start the video system: %s\n", SDL_GetError());
        return 0;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "[audio] SDL could not start audio: %s\n", SDL_GetError());
    SDL_GameControllerEventState(SDL_IGNORE);   /* (the pads' state is read, not their events) */
    SDL_JoystickEventState(SDL_IGNORE);
    int width, height;
    display_window_size(&width, &height);
    mode = display_window_mode();
    if (mode == DISPLAY_WINDOWED) height += BAR_PX;   /* (the mode bar above the picture: the picture keeps its size) */
    display_note_bar(mode == DISPLAY_WINDOWED ? BAR_PX : 0);
    window = SDL_CreateWindow("Super Smash Bros. Ultimate (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                              (mode == DISPLAY_WINDOWED ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (!window) {
        fprintf(stderr, "[gfx] cannot create the window: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetWindowMinimumSize(window, 640, 360);
    if (mode != DISPLAY_WINDOWED) fprintf(stderr, "[display] %s\n", mode_name(mode));
    hide_cursor = display_hide_cursor();
    mouse_moved_at = SDL_GetTicks64();
    note_size();
    note_refresh();
    keys = SDL_GetKeyboardState(&nkeys);
    focused = 1;
    fprintf(stderr, "[gfx] window ready (SDL %s video driver)\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    return 1;
}

/* ---- the start-up menu's input (gpu_menu.c) ----------------------------------------------
 * While it is up, the arrows, Enter, Esc and left clicks go to it, not to the game; and the
 * controllers are read as hle_pad.c reads them while playing - their state, not their events
 * (this loop has those off), with gamecontrollerdb.txt's mappings, looked for every 100 ms -
 * the D-pad and left stick as presses that repeat while held, the south button to play, the east
 * one to start without changes. Once the menu closes they are closed again, for hle_pad.c. */
static SDL_GameController *menu_pads[8];
static int menu_pads_open;

static void menu_pads_stop(void) {
    for (int i = 0; i < 8; i++) if (menu_pads[i]) { SDL_GameControllerClose(menu_pads[i]); menu_pads[i] = NULL; }
    menu_pads_open = 0;
}

static void menu_pads_poll(Uint64 now) {
    static Uint64 scanned, held_since, repeated;
    static int held = -1, was_accept, was_back;
    if (!menu_pads_open) {
        SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");   /* (as hle_pad.c: some pads need it) */
        menu_pads_open = 1;
        scanned = 0;
    }
    SDL_GameControllerUpdate();
    if (now - scanned >= 100) {   /* pads that came and went */
        scanned = now;
        for (int i = 0; i < 8; i++)
            if (menu_pads[i] && !SDL_GameControllerGetAttached(menu_pads[i])) { SDL_GameControllerClose(menu_pads[i]); menu_pads[i] = NULL; }
        for (int k = 0; k < SDL_NumJoysticks() && k < 8; k++) {
            static int told;
            if (!SDL_IsGameController(k)) {   /* (no mapping: the game would not read it either) */
                if (!(told & 1 << k)) fprintf(stderr, "[menu] joystick %d (%s) has no controller mapping\n", k, SDL_JoystickNameForIndex(k));
                told |= 1 << k;
                continue;
            }
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(k);
            int open = 0;
            for (int i = 0; i < 8 && !open; i++) open = menu_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(menu_pads[i])) == id;
            for (int i = 0; i < 8 && !open; i++)
                if (!menu_pads[i]) {
                    menu_pads[i] = SDL_GameControllerOpen(k);
                    open = 1;
                    const char *name = menu_pads[i] ? SDL_GameControllerName(menu_pads[i]) : NULL;
                    fprintf(stderr, "[menu] controller: %s\n", menu_pads[i] ? (name ? name : "gamepad") : SDL_GetError());
                }
        }
    }
    /* every pad's buttons together: a direction (D-pad or stick past half way), accept, back */
    int dir = -1, accept = 0, back = 0;
    for (int i = 0; i < 8; i++) {
        SDL_GameController *gp = menu_pads[i];
        if (!gp) continue;
        int x = SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -16000) dir = MENU_UP;
        else if (SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 16000) dir = MENU_DOWN;
        else if (SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -16000) dir = MENU_LEFT;
        else if (SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 16000) dir = MENU_RIGHT;
        accept |= SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_A);   /* (positional: the south button) */
        back |= SDL_GameControllerGetButton(gp, SDL_CONTROLLER_BUTTON_B);     /* (the east one) */
    }
    if (dir != held) {   /* a new press: now, then again after 400 ms held, every 120 ms */
        held = dir;
        held_since = repeated = now;
        if (dir >= 0) menu_input(dir, 0, 0);
    } else if (dir >= 0 && now - held_since >= 400 && now - repeated >= 120) {
        repeated = now;
        menu_input(dir, 0, 0);
    }
    if (accept && !was_accept) menu_input(MENU_ACCEPT, 0, 0);
    if (back && !was_back) menu_input(MENU_BACK, 0, 0);
    was_accept = accept;
    was_back = back;
}

/* 1 if the menu took the event */
static int menu_event_of(const SDL_Event *e) {
    if (!menu_active) return 0;
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: menu_input(MENU_UP, 0, 0); return 1;
        case SDL_SCANCODE_DOWN: menu_input(MENU_DOWN, 0, 0); return 1;
        case SDL_SCANCODE_LEFT: menu_input(MENU_LEFT, 0, 0); return 1;
        case SDL_SCANCODE_RIGHT: menu_input(MENU_RIGHT, 0, 0); return 1;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            if (e->key.keysym.mod & KMOD_ALT) return 0;   /* (Alt+Enter: full screen) */
            if (!e->key.repeat) menu_input(MENU_ACCEPT, 0, 0);
            return 1;
        case SDL_SCANCODE_ESCAPE: if (!e->key.repeat) menu_input(MENU_BACK, 0, 0); return 1;
        default: return 0;
        }
    case SDL_MOUSEBUTTONDOWN: {
        if (e->button.button != SDL_BUTTON_LEFT) return 0;
        int ww = 1, wh = 1, dw = 1, dh = 1;   /* (window units to the drawable's pixels: they differ on a scaled desktop) */
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
        menu_input(MENU_CLICK, e->button.x * dw / (ww ? ww : 1), e->button.y * dh / (wh ? wh : 1));
        return 1;
    }
    }
    return 0;
}

/* Runs on the main thread for good. `status` is called every `every` seconds (0: never). */
void rt_window_loop(int every, void (*status)(void)) {
    Uint64 last = SDL_GetTicks64();
    for (;;) {
        SDL_Event e;
        if (menu_active) menu_pads_poll(SDL_GetTicks64());
        else if (menu_pads_open) menu_pads_stop();
        int request = display_take_window_request();   /* (the menu's Window setting) */
        if (request >= 0 && window) set_mode(request);
        int got = window ? SDL_WaitEventTimeout(&e, 8) : (SDL_Delay(50), 0);
        while (got) {
            if (menu_event_of(&e)) { got = SDL_PollEvent(&e); continue; }
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "[gfx] window closed\n");
                ExitProcess(0);
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = 1;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = 0;
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) note_size();
                if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) note_refresh();
                break;
            case SDL_MOUSEBUTTONDOWN:   /* the mode bar's button (in a window) */
                if (e.button.button == SDL_BUTTON_LEFT && !precompile_active) {
                    int ww = 1, wh = 1, dw = 1, dh = 1;
                    SDL_GetWindowSize(window, &ww, &wh);
                    SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
                    if (display_bar_hit(e.button.x * dw / (ww ? ww : 1), e.button.y * dh / (wh ? wh : 1))) display_toggle_operation_mode();
                }
                break;
            case SDL_MOUSEMOTION:
                mouse_moved_at = SDL_GetTicks64();
                if (cursor_hidden) { SDL_ShowCursor(SDL_ENABLE); cursor_hidden = 0; }
                break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && precompile_active) precompile_skip = 1;   /* (the start-up's shader screen) */
                if (e.key.keysym.scancode >= SDL_SCANCODE_F1 && e.key.keysym.scancode <= SDL_SCANCODE_F6)
                    gfx_hotkey(e.key.keysym.scancode - SDL_SCANCODE_F1);
                if (e.key.keysym.scancode == SDL_SCANCODE_F7 && !menu_active && !precompile_active) display_toggle_operation_mode();
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    int full = display_window_mode() == DISPLAY_FULLSCREEN ? DISPLAY_FULLSCREEN : DISPLAY_BORDERLESS;
                    set_mode(mode == DISPLAY_WINDOWED ? full : DISPLAY_WINDOWED);
                }
                break;
            }
            got = SDL_PollEvent(&e);
        }
        Uint64 now = SDL_GetTicks64();
        if (window) idle_pointer(now);
        if (every > 0 && status && now - last >= (Uint64)every * 1000) {
            last = now;
            status();
        }
    }
}
