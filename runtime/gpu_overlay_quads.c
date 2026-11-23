/* The overlay's drawing as quads, for every renderer (gpu_overlay_quads.h): the frame rate and a
 * graph of recent frame times, the start-up's screens, F7's notice and a window's mode bar.
 *
 * Text comes from a glyph atlas GDI draws once (a monospaced font, printable ASCII, plus a
 * solid cell for rectangles); everything is quads into one vertex array. */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
    /* each cell's first row empty: the glyphs are drawn from its second, and '_' reaches past its cell into
     * the next row's ('o' had a bar over it); the empty row also keeps scaled text from the cell above */
    for (int c = 0; c < 127 - 32; c++) memset(coverage + (c / COLS * CELL_H) * ATLAS_W + c % COLS * CELL_W, 0, CELL_W);
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

/* ---- the start-up menu's drawing ------------------------------------------------------------
 * Shapes from the solid cell, each vertex its own colour: gradients, rounded boxes (a scanline a
 * row of their corners, its ends' pixels partly covered), soft shadows and glows (fans fading out),
 * and small icons from triangles. */
#define SOLID (127 - 32)
#define PI_F 3.14159265f

static void vtx(float x, float y, unsigned color) {
    float u = (SOLID % COLS * CELL_W + 0.5f * CELL_W) / ATLAS_W, v = (SOLID / COLS * CELL_H + 0.5f * CELL_H) / ATLAS_H;
    verts[nverts++] = (OverlayVertex){x, y, u, v, color};
}
static void tri(float x0, float y0, unsigned c0, float x1, float y1, unsigned c1, float x2, float y2, unsigned c2) {
    if (nverts + 3 > MAX_QUADS * 6) return;
    vtx(x0, y0, c0); vtx(x1, y1, c1); vtx(x2, y2, c2);
}
/* A solid quad, a colour a corner: top left, top right, bottom left, bottom right */
static void quad4(float x, float y, float w, float h, unsigned tl, unsigned tr, unsigned bl, unsigned br) {
    if (w <= 0 || h <= 0) return;
    tri(x, y, tl, x + w, y, tr, x, y + h, bl);
    tri(x, y + h, bl, x + w, y, tr, x + w, y + h, br);
}
static unsigned chan(unsigned c, int s) { return c >> s & 255; }
static unsigned mix(unsigned a, unsigned b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    unsigned out = 0;
    for (int s = 0; s < 32; s += 8) out |= (unsigned)(chan(a, s) + (chan(b, s) - (float)chan(a, s)) * t + 0.5f) << s;
    return out;
}
static unsigned fade(unsigned c, float f) {   /* its alpha times f */
    float a = chan(c, 24) * (f < 0 ? 0 : f > 1 ? 1 : f);
    return (c & 0xFFFFFFu) | (unsigned)(a + 0.5f) << 24;
}
static void vgrad(float x, float y, float w, float h, unsigned top, unsigned bottom) { quad4(x, y, w, h, top, top, bottom, bottom); }
static void hgrad(float x, float y, float w, float h, unsigned left, unsigned right) { quad4(x, y, w, h, left, right, left, right); }

/* A rounded box, r its corners' radius, its colour top to bottom */
static void rbox(float x, float y, float w, float h, float r, unsigned top, unsigned bottom) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int rows = (int)ceilf(r);
    for (int i = 0; i < rows; i++) {
        float dy = r - (i + 0.5f), d = r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0);   /* (the inset of this scanline) */
        for (int bottom_half = 0; bottom_half < 2; bottom_half++) {
            float sy = bottom_half ? y + h - i - 1 : y + i;
            unsigned c = mix(top, bottom, (sy + 0.5f - y) / h);
            float l = x + d, rr = x + w - d, li = ceilf(l), ri = floorf(rr);
            if (ri > li) quad4(li, sy, ri - li, 1, c, c, c, c);
            if (li - l > 0.01f) quad4(li - 1, sy, 1, 1, fade(c, li - l), fade(c, li - l), fade(c, li - l), fade(c, li - l));   /* (the end's pixel, partly) */
            if (rr - ri > 0.01f) quad4(ri, sy, 1, 1, fade(c, rr - ri), fade(c, rr - ri), fade(c, rr - ri), fade(c, rr - ri));
        }
    }
    vgrad(x, y + rows, w, h - 2 * rows, mix(top, bottom, rows / h), mix(top, bottom, 1 - rows / h));
}
/* A rounded box with a border of 1 px: border, then fill */
static void rbox_border(float x, float y, float w, float h, float r, unsigned border, unsigned top, unsigned bottom) {
    rbox(x, y, w, h, r, border, border);
    rbox(x + 1, y + 1, w - 2, h - 2, r - 1, top, bottom);
}

