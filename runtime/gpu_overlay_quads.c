/* The overlay's drawing as quads, for every renderer (gpu_overlay_quads.h): the frame rate and a
 * graph of recent frame times, the start-up's screens, F7's notice and a window's mode bar.
 *
 * Text comes from a glyph atlas GDI draws once (a monospaced font, printable ASCII, plus a
 * solid cell for rectangles); everything is quads into one vertex array. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "gpu_overlay_quads.h"

#define CELL_W 12
#define CELL_H 24
#define COLS 16
#define ATLAS_W OVERLAY_ATLAS_W
#define ATLAS_H OVERLAY_ATLAS_H     /* ' '..'~', then the solid cell (127) */
#define MAX_QUADS 8192
#define HISTORY 240

static OverlayVertex verts[MAX_QUADS * 6];
static unsigned nverts;

const OverlayVertex *overlay_quads(unsigned *count) {
    *count = nverts;
    return verts;
}

/* The glyphs: drawn white on black by GDI, kept as coverage in a one-channel texture. (Linux:
 * the same drawing in DejaVu Sans Mono Bold, a free font, kept in runtime/linux/overlay_font.h -
 * tools/linux/make_overlay_font.py writes it again.) */
#ifndef _WIN32
#include "overlay_font.h"
#endif
int overlay_atlas(unsigned char coverage[ATLAS_W * ATLAS_H]) {
#ifdef _WIN32
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader = (BITMAPINFOHEADER){sizeof(BITMAPINFOHEADER), ATLAS_W, -ATLAS_H, 1, 32, BI_RGB};
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(dc); return 0; }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    for (int c = 32; c < 127; c++) {
        char ch = (char)c;
        TextOutA(dc, (c - 32) % COLS * CELL_W, (c - 32) / COLS * CELL_H + 1, &ch, 1);
    }
    GdiFlush();
    const unsigned *px = bits;
    for (int i = 0; i < ATLAS_W * ATLAS_H; i++) coverage[i] = (unsigned char)(px[i] >> 16);   /* red */
#else
    memcpy(coverage, overlay_font, ATLAS_W * ATLAS_H);
#endif
    int solid = 127 - 32;
    for (int y = 0; y < CELL_H; y++) memset(coverage + (solid / COLS * CELL_H + y) * ATLAS_W + solid % COLS * CELL_W, 255, CELL_W);
#ifdef _WIN32
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
#endif
    return 1;
}

/* ---- drawing ------------------------------------------------------------------------- */
static void quad(float x, float y, float w, float h, int cell, unsigned color) {
    if (nverts + 6 > MAX_QUADS * 6) return;
    float u0 = (float)(cell % COLS * CELL_W) / ATLAS_W, v0 = (float)(cell / COLS * CELL_H) / ATLAS_H;
    float u1 = u0 + (float)CELL_W / ATLAS_W, v1 = v0 + (float)CELL_H / ATLAS_H;
    if (cell == 127 - 32) { u0 = u1 = u0 + 0.5f * CELL_W / ATLAS_W; v0 = v1 = v0 + 0.5f * CELL_H / ATLAS_H; }   /* solid: its middle */
    OverlayVertex a = {x, y, u0, v0, color}, b = {x + w, y, u1, v0, color}, c = {x, y + h, u0, v1, color}, d = {x + w, y + h, u1, v1, color};
    OverlayVertex *o = &verts[nverts];
    o[0] = a; o[1] = b; o[2] = c; o[3] = c; o[4] = b; o[5] = d;
    nverts += 6;
}
static void rect(float x, float y, float w, float h, unsigned color) { quad(x, y, w, h, 127 - 32, color); }

#define RGBA(r, g, b, a) ((unsigned)(r) | (unsigned)(g) << 8 | (unsigned)(b) << 16 | (unsigned)(a) << 24)

/* Text at (x, y), one line per '\n'; returns the y below it. */
static float text(float x, float y, unsigned color, const char *s) {
    float cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += CELL_H; continue; }
        int c = (unsigned char)*s;
        if (c > 32 && c < 127) {
            quad(cx + 1, y + 1, CELL_W, CELL_H, c - 32, RGBA(0, 0, 0, 200));   /* shadow */
            quad(cx, y, CELL_W, CELL_H, c - 32, color);
        }
        cx += CELL_W;
    }
    return y + CELL_H;
}

static int lines_of(const char *s, int *widest) {
    int n = 1, w = 0, cur = 0;
    for (; *s; s++) { if (*s == '\n') { n++; cur = 0; } else if (++cur > w) w = cur; }
    *widest = w;
    return n;
}

