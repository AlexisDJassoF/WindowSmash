/* Linux: the program holds two renderers, Direct3D 11 through DXVK (gpu_d3d11.c, its entry points
 * renamed d3d11_gfx_* by gpu_d3d11_names.h) and Vulkan (gpu_vulkan.c). This file is the gfx_* the
 * rest of the runtime calls: the first call that has work for the renderer chooses one, from
 * SSBU_RENDERER (vulkan | dxvk) or else display.ini's renderer (DXVK when neither says), and every
 * call after goes to it. A
 * Vulkan renderer the machine cannot run falls back to DXVK, the reason in the log.
 *
 * The calls that only ask (the lock's owner, the window's focus, ... - the watchdog's among them,
 * made while the program is failing) never choose: before the choice they are DXVK's, which
 * answer "nothing yet" until their renderer starts.
 *
 * DXVK is loaded here, at its first use, so that a run on Vulkan never loads it. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "gpu_backend_select.h"
#include <d3d11.h>

#define D3D11_FN(ret, name, params) ret d3d11_gfx_##name params;
D3D11_FN(void, clear, (const u32 *regs, u32 arg))
D3D11_FN(void, draw, (const u32 *regs, const CbBinding cb[STAGES][18], int indexed))
D3D11_FN(void, present, (u64 guest, u32 x, u32 y, u32 w, u32 h))
D3D11_FN(int, progress, (const char *title, unsigned done, unsigned total, const char *status))
D3D11_FN(int, menu, (const char *title, int n, const char *const labels[], const char *const values[], const int shown[], int selected,
                     int button, const char *footer, float *rects, float *value_x))
D3D11_FN(void, blit2d, (const u32 *regs))
D3D11_FN(void, dispatch, (const u32 *regs, const u32 *qmd))
D3D11_FN(int, copy_resident, (u64 dst_va, u64 src_va, u64 bytes))
D3D11_FN(void, resident_flush, (u64 va, u64 bytes))
D3D11_FN(void, resident_written, (u64 va, u64 bytes))
D3D11_FN(void, stream_wrote, (u64 va, u64 bytes))
D3D11_FN(void, stream_end, (void))
D3D11_FN(int, draw_logging, (void))
D3D11_FN(void *, window_handle, (void))
D3D11_FN(void *, display_output, (void))
D3D11_FN(void, hotkey, (int n))
D3D11_FN(int, window_focused, (void))
D3D11_FN(DWORD, lock_owner, (void))
D3D11_FN(DWORD, last_enter_thread, (void))
D3D11_FN(void, report_compiles, (void (*line)(const char *text)))

static const GfxBackend d3d11 = {
    "Direct3D 11 (DXVK)",
    d3d11_gfx_clear, d3d11_gfx_draw, d3d11_gfx_present, d3d11_gfx_progress, d3d11_gfx_menu, d3d11_gfx_blit2d, d3d11_gfx_dispatch,
    d3d11_gfx_copy_resident, d3d11_gfx_resident_flush, d3d11_gfx_resident_written, d3d11_gfx_stream_wrote, d3d11_gfx_stream_end,
    d3d11_gfx_draw_logging, d3d11_gfx_window_handle, d3d11_gfx_display_output, d3d11_gfx_hotkey, d3d11_gfx_window_focused,
    d3d11_gfx_lock_owner, d3d11_gfx_last_enter_thread, d3d11_gfx_report_compiles,
};

static const GfxBackend *volatile chosen;
static pthread_once_t choose_once = PTHREAD_ONCE_INIT;

int vk_incomplete_ok;

static void choose(void) {
    const char *want = getenv("SSBU_RENDERER");
    const GfxBackend *b = NULL;
    if (want && *want && strcmp(want, "vulkan") && strcmp(want, "dxvk"))
        fprintf(stderr, "[gfx] SSBU_RENDERER=%s: not vulkan or dxvk, ignored\n", want);
    if (!want || !*want) want = display_renderer() == RENDERER_VULKAN ? "vulkan" : "dxvk";   /* (display.ini's, as the start-up menu writes it) */
    vk_incomplete_ok = 1;   /* (asked for by name, here or there: it runs as it is) */
    if (!strcmp(want, "vulkan")) {
        char why[256] = "";
        if (!(b = vk_backend_open(why, sizeof why))) fprintf(stderr, "[gfx] Vulkan: %s; using DXVK\n", why[0] ? why : "unavailable");
    }
    __atomic_store_n(&chosen, b ? b : &d3d11, __ATOMIC_RELEASE);
}