/* A soft shadow or glow around the box (x, y, w, h): color inside, fading out over s px */
static void soft(float x, float y, float w, float h, float s, unsigned color) {
    unsigned out = fade(color, 0);
    quad4(x, y, w, h, color, color, color, color);
    vgrad(x, y - s, w, s, out, color);
    vgrad(x, y + h, w, s, color, out);
    hgrad(x - s, y, s, h, out, color);
    hgrad(x + w, y, s, h, color, out);
    const float cx[4] = {x, x + w, x, x + w}, cy[4] = {y, y, y + h, y + h}, a0[4] = {PI_F, 1.5f * PI_F, 0.5f * PI_F, 0};
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 8; i++) {
            float a = a0[k] + i * (PI_F / 2) / 8, b = a + (PI_F / 2) / 8;
            tri(cx[k], cy[k], color, cx[k] + cosf(a) * s, cy[k] + sinf(a) * s, out, cx[k] + cosf(b) * s, cy[k] + sinf(b) * s, out);
        }
}
/* A glow: an ellipse fading from color in the middle to nothing */
static void glow(float cx, float cy, float rx, float ry, unsigned color) {
    unsigned mid = fade(color, 0.45f), out = fade(color, 0);
    for (int i = 0; i < 40; i++) {
        float a = i * 2 * PI_F / 40, b = a + 2 * PI_F / 40, ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        tri(cx, cy, color, cx + ca * rx * 0.5f, cy + sa * ry * 0.5f, mid, cx + cb * rx * 0.5f, cy + sb * ry * 0.5f, mid);
        tri(cx + ca * rx * 0.5f, cy + sa * ry * 0.5f, mid, cx + ca * rx, cy + sa * ry, out, cx + cb * rx * 0.5f, cy + sb * ry * 0.5f, mid);
        tri(cx + cb * rx * 0.5f, cy + sb * ry * 0.5f, mid, cx + ca * rx, cy + sa * ry, out, cx + cb * rx, cy + sb * ry, out);
    }
}
static void disc(float cx, float cy, float r, unsigned color) {   /* (its rim smoothed by a fading ring) */
    unsigned out = fade(color, 0);
    for (int i = 0; i < 24; i++) {
        float a = i * 2 * PI_F / 24, b = a + 2 * PI_F / 24;
        tri(cx, cy, color, cx + cosf(a) * r, cy + sinf(a) * r, color, cx + cosf(b) * r, cy + sinf(b) * r, color);
        tri(cx + cosf(a) * r, cy + sinf(a) * r, color, cx + cosf(a) * (r + 1), cy + sinf(a) * (r + 1), out, cx + cosf(b) * r, cy + sinf(b) * r, color);
        tri(cx + cosf(b) * r, cy + sinf(b) * r, color, cx + cosf(a) * (r + 1), cy + sinf(a) * (r + 1), out, cx + cosf(b) * (r + 1), cy + sinf(b) * (r + 1), out);
    }
}
static void line(float x0, float y0, float x1, float y1, float t, unsigned c) {   /* t px thick */
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) return;
    float nx = -dy / l * t / 2, ny = dx / l * t / 2;
    tri(x0 + nx, y0 + ny, c, x1 + nx, y1 + ny, c, x0 - nx, y0 - ny, c);
    tri(x0 - nx, y0 - ny, c, x1 + nx, y1 + ny, c, x1 - nx, y1 - ny, c);
}