/* ---- the frame ---------------------------------------------------------------------- */
static double history[HISTORY];   /* milliseconds per frame, newest at [head - 1] */
static unsigned head, filled;

int overlay_build_present(int show, const char *detail) {
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        history[head] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        head = (head + 1) % HISTORY;
        if (filled < HISTORY) filled++;
    }
    last = now;
    if (!show) return 0;

    /* the last second: frames, slowest and fastest */
    double total = 0, worst = 0, best = 1e9;
    unsigned frames = 0;
    for (unsigned i = 0; i < filled && total < 1000.0; i++) {
        double ms = history[(head + HISTORY - 1 - i) % HISTORY];
        total += ms; frames++;
        if (ms > worst) worst = ms;
        if (ms < best) best = ms;
    }
    double fps = total > 0 ? frames * 1000.0 / total : 0, last_ms = filled ? history[(head + HISTORY - 1) % HISTORY] : 0;
    nverts = 0;
    char line[256];
    float x = 16, y = 16, gw = 2.0f * HISTORY, gh = 100;   /* graph: 2 px a frame; 100 px = 50 ms */
    int detail_w = 0, detail_lines = detail ? lines_of(detail, &detail_w) : 0;
    float panel_w = gw + 16, panel_h = 2 * CELL_H + 8 + gh + 12 + detail_lines * CELL_H;
    if (detail_w * CELL_W + 16 > panel_w) panel_w = (float)(detail_w * CELL_W + 16);
    rect(x - 8, y - 6, panel_w, panel_h, RGBA(0, 0, 0, 150));
    unsigned fps_color = fps >= 57 ? RGBA(120, 255, 120, 255) : fps >= 45 ? RGBA(255, 230, 90, 255) : RGBA(255, 100, 90, 255);
    snprintf(line, sizeof line, "%5.1f FPS  %5.1f ms", fps, last_ms);
    y = text(x, y, fps_color, line);
    snprintf(line, sizeof line, "last second: %.1f-%.1f ms", frames ? best : 0, worst);
    y = text(x, y, RGBA(230, 230, 230, 255), line);
    y += 6;
    /* the graph: a bar per frame, its height its time; lines at 60 and 30 frames a second */
    float scale = gh / 50.0f;
    rect(x, y, gw, gh, RGBA(20, 20, 20, 160));
    for (unsigned i = 0; i < filled; i++) {
        double ms = history[(head + HISTORY - filled + i) % HISTORY];
        float bh = (float)(ms > 50 ? 50 : ms) * scale;
        unsigned c = ms <= 17.5 ? RGBA(90, 220, 90, 230) : ms <= 34 ? RGBA(240, 210, 60, 230) : RGBA(240, 70, 60, 240);
        rect(x + gw - 2.0f * (filled - i), y + gh - bh, 2, bh, c);
    }
    rect(x, y + gh - 16.67f * scale, gw, 1, RGBA(255, 255, 255, 140));
    rect(x, y + gh - 33.33f * scale, gw, 1, RGBA(255, 255, 255, 70));
    y += gh + 6;
    if (detail) text(x, y, RGBA(210, 225, 255, 255), detail);
    return 1;
}

/* The start-up's shader screen (gpu_precompile.c): a title, a bar of done out of total (none
 * while total is 0), a line under it and the way out, centred in the w x h back buffer. */
void overlay_build_progress(unsigned w, unsigned h, const char *title, unsigned done, unsigned total, const char *status) {
    nverts = 0;
    float bw = w * 0.6f < 360 ? 360 : w * 0.6f, bh = 18, x = (w - bw) / 2, y = h / 2.0f - 2 * CELL_H;
    if (bw > w - 32.0f) { bw = w - 32.0f; x = 16; }
    text((w - (float)strlen(title) * CELL_W) / 2, y, RGBA(240, 240, 240, 255), title);
    y += CELL_H + 12;
    rect(x, y, bw, bh, RGBA(60, 60, 64, 255));
    if (total) {
        float f = done >= total ? 1.0f : (float)done / total;
        rect(x, y, bw * f, bh, RGBA(90, 200, 110, 255));
    }
    y += bh + 12;
    if (status) text((w - (float)strlen(status) * CELL_W) / 2, y, RGBA(210, 210, 210, 255), status);
    y += CELL_H + 6;
    static const char hint[] = "Esc: play now";
    text((w - (float)(sizeof hint - 1) * CELL_W) / 2, y, RGBA(150, 150, 150, 255), hint);
}

/* The start-up menu (gpu_menu.c): a title, then a row per shown item - its label, and its value
 * between < > - the selected one lit, the button row (Play) as a button, and a footer; centred
 * in the w x h back buffer. Each row's place goes to rects (x, y, w, h; 0 for a row not shown),
 * and where the values start to *value_x, for clicks. */
