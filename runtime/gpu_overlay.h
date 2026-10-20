/* The debug overlay: frame rate, a frame time graph and statistics, drawn over each
 * presented frame (gpu_overlay.c). */
#pragma once
#include <d3d11.h>

/* Called once per presented frame, after the picture is in rtv (at x, y, w x h): notes the
 * frame's time and, if show, draws the overlay over the picture; detail (lines separated by
 * '\n', or NULL) goes under the graph. Leaves the pipeline state changed. */
void overlay_present(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x, unsigned y, unsigned w,
                     unsigned h, int show, const char *detail);

/* The start-up's shader screen: title, a bar (done of total; none while total is 0), a status
 * line and "Esc: play now", centred in the w x h rtv. Leaves the pipeline state changed. */
void overlay_progress(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned w, unsigned h,
                      const char *title, unsigned done, unsigned total, const char *status);

/* The start-up menu (gpu_menu.c): rows of label and value, the selected one lit, the button row as
 * a button; each row's place into rects (4 floats a row) and where values start into *value_x. */
void overlay_menu(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned w, unsigned h, const char *title, int n,
                  const char *const labels[], const char *const values[], const int shown[], int selected, int button, const char *footer,
                  float *rects, float *value_x);

/* A short notice (F7's mode) at the top of the picture at (x0, y0, w x h). Leaves the pipeline state changed. */
void overlay_notice(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w, unsigned h,
                    const char *note);

/* The mode bar across the top of a window, bar px tall; the mode's button place into button (x, y, w, h). */
void overlay_bar(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned w, unsigned h, unsigned bar, const char *mode,
                 int button[4]);