/* Text: at whole pixels, size times the font's, coloured top to bottom, with a shadow (shadow's alpha; 0: none) */
static float text_ex(float x, float y, float size, float spacing, unsigned top, unsigned bottom, int shadow, const char *s) {
    x = floorf(x + 0.5f);
    y = floorf(y + 0.5f);
    float cw = CELL_W * size, ch = CELL_H * size;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c > 32 && c < 127 && nverts + 12 <= MAX_QUADS * 6) {
            float u0 = (float)((c - 32) % COLS * CELL_W) / ATLAS_W, v0 = (float)((c - 32) / COLS * CELL_H) / ATLAS_H;
            float u1 = u0 + (float)CELL_W / ATLAS_W, v1 = v0 + (float)CELL_H / ATLAS_H;
            for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
                float o = pass ? 0 : (size > 1.4f ? 2 : 1);
                unsigned t = pass ? top : RGBA(0, 0, 0, shadow), b = pass ? bottom : RGBA(0, 0, 0, shadow);
                OverlayVertex a = {x + o, y + o, u0, v0, t}, bb = {x + o + cw, y + o, u1, v0, t}, cc = {x + o, y + o + ch, u0, v1, b},
                              d = {x + o + cw, y + o + ch, u1, v1, b};
                OverlayVertex *v = &verts[nverts];
                v[0] = a; v[1] = bb; v[2] = cc; v[3] = cc; v[4] = bb; v[5] = d;
                nverts += 6;
            }
        }
        x += cw + spacing;
    }
    return x;
}
static float text_w(const char *s, float size, float spacing) { size_t n = strlen(s); return n ? n * (CELL_W * size + spacing) - spacing : 0; }
static void text_c(float cx, float y, float size, float spacing, unsigned color, const char *s) {   /* centred on cx */
    text_ex(cx - text_w(s, size, spacing) / 2, y, size, spacing, color, color, 170, s);
}
/* s cut to fit max px (its end "..."), into out */
static const char *fit(const char *s, float max, char *out, size_t n) {
    size_t can = max > 0 ? (size_t)(max / CELL_W) : 0, l = strlen(s);
    if (l <= can) return s;
    if (can < 4) can = 4;
    snprintf(out, n, "%.*s...", (int)(can - 3), s);
    return out;
}

/* ---- icons, size px tall ---- */
static void icon_display(float x, float y, float s, unsigned c) {   /* a monitor */
    float w = s * 1.3f, h = s * 0.8f, t = s > 16 ? 2 : 1.5f;
    rbox(x, y, w, h, 2, c, c);
    rbox(x + t, y + t, w - 2 * t, h - 2 * t, 1, RGBA(0, 0, 0, 120), RGBA(0, 0, 0, 120));
    quad4(x + w / 2 - t, y + h, 2 * t, s * 0.12f, c, c, c, c);
    rbox(x + w * 0.28f, y + h + s * 0.12f, w * 0.44f, t + 1, 1, c, c);
}
static void icon_network(float x, float y, float s, unsigned c) {   /* signal bars */
    float bw = s * 0.24f, gap = s * 0.12f;
    for (int i = 0; i < 4; i++) {
        float bh = s * (0.3f + 0.233f * i);
        rbox(x + i * (bw + gap), y + s - bh, bw, bh, 1.5f, c, c);
    }
}
static void arrow(float cx, float cy, float s, int dir, unsigned c) {   /* a chevron-like triangle, dir -1 left, 1 right */
    tri(cx + dir * s * 0.55f, cy, c, cx - dir * s * 0.45f, cy - s * 0.7f, c, cx - dir * s * 0.45f, cy + s * 0.7f, c);
}
static void icon_play(float cx, float cy, float s, unsigned c) { tri(cx + s * 0.6f, cy, c, cx - s * 0.45f, cy - s * 0.7f, c, cx - s * 0.45f, cy + s * 0.7f, c); }
static void icon_warning(float cx, float cy, float s, unsigned c) {
    tri(cx, cy - s * 0.62f, c, cx + s * 0.62f, cy + s * 0.5f, c, cx - s * 0.62f, cy + s * 0.5f, c);
    rbox(cx - 1.5f, cy - s * 0.22f, 3, s * 0.42f, 1.5f, RGBA(30, 20, 0, 255), RGBA(30, 20, 0, 255));
    disc(cx, cy + s * 0.33f, 1.6f, RGBA(30, 20, 0, 255));
}
static void icon_ok(float cx, float cy, float s, unsigned c) {
    disc(cx, cy, s * 0.5f, c);
    unsigned k = RGBA(8, 40, 20, 255);
    line(cx - s * 0.24f, cy + s * 0.02f, cx - s * 0.06f, cy + s * 0.2f, 2.4f, k);
    line(cx - s * 0.07f, cy + s * 0.2f, cx + s * 0.26f, cy - s * 0.18f, 2.4f, k);
}
static void icon_info(float cx, float cy, float s, unsigned c) {
    disc(cx, cy, s * 0.5f, c);
    unsigned k = RGBA(14, 18, 34, 255);
    disc(cx, cy - s * 0.22f, 1.6f, k);
    rbox(cx - 1.4f, cy - s * 0.06f, 2.8f, s * 0.32f, 1.4f, k, k);
}
static void icon_lock(float x, float y, float s, unsigned c) {
    float w = s * 0.75f;
    line(x + w * 0.22f, y + s * 0.48f, x + w * 0.22f, y + s * 0.22f, 2, c);
    line(x + w * 0.78f, y + s * 0.48f, x + w * 0.78f, y + s * 0.22f, 2, c);
    line(x + w * 0.21f, y + s * 0.24f, x + w * 0.5f, y + s * 0.02f, 2, c);
    line(x + w * 0.5f, y + s * 0.02f, x + w * 0.79f, y + s * 0.24f, 2, c);
    rbox(x, y + s * 0.45f, w, s * 0.55f, 2, c, c);
}
/* A key: its cap, a little raised */
static float keycap(float x, float y, const char *key) {
    float w = text_w(key, 1, 0) + 16, h = CELL_H + 4;
    rbox(x, y + 2, w, h, 6, RGBA(70, 74, 96, 255), RGBA(50, 52, 70, 255));
    rbox(x, y, w, h, 6, RGBA(226, 230, 245, 255), RGBA(176, 182, 204, 255));
    text_ex(x + 8, y + 2, 1, 0, RGBA(26, 28, 40, 255), RGBA(26, 28, 40, 255), 0, key);
    return w;
}