void overlay_build_menu(unsigned w, unsigned h, const char *title, int n, const char *const labels[], const char *const values[],
                        const int shown[], int selected, int button, const char *footer, float *rects, float *value_x) {
    nverts = 0;
    int rows = 0;
    for (int i = 0; i < n; i++) rows += shown[i] != 0;
    /* (labels up to 17 characters, values up to 26: "<  75% (1440 x 810)  >", "(set by SSBU_...)") */
    float row_h = CELL_H + 14, pw = 46 * CELL_W + 48, ph = (rows + 1) * row_h + 3 * CELL_H + 40;
    if (pw > w - 16.0f) pw = w - 16.0f;
    float px = (w - pw) / 2, py = (h - ph) / 2, lx = px + 24, vx = px + 24 + 18 * CELL_W;
    rect(px, py, pw, ph, RGBA(24, 24, 28, 235));
    text((w - (float)strlen(title) * CELL_W) / 2, py + 14, RGBA(240, 240, 240, 255), title);
    float y = py + 14 + CELL_H + 18;
    for (int i = 0; i < n; i++) {
        float *r = rects + 4 * i;
        r[0] = r[1] = r[2] = r[3] = 0;
        if (!shown[i]) continue;
        int sel = i == selected;
        if (i == button) {
            y += 8;
            float bw = 12.0f * CELL_W, bx = (w - bw) / 2;
            rect(bx, y, bw, row_h - 4, sel ? RGBA(90, 200, 110, 255) : RGBA(60, 60, 66, 255));
            text((w - (float)strlen(labels[i]) * CELL_W) / 2, y + (row_h - 4 - CELL_H) / 2, sel ? RGBA(10, 10, 10, 255) : RGBA(230, 230, 230, 255),
                 labels[i]);
            r[0] = bx, r[1] = y, r[2] = bw, r[3] = row_h - 4;
        } else {
            if (sel) rect(px + 8, y - 4, pw - 16, row_h - 6, RGBA(70, 90, 140, 200));
            text(lx, y + 2, RGBA(225, 225, 225, 255), labels[i]);
            char v[96];
            snprintf(v, sizeof v, "<  %s  >", values[i]);
            text(vx, y + 2, sel ? RGBA(255, 255, 255, 255) : RGBA(190, 200, 220, 255), v);
            r[0] = px + 8, r[1] = y - 4, r[2] = pw - 16, r[3] = row_h - 6;
        }
        y += row_h;
    }
    if (footer) text((w - (float)strlen(footer) * CELL_W) / 2, py + ph - CELL_H - 12, RGBA(150, 150, 150, 255), footer);
    *value_x = vx;
}

/* A short notice (F7's mode) at the top of a w x h picture, in a box. */
void overlay_build_notice(unsigned w, unsigned h, const char *note) {
    nverts = 0;
    float tw = (float)strlen(note) * CELL_W, bw = tw + 40, bh = CELL_H + 20, bx = (w - bw) / 2, by = h * 0.06f;
    rect(bx, by, bw, bh, RGBA(20, 20, 24, 210));
    rect(bx, by + bh - 3, bw, 3, RGBA(90, 200, 110, 255));
    text(bx + 20, by + 10, RGBA(245, 245, 245, 255), note);
}

/* The mode bar across the top of a window (bar px tall): the mode as a button, and the keys. The
 * button's place goes to button (x, y, w, h). */
void overlay_build_bar(unsigned w, unsigned h, unsigned bar, const char *mode, int button[4]) {
    (void)h;
    nverts = 0;
    float ty = (bar - CELL_H) / 2.0f;
    rect(0, 0, (float)w, (float)bar, RGBA(32, 32, 38, 255));
    rect(0, (float)bar - 1, (float)w, 1, RGBA(70, 70, 80, 255));
    text(10, ty, RGBA(170, 170, 180, 255), "Mode:");
    float bx = 10 + 6.0f * CELL_W, bw = (float)strlen(mode) * CELL_W + 24, bh = bar - 8.0f;
    rect(bx, 4, bw, bh, RGBA(70, 90, 140, 255));
    text(bx + 12, ty, RGBA(245, 245, 245, 255), mode);
    static const char keys[] = "F7: docked/handheld   F11: full screen";
    float kx = w - (float)(sizeof keys - 1) * CELL_W - 12;
    if (kx > bx + bw + 24) text(kx, ty, RGBA(140, 140, 150, 255), keys);
    button[0] = (int)bx, button[1] = 4, button[2] = (int)bw, button[3] = (int)bh;
}