/* The renderer, chosen now if it is not yet. */
static const GfxBackend *work(void) {
    const GfxBackend *b = __atomic_load_n(&chosen, __ATOMIC_ACQUIRE);
    if (b) return b;
    pthread_once(&choose_once, choose);
    return chosen;
}

/* The renderer for a question: DXVK's until one is chosen. */
static const GfxBackend *ask(void) {
    const GfxBackend *b = __atomic_load_n(&chosen, __ATOMIC_ACQUIRE);
    return b ? b : &d3d11;
}

void gfx_clear(const u32 *regs, u32 arg) { work()->clear(regs, arg); }
void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) { work()->draw(regs, cb, indexed); }
void gfx_present(u64 guest, u32 x, u32 y, u32 w, u32 h) { work()->present(guest, x, y, w, h); }
int gfx_progress(const char *title, unsigned done, unsigned total, const char *status) { return work()->progress(title, done, total, status); }
int gfx_menu(const char *title, int n, const char *const labels[], const char *const values[], const int shown[], int selected, int button,
             const char *footer, float *rects, float *value_x) {
    return work()->menu(title, n, labels, values, shown, selected, button, footer, rects, value_x);
}
void gfx_blit2d(const u32 *regs) { work()->blit2d(regs); }
void gfx_dispatch(const u32 *regs, const u32 *qmd) { work()->dispatch(regs, qmd); }
int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes) { return work()->copy_resident(dst_va, src_va, bytes); }
void gfx_resident_flush(u64 va, u64 bytes) { work()->resident_flush(va, bytes); }
void gfx_resident_written(u64 va, u64 bytes) { work()->resident_written(va, bytes); }
void gfx_stream_wrote(u64 va, u64 bytes) { work()->stream_wrote(va, bytes); }
void gfx_stream_end(void) { work()->stream_end(); }
int gfx_draw_logging(void) { return work()->draw_logging(); }
void *gfx_window_handle(void) { return ask()->window_handle(); }
void *gfx_display_output(void) { return ask()->display_output(); }
void gfx_hotkey(int n) { ask()->hotkey(n); }
int gfx_window_focused(void) { return ask()->window_focused(); }
DWORD gfx_lock_owner(void) { return ask()->lock_owner(); }
DWORD gfx_last_enter_thread(void) { return ask()->last_enter_thread(); }
void gfx_report_compiles(void (*line)(const char *text)) { ask()->report_compiles(line); }

/* DXVK's D3D11CreateDevice (gpu_d3d11.c's, by gpu_d3d11_names.h), from the libraries next to the
 * program (its RUNPATH, $ORIGIN/lib): DXGI first, as the D3D11 library needs it and has no
 * RUNPATH of its own to find it by. */
HRESULT WINAPI rt_dxvk_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                         const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk, ID3D11Device **device,
                                         D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context) {
    static PFN_D3D11_CREATE_DEVICE create;
    if (!create) {
        void *d3d11 = NULL;
        if (!dlopen("libdxvk_dxgi.so.0", RTLD_NOW | RTLD_GLOBAL) || !(d3d11 = dlopen("libdxvk_d3d11.so.0", RTLD_NOW | RTLD_GLOBAL)) ||
            !(create = (PFN_D3D11_CREATE_DEVICE)dlsym(d3d11, "D3D11CreateDevice"))) {
            fprintf(stderr, "[gfx] cannot load DXVK: %s\n", dlerror());
            return E_FAIL;
        }
    }
    return create(adapter, type, software, flags, levels, nlevels, sdk, device, level, context);
}