/* The footer's hints: '\t' between them; "key=what" a key and what it does, '!' first a warning,
 * '#' first a countdown's fraction left (a bar); else plain text. Its width (draw 0: only measured). */
static float hints(float x, float y, const char *footer, int draw, float *countdown) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", footer);
    float cx = x;
    int first = 1;
    for (char *tok = buf, *next; tok; tok = next) {
        next = strchr(tok, '\t');
        if (next) *next++ = 0;
        if (!*tok) continue;
        if (*tok == '#') { if (countdown) *countdown = (float)atof(tok + 1); continue; }
        if (!first) cx += 26;
        first = 0;
        char *eq = strchr(tok, '=');
        if (*tok == '!') {
            if (draw) icon_warning(cx + 9, y + CELL_H / 2.0f + 2, 16, RGBA(255, 196, 70, 255));
            cx += 24;
            if (draw) text_ex(cx, y + 2, 1, 0, RGBA(255, 214, 120, 255), RGBA(255, 214, 120, 255), 150, tok + 1);
            cx += text_w(tok + 1, 1, 0);
        } else if (eq && eq != tok) {
            *eq = 0;
            if (draw) cx += keycap(cx, y, tok); else cx += text_w(tok, 1, 0) + 16;
            cx += 8;
            if (draw) text_ex(cx, y + 2, 1, 0, RGBA(170, 176, 200, 255), RGBA(150, 156, 180, 255), 150, eq + 1);
            cx += text_w(eq + 1, 1, 0);
        } else {
            if (draw) text_ex(cx, y + 2, 1, 0, RGBA(190, 196, 220, 255), RGBA(160, 166, 190, 255), 150, tok);
            cx += text_w(tok, 1, 0);
        }
    }
    return cx - x;
}

