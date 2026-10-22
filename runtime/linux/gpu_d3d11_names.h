/* Linux: gpu_d3d11.c's entry points under names of their own (build_linux.py force-includes this
 * file when it compiles gpu_d3d11.c, and only then), so that the program can hold both renderers.
 * The gfx_* names the rest of the runtime calls are gpu_backend_select.c's, which passes each call
 * on to the renderer chosen at start (SSBU_RENDERER). The Windows build never sees this file:
 * there gpu_d3d11.c's functions keep their names and are the only renderer.
 *
 * D3D11CreateDevice is DXVK's, loaded at run time (gpu_backend_select.c) so that a run on the
 * Vulkan renderer never loads DXVK. */
#pragma once

#define gfx_blit2d d3d11_gfx_blit2d
#define gfx_clear d3d11_gfx_clear
#define gfx_copy_resident d3d11_gfx_copy_resident
#define gfx_dispatch d3d11_gfx_dispatch
#define gfx_display_output d3d11_gfx_display_output
#define gfx_draw d3d11_gfx_draw
#define gfx_draw_logging d3d11_gfx_draw_logging
#define gfx_hotkey d3d11_gfx_hotkey
#define gfx_last_enter_thread d3d11_gfx_last_enter_thread
#define gfx_lock_owner d3d11_gfx_lock_owner
#define gfx_menu d3d11_gfx_menu
#define gfx_present d3d11_gfx_present
#define gfx_progress d3d11_gfx_progress
#define gfx_report_compiles d3d11_gfx_report_compiles
#define gfx_resident_flush d3d11_gfx_resident_flush
#define gfx_resident_written d3d11_gfx_resident_written
#define gfx_stream_end d3d11_gfx_stream_end
#define gfx_stream_wrote d3d11_gfx_stream_wrote
#define gfx_window_focused d3d11_gfx_window_focused
#define gfx_window_handle d3d11_gfx_window_handle

#define D3D11CreateDevice rt_dxvk_D3D11CreateDevice
