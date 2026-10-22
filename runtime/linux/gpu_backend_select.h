/* Linux: the renderers the program holds, as one table of the gfx_* entry points each provides
 * (gpu_backend.h and the few the rest of the runtime declares where it calls them). The chosen
 * one answers every gfx_* call; see gpu_backend_select.c. */
#pragma once
#include "gpu_backend.h"
#include <windows.h>   /* (DWORD) */

typedef struct GfxBackend {
    const char *name;                 /* for the log: "Direct3D 11 (DXVK)", "Vulkan" */
    void (*clear)(const u32 *regs, u32 arg);
    void (*draw)(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);
    void (*present)(u64 guest, u32 x, u32 y, u32 w, u32 h);
    int (*progress)(const char *title, unsigned done, unsigned total, const char *status);
    int (*menu)(const char *title, int n, const char *const labels[], const char *const values[], const int shown[], int selected,
                int button, const char *footer, float *rects, float *value_x);
    void (*blit2d)(const u32 *regs);
    void (*dispatch)(const u32 *regs, const u32 *qmd);
    int (*copy_resident)(u64 dst_va, u64 src_va, u64 bytes);
    void (*resident_flush)(u64 va, u64 bytes);
    void (*resident_written)(u64 va, u64 bytes);
    void (*stream_wrote)(u64 va, u64 bytes);
    void (*stream_end)(void);
    int (*draw_logging)(void);
    void *(*window_handle)(void);
    void *(*display_output)(void);    /* (an IDXGIOutput under DXVK; NULL where there is none) */
    void (*hotkey)(int n);
    int (*window_focused)(void);
    DWORD (*lock_owner)(void);
    DWORD (*last_enter_thread)(void);
    void (*report_compiles)(void (*line)(const char *text));
} GfxBackend;

/* The Vulkan renderer (gpu_vulkan.c), if this machine can run it: NULL otherwise, with the reason
 * in why. Called once, from the first gfx_* call. */
const GfxBackend *vk_backend_open(char *why, size_t why_size);