/* The screen behind the menu: a deep gradient, light from two corners, slanted bands and a vignette */
static void backdrop(float w, float h) {
    vgrad(0, 0, w, h, RGBA(20, 22, 44, 255), RGBA(6, 6, 14, 255));
    glow(w * 0.16f, h * 0.10f, w * 0.55f, h * 0.65f, RGBA(96, 70, 255, 70));
    glow(w * 0.88f, h * 0.95f, w * 0.50f, h * 0.60f, RGBA(255, 84, 50, 52));
    glow(w * 0.5f, h * 0.5f, w * 0.45f, h * 0.45f, RGBA(60, 120, 255, 26));
    static const float band[][3] = {{-0.10f, 0.10f, 14}, {0.06f, 0.025f, 10}, {0.62f, 0.16f, 9}, {0.80f, 0.04f, 14}};
    for (unsigned i = 0; i < sizeof band / sizeof band[0]; i++) {   /* (from the bottom, leaning right) */
        float x0 = band[i][0] * w, bw = band[i][1] * w, lean = h * 0.45f;
        unsigned c = RGBA(255, 255, 255, (unsigned)band[i][2]), c0 = fade(c, 0.2f);
        tri(x0, h, c0, x0 + bw, h, c0, x0 + lean, 0, c);
        tri(x0 + lean, 0, c, x0 + bw, h, c0, x0 + bw + lean, 0, c);
    }
    unsigned dark = RGBA(0, 0, 0, 130), none = RGBA(0, 0, 0, 0);
    vgrad(0, 0, w, h * 0.18f, dark, none);
    vgrad(0, h * 0.82f, w, h * 0.18f, none, dark);
    hgrad(0, 0, w * 0.12f, h, dark, none);
    hgrad(w * 0.88f, 0, w * 0.12f, h, none, dark);
}

/* The start-up menu (gpu_menu.c), centred in the w x h back buffer: a logo, the pages' tabs (title:
 * their names, '\t' between them, '*' before the one shown - else a plain title), a row per shown
 * item - its label, and its value: between arrows, or as it is when it starts with a control
 * character ('\1' information, '\2' a text field, '\3' a warning, '\4' good news, '\5' a note) - the
 * selected one lit, the button row (Play) as a button, and the footer's hints (see hints). Each row's
 * place goes to rects (x, y, w, h; 0 for a row not shown), and where the values start to *value_x,
 * for clicks; the tabs are across the rows' width, just above the first one. */
