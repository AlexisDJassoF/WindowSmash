/* The game's window on Linux: SDL2, on the process's main thread.
 *
 * On Windows the renderer makes a Win32 window on a thread of its own and reads the keyboard
 * with GetAsyncKeyState and the gamepad with XInput. Here the main thread creates an SDL window
 * at start-up (DXVK presents into it: its HWND is the SDL_Window) and then runs the event loop
 * for good, keeping what the rest of the runtime asks for up to date: the keyboard (behind
 * GetAsyncKeyState), the first game controller (behind an XInput-shaped state), focus, the
 * debug keys F1-F6 and the window's close button. F11 or Alt+Enter toggles full screen, as
 * Alt+Enter does on Windows. */
#define _GNU_SOURCE 1
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#define WINDOW_W 1280
#define WINDOW_H 720

static SDL_Window *window;
static volatile int focused;
static const Uint8 *keys;
static int nkeys;

void gfx_hotkey(int n);   /* (gpu_d3d11.c) F1 + n was pressed */

/* ---- the gamepad, as XInput reports one --------------------------------------------- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
static SDL_GameController *pad;
static SDL_mutex *pad_lock;
static XState pad_state;
static int pad_present;

static SHORT stick(Sint16 v, int flip) {
    int x = flip ? -(int)v - 1 : v;   /* SDL's y grows downwards, XInput's upwards */
    return (SHORT)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

static void read_pad(void) {
    XState s = {0};
    int present = 0;
    if (pad && SDL_GameControllerGetAttached(pad)) {
        present = 1;
        static const struct { int sdl; WORD x; } map[] = {   /* positional: SDL's A is the bottom button, like XInput's */
            {SDL_CONTROLLER_BUTTON_A, 0x1000}, {SDL_CONTROLLER_BUTTON_B, 0x2000}, {SDL_CONTROLLER_BUTTON_X, 0x4000},
            {SDL_CONTROLLER_BUTTON_Y, 0x8000}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200}, {SDL_CONTROLLER_BUTTON_START, 0x0010},
            {SDL_CONTROLLER_BUTTON_BACK, 0x0020}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040},
            {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080}, {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008},
        };
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
            if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)map[i].sdl)) s.pad.buttons |= map[i].x;
        s.pad.lt = (BYTE)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        s.pad.rt = (BYTE)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        s.pad.lx = stick(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), 0);
        s.pad.ly = stick(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), 1);
        s.pad.rx = stick(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX), 0);
        s.pad.ry = stick(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY), 1);
    }
    SDL_LockMutex(pad_lock);
    s.packet = pad_state.packet + (memcmp(&s.pad, &pad_state.pad, sizeof s.pad) != 0);
    pad_state = s;
    pad_present = present;
    SDL_UnlockMutex(pad_lock);
}

static void open_pad(void) {
    if (pad && SDL_GameControllerGetAttached(pad)) return;
    if (pad) { SDL_GameControllerClose(pad); pad = NULL; }
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i)))
            fprintf(stderr, "[input] controller: %s\n", SDL_GameControllerName(pad) ? SDL_GameControllerName(pad) : "?");
}

/* XInputGetState(0, &state) for hle_hid.c: 0, or ERROR_DEVICE_NOT_CONNECTED (1167). */
DWORD rt_xinput_get_state(DWORD index, void *state) {
    if (index || !pad_lock) return 1167;
    SDL_LockMutex(pad_lock);
    int present = pad_present;
    memcpy(state, &pad_state, sizeof pad_state);
    SDL_UnlockMutex(pad_lock);
    return present ? 0 : 1167;
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
    }
    return SDL_SCANCODE_UNKNOWN;
}

SHORT GetAsyncKeyState(int vk) {
    if (!keys) return 0;
    SDL_Scancode s = scancode_of(vk);
    int down = s != SDL_SCANCODE_UNKNOWN && (int)s < nkeys && keys[s];
    if (vk == VK_SHIFT) down |= keys[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) down |= keys[SDL_SCANCODE_RCTRL];
    return down ? (SHORT)0x8000 : 0;
}

HWND GetForegroundWindow(void) { return focused ? (HWND)window : NULL; }
void *rt_window(void) { return window; }

/* ---- start-up and the event loop ---------------------------------------------------- */
int rt_window_init(void) {
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);   /* (DXVK's native build asks which window system) */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");   /* positional, as XInput */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "Super Smash Bros. Ultimate");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[gfx] SDL could not start the video system: %s\n", SDL_GetError());
        return 0;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "[audio] SDL could not start audio: %s\n", SDL_GetError());
    pad_lock = SDL_CreateMutex();
    window = SDL_CreateWindow("Super Smash Bros. Ultimate (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_W, WINDOW_H,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "[gfx] cannot create the window: %s\n", SDL_GetError());
        return 0;
    }
    keys = SDL_GetKeyboardState(&nkeys);
    focused = 1;
    open_pad();
    read_pad();
    fprintf(stderr, "[gfx] window ready (SDL %s video driver)\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    return 1;
}

/* Runs on the main thread for good. `status` is called every `every` seconds (0: never). */
void rt_window_loop(int every, void (*status)(void)) {
    Uint64 last = SDL_GetTicks64();
    for (;;) {
        SDL_Event e;
        int got = window ? SDL_WaitEventTimeout(&e, 8) : (SDL_Delay(50), 0);
        while (got) {
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "[gfx] window closed\n");
                ExitProcess(0);
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = 1;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = 0;
                break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                if (e.key.keysym.scancode >= SDL_SCANCODE_F1 && e.key.keysym.scancode <= SDL_SCANCODE_F6)
                    gfx_hotkey(e.key.keysym.scancode - SDL_SCANCODE_F1);
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    int full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
                open_pad();
                break;
            }
            got = SDL_PollEvent(&e);
        }
        if (window) read_pad();
        Uint64 now = SDL_GetTicks64();
        if (every > 0 && status && now - last >= (Uint64)every * 1000) {
            last = now;
            status();
        }
    }
}