void overlay_build_menu(unsigned w, unsigned h, const char *title, int n, const char *const labels[], const char *const values[],
                        const int shown[], int selected, int button, const char *footer, float *rects, float *value_x) {
    static unsigned frame;
    frame++;
    nverts = 0;
    float W = (float)w, H = (float)h;
    backdrop(W, H);

    int rows = 0;
    for (int i = 0; i < n; i++) rows += shown[i] && i != button;
    /* widths: the label's column, values (the arrows' room too), callouts across the row */
    float label_w = 18.0f * CELL_W, value_w = 26.0f * CELL_W + 72, callout_w = 0;
    for (int i = 0; i < n; i++) {
        if (!shown[i] || i == button) continue;
        unsigned char k = (unsigned char)values[i][0];
        float l = text_w(values[i] + (k < 32), 1, 0);
        if (k >= 3 && k <= 5) { if (l + 72 > callout_w) callout_w = l + 72; }
        else if (k == 1 || k == 2) { if (l + 32 > value_w) value_w = l + 32; }
        else if (l + 72 > value_w) value_w = l + 72;
    }
    float countdown = -1, footer_w = footer ? hints(0, 0, footer, 0, &countdown) : 0;
    float pw = 32 + label_w + value_w + 32;
    if (callout_w + 64 > pw) pw = callout_w + 64;
    if (footer_w + 64 > pw) pw = footer_w + 64;
    if (pw < 720) pw = 720;
    if (pw > W - 32) pw = W - 32;

    /* heights: shrunk to fit a small window */
    int tabs = title && strchr(title, '\t');
    float row_h = 46, logo = 2.0f, tabs_h = 44, play_h = 54, footer_h = 56;
    float logo_h = CELL_H * logo + 32;
    float ph = 22 + (tabs ? tabs_h + 18 : CELL_H + 18) + rows * row_h + 14 + play_h + 18 + footer_h;
    if (logo_h + ph + 24 > H) { row_h = 38; logo = 1.5f; logo_h = CELL_H * logo + 18; play_h = 46; ph -= rows * 8 + 8; }
    if (logo_h + ph + 16 > H) logo_h = 0;
    float px = floorf((W - pw) / 2), py = floorf((H - ph - logo_h) / 2 + logo_h);
    if (py < 8) py = 8;

    /* the logo */
    if (logo_h > 0) {
        const char *a = "WINDOW", *b = "SMASH";
        float sp = logo * 2, tw = text_w(a, logo, sp) + sp * 3 + text_w(b, logo, sp), lx = (W - tw) / 2, ly = py - logo_h + 2;
        glow(W / 2, ly + CELL_H * logo / 2, tw * 0.75f, CELL_H * logo * 1.4f, RGBA(255, 110, 60, 34));
        float x = text_ex(lx, ly, logo, sp, RGBA(255, 255, 255, 255), RGBA(176, 186, 230, 255), 200, a);
        text_ex(x + sp * 2, ly, logo, sp, RGBA(255, 198, 80, 255), RGBA(255, 72, 54, 255), 200, b);
        float uy = ly + CELL_H * logo + 4;
        hgrad(lx, uy, tw / 2, 3, RGBA(255, 120, 60, 0), RGBA(255, 120, 60, 230));
        hgrad(lx + tw / 2, uy, tw / 2, 3, RGBA(255, 120, 60, 230), RGBA(255, 120, 60, 0));
    }

    /* the panel */
    soft(px + 6, py + 12, pw - 12, ph - 6, 28, RGBA(0, 0, 0, 120));
    rbox_border(px, py, pw, ph, 20, RGBA(92, 100, 150, 150), RGBA(34, 37, 60, 238), RGBA(18, 19, 32, 246));
    hgrad(px + 24, py + 1, (pw - 48) / 2, 1, RGBA(160, 170, 255, 0), RGBA(170, 180, 255, 160));   /* (the top edge, lit) */
    hgrad(px + pw / 2, py + 1, (pw - 48) / 2, 1, RGBA(170, 180, 255, 160), RGBA(160, 170, 255, 0));

    float rx = px + 16, rw = pw - 32, y = py + 22;
    /* the tabs */
    if (tabs) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", title);
        char *name[2] = {buf, strchr(buf, '\t') + 1};
        name[1][-1] = 0;
        rbox_border(rx, y, rw, tabs_h, tabs_h / 2, RGBA(70, 76, 112, 200), RGBA(12, 13, 24, 230), RGBA(20, 22, 36, 230));
        for (int t = 0; t < 2; t++) {
            int on = name[t][0] == '*';
            const char *label = name[t] + on;
            char up[32];
            size_t l = 0;
            for (; label[l] && l < sizeof up - 1; l++) up[l] = (char)(label[l] >= 'a' && label[l] <= 'z' ? label[l] - 32 : label[l]);
            up[l] = 0;
            float tx = rx + 4 + t * (rw - 8) / 2, tw = (rw - 8) / 2;
            if (on) {
                soft(tx + 10, y + 10, tw - 20, tabs_h - 16, 14, RGBA(90, 90, 255, 60));
                rbox(tx, y + 4, tw, tabs_h - 8, (tabs_h - 8) / 2, RGBA(120, 132, 255, 255), RGBA(84, 72, 220, 255));
                rbox(tx + 2, y + 5, tw - 4, (tabs_h - 8) / 2 - 2, (tabs_h - 8) / 2 - 2, RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 6));
            }
            unsigned c = on ? RGBA(255, 255, 255, 255) : RGBA(150, 156, 190, 255);
            float lw = text_w(up, 1, 2), iw = 22, cx = tx + (tw - lw - iw - 10) / 2, cy = y + (tabs_h - CELL_H) / 2;
            if (t == 0) icon_display(cx, cy + 3, 15, c); else icon_network(cx + 2, cy + 3, 17, c);
            text_ex(cx + iw + 10, cy, 1, 2, c, on ? RGBA(222, 226, 255, 255) : c, on ? 150 : 0, up);
        }
        y += tabs_h + 18;
    } else {
        text_c(W / 2, y, 1, 0, RGBA(240, 240, 240, 255), title ? title : "");
        y += CELL_H + 18;
    }

    /* the rows */
    float vx = rx + 16 + label_w, vw = rx + rw - 12 - vx;
    if (vw < 160) { vx = rx + rw - 172; vw = 160; }
    int drawn = 0;
    for (int i = 0; i < n; i++) {
        float *r = rects + 4 * i;
        r[0] = r[1] = r[2] = r[3] = 0;
        if (!shown[i] || i == button) continue;
        int sel = i == selected;
        unsigned char kind = (unsigned char)values[i][0];
        const char *v = values[i] + (kind < 32);
        char cut[200], val[200];
        snprintf(val, sizeof val, "%s", v);
        size_t vl = strlen(val);
        int caret = sel && vl && val[vl - 1] == '_';   /* (the model's cursor: drawn as a caret) */
        if (caret) val[--vl] = 0;
        float ry = y, rh = row_h - 6, mid = ry + rh / 2, ty = mid - CELL_H / 2.0f;
        r[0] = rx, r[1] = ry, r[2] = rw, r[3] = rh;
        if (drawn++ && !sel && i - 1 != selected) hgrad(rx + 16, ry - 3, rw - 32, 1, RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 18));

        if (kind >= 3 && kind <= 5) {   /* a callout across the row */
            unsigned edge = kind == 3 ? RGBA(255, 180, 60, 255) : kind == 4 ? RGBA(90, 220, 140, 255) : RGBA(110, 150, 255, 255);
            rbox(rx + 4, ry, rw - 8, rh, 10, fade(edge, 0.16f), fade(edge, 0.07f));
            rbox(rx + 4, ry + 4, 4, rh - 8, 2, edge, fade(edge, 0.6f));
            if (kind == 3) icon_warning(rx + 32, mid, 20, edge);
            else if (kind == 4) icon_ok(rx + 32, mid, 20, edge);
            else icon_info(rx + 32, mid, 20, edge);
            unsigned tc = kind == 3 ? RGBA(255, 226, 170, 255) : kind == 4 ? RGBA(190, 255, 214, 255) : RGBA(206, 216, 255, 255);
            text_ex(rx + 54, ty, 1, 0, tc, mix(tc, RGBA(150, 150, 170, 255), 0.25f), 160, fit(val, rw - 70, cut, sizeof cut));
            y += row_h;
            continue;
        }
        if (sel) {
            soft(rx + 8, ry + 6, rw - 16, rh - 10, 12, RGBA(80, 100, 255, 46));
            rbox(rx, ry, rw, rh, 12, RGBA(70, 84, 170, 150), RGBA(44, 52, 120, 150));
            rbox(rx + 6, ry + 7, 4, rh - 14, 2, RGBA(150, 170, 255, 255), RGBA(110, 100, 255, 255));
        }
        unsigned lc = kind == 1 ? RGBA(140, 148, 180, 255) : sel ? RGBA(255, 255, 255, 255) : RGBA(214, 218, 236, 255);
        text_ex(rx + 22, ty, 1, 0, lc, mix(lc, RGBA(120, 124, 150, 255), 0.3f), 150, labels[i]);
        if (kind == 1) {   /* information: as it is */
            disc(vx + 4, mid, 3, RGBA(110, 130, 200, 255));
            text_ex(vx + 16, ty, 1, 0, RGBA(176, 188, 224, 255), RGBA(146, 156, 196, 255), 150, fit(val, vw - 16, cut, sizeof cut));
        } else if (kind == 2) {   /* a text field */
            float fh = rh - 10;
            rbox_border(vx, mid - fh / 2, vw, fh, 8, sel ? RGBA(130, 150, 255, 255) : RGBA(70, 76, 110, 255), RGBA(8, 9, 16, 240),
                        RGBA(16, 18, 30, 240));
            const char *t = fit(val, vw - 30, cut, sizeof cut);
            float end = text_ex(vx + 12, ty, 1, 0, RGBA(240, 242, 255, 255), RGBA(200, 206, 230, 255), 0, t);
            if (caret && (frame / 30) % 2 == 0) rbox(end + 1, mid - CELL_H / 2.0f + 2, 2, CELL_H - 4, 1, RGBA(150, 170, 255, 255), RGBA(150, 170, 255, 255));
        } else {   /* a choice: a pill between arrows */
            int locked = strstr(val, "(set by ") != NULL;
            float fh = rh - 10, fy = mid - fh / 2;
            unsigned border = sel ? RGBA(140, 160, 255, 255) : RGBA(66, 72, 106, 255);
            rbox_border(vx, fy, vw, fh, fh / 2, border, sel ? RGBA(34, 40, 82, 245) : RGBA(14, 15, 26, 235),
                        sel ? RGBA(24, 26, 56, 245) : RGBA(22, 24, 38, 235));
            const char *t = fit(val, vw - 80, cut, sizeof cut);
            float tw = text_w(t, 1, 0), tx = vx + (vw - tw) / 2;
            unsigned tc = locked ? RGBA(150, 154, 176, 255) : sel ? RGBA(255, 255, 255, 255) : RGBA(200, 208, 232, 255);
            if (locked) icon_lock(vx + 16, mid - 8, 16, RGBA(150, 154, 176, 255));
            else {
                unsigned ac = sel ? RGBA(170, 186, 255, 255) : RGBA(96, 104, 146, 255);
                arrow(vx + 20, mid, 7, -1, ac);
                arrow(vx + vw - 20, mid, 7, 1, ac);
            }
            float end = text_ex(tx, ty, 1, 0, tc, mix(tc, RGBA(140, 146, 180, 255), 0.3f), 150, t);
            if (caret && (frame / 30) % 2 == 0) rbox(end + 1, mid - CELL_H / 2.0f + 2, 2, CELL_H - 4, 1, RGBA(150, 170, 255, 255), RGBA(150, 170, 255, 255));
        }
        y += row_h;
    }

    /* the button */
    if (button >= 0 && button < n && shown[button]) {
        int sel = selected == button;
        y += 14;
        float bw = 300, bh = play_h, bx = floorf((W - bw) / 2);
        if (sel) {
            soft(bx + 16, y + 10, bw - 32, bh - 12, 26, RGBA(60, 230, 120, 70));
            rbox(bx, y, bw, bh, bh / 2, RGBA(120, 240, 150, 255), RGBA(30, 160, 84, 255));
            rbox(bx + 3, y + 2, bw - 6, bh / 2 - 2, bh / 2 - 3, RGBA(255, 255, 255, 70), RGBA(255, 255, 255, 10));
        } else rbox_border(bx, y, bw, bh, bh / 2, RGBA(80, 190, 120, 220), RGBA(32, 46, 48, 240), RGBA(20, 28, 30, 240));
        char up[32];
        size_t l = 0;
        for (; labels[button][l] && l < sizeof up - 1; l++) up[l] = (char)(labels[button][l] >= 'a' && labels[button][l] <= 'z' ? labels[button][l] - 32 : labels[button][l]);
        up[l] = 0;
        float lw = text_w(up, 1.25f, 4), cx = bx + (bw - lw - 30) / 2, cy = y + bh / 2;
        unsigned c = sel ? RGBA(8, 40, 20, 255) : RGBA(170, 240, 196, 255);
        icon_play(cx + 8, cy, 11, c);
        text_ex(cx + 30, cy - CELL_H * 1.25f / 2, 1.25f, 4, c, c, sel ? 0 : 160, up);
        float *r = rects + 4 * button;
        r[0] = bx, r[1] = y, r[2] = bw, r[3] = bh;
        y += bh;
    } else if (button >= 0 && button < n) {
        float *r = rects + 4 * button;
        r[0] = r[1] = r[2] = r[3] = 0;
    }

    /* the footer: a line, the countdown's bar, the hints */
    float fy = py + ph - footer_h;
    hgrad(px + 24, fy, (pw - 48) / 2, 1, RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 30));
    hgrad(px + pw / 2, fy, (pw - 48) / 2, 1, RGBA(255, 255, 255, 30), RGBA(255, 255, 255, 0));
    if (countdown >= 0) {
        float f = countdown > 1 ? 1 : countdown, tw = pw - 96;
        rbox(px + 48, fy + 8, tw, 4, 2, RGBA(255, 255, 255, 26), RGBA(255, 255, 255, 26));
        if (f > 0) rbox(px + 48, fy + 8, tw * f, 4, 2, RGBA(255, 190, 80, 255), RGBA(255, 100, 60, 255));
    }
    if (footer) hints(floorf((W - footer_w) / 2), fy + (footer_h - CELL_H) / 2 + 2, footer, 1, NULL);
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
