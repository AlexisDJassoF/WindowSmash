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
#include "gpu_menu.h"   /* (MENU_JOIN_BUTTON, MENU_ICON_BUTTON: where clicks on the Network page's list land) */

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

/* The menu's colours: near black, dark greys, one green */
#define M_BG_TOP RGBA(13, 13, 16, 255)
#define M_BG_BOTTOM RGBA(7, 7, 9, 255)
#define M_PANEL RGBA(24, 25, 29, 252)
#define M_PANEL_EDGE RGBA(58, 61, 68, 255)
#define M_INNER RGBA(15, 16, 19, 255)
#define M_INNER_EDGE RGBA(34, 36, 41, 255)
#define M_ROW RGBA(27, 28, 32, 255)
#define M_ROW_EDGE RGBA(36, 38, 43, 255)
#define M_TEXT RGBA(226, 229, 234, 255)
#define M_DIM RGBA(150, 154, 162, 255)
#define M_GREEN RGBA(98, 226, 74, 255)
#define M_GREEN_DARK RGBA(18, 38, 18, 255)

/* A key: its cap, outlined */
static float keycap(float x, float y, const char *key) {
    float w = text_w(key, 1, 0) + 18, h = CELL_H + 8;
    rbox_border(x, y - 4, w, h, 7, RGBA(92, 96, 106, 255), RGBA(42, 44, 50, 255), RGBA(30, 32, 37, 255));
    text_ex(x + 9, y, 1, 0, M_TEXT, M_TEXT, 0, key);
    return w;
}

#define PAD_BUTTON (CELL_H + 8.0f)   /* a pad's face button in the hints: its width and height */
static void pad_button(float x, float mid, char kind, char where);

/* The footer's hints: '\t' between them; "key=what" a key and what it does (a key "^" kind where: a
 * pad's face button drawn, see pad_button), '!' first a warning,
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
        if (!first) cx += 48;
        first = 0;
        char *eq = strchr(tok, '=');
        if (*tok == '!') {
            if (draw) icon_warning(cx + 9, y + CELL_H / 2.0f, 16, RGBA(255, 196, 70, 255));
            cx += 24;
            if (draw) text_ex(cx, y, 1, 0, RGBA(255, 214, 120, 255), RGBA(255, 214, 120, 255), 0, tok + 1);
            cx += text_w(tok + 1, 1, 0);
        } else if (eq && eq != tok) {
            *eq = 0;
            if (tok[0] == '^' && tok[1] && tok[2]) {   /* (a pad's button) */
                if (draw) pad_button(cx, y + CELL_H / 2.0f, tok[1], tok[2]);
                cx += PAD_BUTTON;
            } else if (draw) cx += keycap(cx, y, tok);
            else cx += text_w(tok, 1, 0) + 18;
            cx += 6;
            if (draw) text_ex(cx, y, 1, 0, M_DIM, M_DIM, 0, ":");
            cx += CELL_W + 12;
            if (draw) text_ex(cx, y, 1, 0, RGBA(196, 200, 208, 255), RGBA(196, 200, 208, 255), 0, eq + 1);
            cx += text_w(eq + 1, 1, 0);
        } else {
            if (draw) text_ex(cx, y, 1, 0, RGBA(196, 200, 208, 255), RGBA(196, 200, 208, 255), 0, tok);
            cx += text_w(tok, 1, 0);
        }
    }
    return cx - x;
}

static int over_game;   /* overlay_menu_over: the menu drawn over the game's picture */
void overlay_menu_over(int on) { over_game = on; }

/* The screen behind the menu: plain, near black, its edges a little darker - or over the game, the
 * game's picture dimmed */
static void backdrop(float w, float h) {
    if (over_game) { quad4(0, 0, w, h, RGBA(0, 0, 0, 175), RGBA(0, 0, 0, 175), RGBA(0, 0, 0, 205), RGBA(0, 0, 0, 205)); return; }
    vgrad(0, 0, w, h, M_BG_TOP, M_BG_BOTTOM);
    unsigned dark = RGBA(0, 0, 0, 90), none = RGBA(0, 0, 0, 0);
    hgrad(0, 0, w * 0.15f, h, dark, none);
    hgrad(w * 0.85f, 0, w * 0.15f, h, none, dark);
}

/* A box with a border t px thick: border, then fill */
static void rbox_edge(float x, float y, float w, float h, float r, float t, unsigned border, unsigned fill) {
    rbox(x, y, w, h, r, border, border);
    rbox(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 0 ? r - t : 0, fill, fill);
}

/* ---- the Network page's list of rooms ---- */
static void ring(float cx, float cy, float rx, float ry, float t, unsigned c) {   /* an ellipse's outline, t px thick */
    for (int i = 0; i < 32; i++) {
        float a = i * 2 * PI_F / 32, b = a + 2 * PI_F / 32;
        line(cx + cosf(a) * rx, cy + sinf(a) * ry, cx + cosf(b) * rx, cy + sinf(b) * ry, t, c);
    }
}
/* A pad's face button, PAD_BUTTON across, its left end at x and its middle at mid: kind X (Xbox and
 * most pads: A B X Y on coloured buttons), P (PlayStation: cross, circle, square, triangle) or N
 * (Nintendo: B A Y X, by position); where S, E, W or N, the button's place in the diamond */
static void pad_button(float x, float mid, char kind, char where) {
    float r = PAD_BUTTON / 2, cx = x + r;
    int at = where == 'S' ? 0 : where == 'E' ? 1 : where == 'W' ? 2 : 3;
    soft(cx - r * 0.5f, mid - r * 0.4f, r, r, 6, RGBA(0, 0, 0, 70));
    if (kind == 'P') {
        static const unsigned SYMBOL[4] = {RGBA(128, 176, 240, 255), RGBA(240, 112, 112, 255), RGBA(228, 136, 208, 255), RGBA(96, 212, 176, 255)};
        unsigned c = SYMBOL[at];
        disc(cx, mid, r, RGBA(118, 122, 132, 255));
        disc(cx, mid, r - 2, RGBA(36, 38, 44, 255));
        float s = r * 0.42f;
        if (at == 0) { line(cx - s, mid - s, cx + s, mid + s, 3, c); line(cx - s, mid + s, cx + s, mid - s, 3, c); }
        else if (at == 1) { disc(cx, mid, s * 1.1f + 1.3f, c); disc(cx, mid, s * 1.1f - 1.3f, RGBA(36, 38, 44, 255)); }
        else if (at == 2) {
            line(cx - s, mid - s, cx + s, mid - s, 2.6f, c); line(cx + s, mid - s, cx + s, mid + s, 2.6f, c);
            line(cx + s, mid + s, cx - s, mid + s, 2.6f, c); line(cx - s, mid + s, cx - s, mid - s, 2.6f, c);
        } else {
            float t = s * 1.15f;
            line(cx, mid - t, cx + t, mid + t * 0.75f, 2.6f, c); line(cx + t, mid + t * 0.75f, cx - t, mid + t * 0.75f, 2.6f, c);
            line(cx - t, mid + t * 0.75f, cx, mid - t, 2.6f, c);
        }
        return;
    }
    static const char XBOX[4] = {'A', 'B', 'X', 'Y'}, NINTENDO[4] = {'B', 'A', 'Y', 'X'};
    char letter[2] = {kind == 'N' ? NINTENDO[at] : XBOX[at], 0};
    if (kind == 'N') {   /* (dark buttons, white letters) */
        disc(cx, mid, r, RGBA(150, 154, 164, 255));
        disc(cx, mid, r - 2, RGBA(44, 46, 52, 255));
        text_ex(cx - CELL_W / 2.0f, mid - CELL_H / 2.0f, 1, 0, RGBA(246, 247, 250, 255), RGBA(246, 247, 250, 255), 0, letter);
        return;
    }
    static const unsigned FILL[4] = {RGBA(84, 178, 64, 255), RGBA(222, 64, 56, 255), RGBA(48, 120, 226, 255), RGBA(236, 182, 28, 255)};
    disc(cx, mid, r, FILL[at]);
    disc(cx, mid - 1, r - 3, mix(FILL[at], RGBA(255, 255, 255, 255), 0.12f));
    text_ex(cx - CELL_W / 2.0f, mid - CELL_H / 2.0f, 1, 0, RGBA(255, 255, 255, 255), RGBA(240, 240, 240, 255), 110, letter);
}

static void icon_globe(float cx, float cy, float r, unsigned c) {
    ring(cx, cy, r, r, 2.2f, c);
    ring(cx, cy, r * 0.42f, r, 2, c);
    line(cx, cy - r, cx, cy + r, 2, c);
    line(cx - r, cy, cx + r, cy, 2, c);
    float y = r * 0.5f, x = sqrtf(r * r - y * y);
    line(cx - x, cy - y, cx + x, cy - y, 1.6f, c);
    line(cx - x, cy + y, cx + x, cy + y, 1.6f, c);
}
/* Two arcs chasing each other, an arrowhead on each */
static void icon_refresh(float cx, float cy, float r, unsigned c) {
    for (int half = 0; half < 2; half++) {
        float a0 = half ? 0.15f * PI_F : 1.15f * PI_F, a1 = a0 + 0.72f * PI_F;
        for (int i = 0; i < 10; i++) {
            float a = a0 + (a1 - a0) * i / 10, b = a0 + (a1 - a0) * (i + 1) / 10;
            line(cx + cosf(a) * r, cy + sinf(a) * r, cx + cosf(b) * r, cy + sinf(b) * r, 2.4f, c);
        }
        float ex = cx + cosf(a1) * r, ey = cy + sinf(a1) * r, tx = -sinf(a1), ty = cosf(a1), nx = cosf(a1), ny = sinf(a1), s = r * 0.5f;
        tri(ex + tx * s, ey + ty * s, c, ex + nx * s * 0.75f, ey + ny * s * 0.75f, c, ex - nx * s * 0.75f, ey - ny * s * 0.75f, c);
    }
}
static void person(float cx, float top, float s, unsigned c) {   /* a head and shoulders, s px tall */
    disc(cx, top + s * 0.24f, s * 0.22f, c);
    float by = top + s, r = s * 0.42f;
    for (int i = 0; i < 12; i++) {
        float a = PI_F + i * PI_F / 12, b = a + PI_F / 12;
        tri(cx, by, c, cx + cosf(a) * r, by + sinf(a) * r * 1.05f, c, cx + cosf(b) * r, by + sinf(b) * r * 1.05f, c);
    }
}
static void icon_people(float x, float y, float s, unsigned c) {
    person(x + s * 0.72f, y, s * 0.85f, fade(c, 0.7f));
    person(x + s * 0.38f, y + s * 0.12f, s * 0.88f, c);
}
static void signal_bars(float x, float bottom, float h, int bars, unsigned on, unsigned off) {
    for (int i = 0; i < 4; i++) {
        float bh = h * (0.4f + 0.2f * i);
        rbox(x + i * 8, bottom - bh, 5, bh, 1.5f, i < bars ? on : off, i < bars ? on : off);
    }
}
static unsigned signal_color(char s) { return s == 'g' ? M_GREEN : s == 'y' ? RGBA(244, 186, 44, 255) : RGBA(112, 116, 124, 255); }

/* A square button with an icon (the heading's Refresh) */
static void icon_button(float x, float y, float s) {
    rbox_edge(x, y, s, s, 7, 1, RGBA(84, 88, 98, 255), RGBA(32, 34, 39, 255));
    icon_refresh(x + s / 2, y + s / 2, s * 0.26f, M_TEXT);
}

/* The heading: a globe, what the list is (label) and what it holds (sub), the Refresh button */
static void list_heading(float x, float y, float w, float h, const char *label, const char *sub) {
    char cut[200];
    rbox_edge(x, y, w, h, 5, 1, M_ROW_EDGE, RGBA(22, 23, 27, 255));
    rbox(x + 1, y + 1, h - 2, h - 2, 4, RGBA(30, 32, 37, 255), RGBA(30, 32, 37, 255));
    icon_globe(x + h / 2, y + h / 2, h * 0.28f, M_TEXT);
    float tx = x + h + 18, small = h < 60 ? 0.75f : 0.8f;
    char title[64];
    snprintf(title, sizeof title, "%s", label);
    int up = 1;
    for (char *p = title; *p; p++) { if (up && *p >= 'a' && *p <= 'z') *p -= 32; up = *p == ' '; }   /* (Title Case) */
    text_ex(tx, y + h / 2 - CELL_H - 1, 1, 0.5f, RGBA(240, 242, 246, 255), RGBA(240, 242, 246, 255), 0, title);
    float room = w - (tx - x) - MENU_ICON_BUTTON - 8;
    text_ex(tx, y + h / 2 + 3, small, 0, M_DIM, M_DIM, 0, fit(sub, room / small, cut, sizeof cut));
    float s = h - 16 < 48 ? h - 16 : 48;
    icon_button(x + w - 8 - s, y + (h - s) / 2, s);
}

/* A room (net_menu_value's '\7'): its signal, its name and the line under it, its players and Join */
static void list_room(float x, float y, float w, float h, int sel, const char *v) {
    char buf[200], cut[200];
    snprintf(buf, sizeof buf, "%s", v);
    char signal = buf[0], bars = buf[1], chosen = buf[2];
    char *field[5] = {0};
    char *p = strchr(buf, '\t');
    for (int i = 0; i < 5 && p; i++) {
        *p++ = 0;
        field[i] = p;
        p = strchr(p, '\t');
    }
    if (!field[4]) return;
    const char *name = field[0], *state = field[1], *players = field[3], *action = field[4];
    char *rest = field[2];
    int can = signal != 'o' && strcmp(action, "Full");
    unsigned sc = signal_color(signal);
    float mid = y + h / 2;

    if (sel) rbox_edge(x - 1, y - 1, w + 2, h + 2, 6, 3, M_GREEN, M_GREEN_DARK);
    else rbox_edge(x, y, w, h, 5, 1, M_ROW_EDGE, M_ROW);
    signal_bars(x + 22, mid + 13, 26, bars - '0', sc, signal == 'o' ? RGBA(64, 67, 74, 255) : fade(sc, 0.25f));

    /* the button, then the players, from the right */
    float bw = 140, bh = h - 18 < 44 ? h - 18 : 44, bx = x + w - 16 - bw, by = mid - bh / 2;
    if (can && sel) {
        rbox(bx, by, bw, bh, 6, RGBA(120, 236, 96, 255), RGBA(120, 236, 96, 255));
        rbox(bx + 2, by + 2, bw - 4, bh - 4, 5, RGBA(64, 176, 48, 255), RGBA(40, 140, 34, 255));
    } else if (can) rbox_edge(bx, by, bw, bh, 6, 2, RGBA(96, 100, 110, 255), RGBA(26, 27, 31, 255));
    else rbox_edge(bx, by, bw, bh, 6, 2, RGBA(50, 53, 60, 255), RGBA(22, 23, 27, 255));
    unsigned bc = can ? RGBA(244, 246, 250, 255) : RGBA(96, 100, 108, 255);
    text_ex(bx + (bw - text_w(action, 1, 1)) / 2, mid - CELL_H / 2.0f, 1, 1, bc, bc, can && sel ? 120 : 0, action);
    float px = bx - 24;
    if (*players) {
        px -= text_w(players, 1, 0);
        text_ex(px, mid - CELL_H / 2.0f, 1, 0, M_TEXT, M_TEXT, 0, players);
        px -= 8;
    }
    px -= 30;
    icon_people(px, mid - 12, 24, RGBA(196, 200, 208, 255));

    /* the name (a check when it is the address chosen), and under it: the state, then the rest */
    float tx = x + 72, room = px - 16 - tx, small = h < 60 ? 0.75f : 0.8f;
    unsigned nc = signal == 'o' ? RGBA(176, 180, 188, 255) : sel ? RGBA(255, 255, 255, 255) : RGBA(236, 238, 242, 255);
    float end = text_ex(tx, mid - CELL_H - 1, 1, 0.5f, nc, nc, 0, fit(name, room - 30, cut, sizeof cut));
    if (chosen == '*') icon_ok(end + 16, mid - CELL_H / 2.0f - 1, 18, M_GREEN);
    float sy = mid + 3, cx = tx;
    disc(cx + 5, sy + CELL_H * small / 2, 4.5f, sc);
    cx = text_ex(cx + 16, sy, small, 0, signal == 'o' ? M_DIM : sc, signal == 'o' ? M_DIM : sc, 0, state);
    for (char *part = rest, *next; part && *part; part = next) {
        next = strchr(part, '|');
        if (next) *next++ = 0;
        float pw = text_w(part, small, 0);
        if (cx + 20 + pw > tx + room) break;
        disc(cx + 9, sy + CELL_H * small / 2, 2, M_DIM);
        cx = text_ex(cx + 18, sy, small, 0, M_DIM, M_DIM, 0, part);
    }
}

/* The pages ('\b': this one, '\t', how many): Refresh on the left, < page / pages > in the values' column */
static void list_pages(float x, float y, float w, float h, float vx, float vw, int sel, const char *v) {
    (void)w;
    int page = atoi(v), pages = strchr(v, '\t') ? atoi(strchr(v, '\t') + 1) : 1;
    float bw = vx - x - 24, mid = y + h / 2;
    if (sel) rbox_edge(x - 1, y - 1, bw + 2, h + 2, 7, 3, M_GREEN, M_GREEN_DARK);
    else rbox_edge(x, y, bw, h, 6, 2, RGBA(84, 88, 98, 255), RGBA(28, 30, 34, 255));
    float lw = text_w("Refresh", 1, 0.5f), ix = x + (bw - lw - 34) / 2;
    icon_refresh(ix + 11, mid, 10, M_TEXT);
    text_ex(ix + 34, mid - CELL_H / 2.0f, 1, 0.5f, M_TEXT, M_TEXT, 0, "Refresh");
    char label[24];
    snprintf(label, sizeof label, "%d / %d", page, pages);
    float cx = vx + vw / 2, s = h - 6 < 40 ? h - 6 : 40, tw = text_w(label, 1, 1);
    unsigned ac = pages > 1 ? (sel ? RGBA(255, 255, 255, 255) : M_TEXT) : RGBA(84, 88, 96, 255);
    for (int side = 0; side < 2; side++) {
        float bx = side ? cx + tw / 2 + 24 : cx - tw / 2 - 24 - s;
        rbox_edge(bx, mid - s / 2, s, s, 6, 1, pages > 1 ? RGBA(84, 88, 98, 255) : RGBA(50, 53, 60, 255), RGBA(28, 30, 34, 255));
        text_ex(bx + (s - CELL_W) / 2, mid - CELL_H / 2.0f, 1, 0, ac, ac, 0, side ? ">" : "<");
    }
    text_ex(cx - tw / 2, mid - CELL_H / 2.0f, 1, 1, M_TEXT, M_TEXT, 0, label);
}

/* A pad's shoulder button by the tabs, as a cap: its left end at x, its middle at mid */
static void shoulder_cap(float x, float mid, const char *label) {
    float w = text_w(label, 1, 0.5f) + 26, h = CELL_H + 8;
    rbox_edge(x, mid - h / 2, w, h, 8, 2, RGBA(150, 154, 164, 255), RGBA(34, 36, 41, 255));
    text_ex(x + 13, mid - CELL_H / 2.0f, 1, 0.5f, RGBA(244, 246, 250, 255), RGBA(244, 246, 250, 255), 0, label);
}

/* The start-up menu (gpu_menu.c), centred in the w x h back buffer: the pages' tabs (title: their
 * names, '\t' between them, '*' before the one shown, then optionally '\t' and the key that changes
 * pages, "Tab" when none, or a pad's two shoulder buttons "L|R" - else a plain title), a row per shown item -
 * its label, and its value: between arrows, or as it is when it starts with a control character
 * ('\1' information, '\2' a text field, '\3' a warning, '\4' good news, '\5' a note) - the selected
 * one outlined, the button row (Play) as a button, and the footer's hints (see hints). Each row's
 * place goes to rects (x, y, w, h; 0 for a row not shown), and where the values start to *value_x,
 * for clicks; the tabs are just above the first row (Display left of the middle, Network right). */
void overlay_build_menu(unsigned w, unsigned h, const char *title, int n, const char *const labels[], const char *const values[],
                        const int shown[], int selected, int button, const char *footer, float *rects, float *value_x) {
    static unsigned frame;
    frame++;
    nverts = 0;
    float W = (float)w, H = (float)h;
    backdrop(W, H);

    /* rows: the plain ones, and the taller ones of the Network page's list (its heading, rooms) */
    int rows = 0, tall = 0, list = 0;
    for (int i = 0; i < n; i++) {
        if (!shown[i] || i == button) continue;
        unsigned char k = (unsigned char)values[i][0];
        rows++;
        tall += k == 6 || k == 7;
        list |= k >= 6 && k <= 8;
    }
    /* widths: the label's column, values (the arrows' room too), callouts across the row */
    float label_w = 18.0f * CELL_W, value_w = 24.0f * CELL_W + 100, callout_w = 0;
    for (int i = 0; i < n; i++) {
        if (!shown[i] || i == button) continue;
        unsigned char k = (unsigned char)values[i][0];
        if (k >= 6 && k <= 8) continue;
        float l = text_w(values[i] + (k < 32), 1, 0);
        if (k >= 3 && k <= 5) { if (l + 72 > callout_w) callout_w = l + 72; }
        else if (k == 1 || k == 2) { if (l + 32 > value_w) value_w = l + 32; }
        else if (l + 100 > value_w) value_w = l + 100;
    }
    float countdown = -1, footer_w = footer ? hints(0, 0, footer, 0, &countdown) : 0;
    float pw = 16 + 16 + label_w + value_w + 16;
    if (callout_w + 48 > pw) pw = callout_w + 48;
    if (footer_w + 64 > pw) pw = footer_w + 64;
    if (pw < (list ? 880 : 720)) pw = list ? 880 : 720;
    if (pw > W - 32) pw = W - 32;

    /* heights: shrunk to fit a small window */
    int tabs = title && strchr(title, '\t');
    float row_h = 46, gap = 4, tabs_h = 46, play_h = 52, footer_h = 58, more = 18;   /* (more: a tall row's) */
    float ph = 8 + tabs_h + 10 + rows * (row_h + gap) + tall * more + 14 + play_h + 16 + footer_h;
    if (ph + 16 > H) {
        row_h = 36; play_h = 44; footer_h = 48; more = 14;
        ph = 8 + tabs_h + 10 + rows * (row_h + gap) + tall * more + 14 + play_h + 16 + footer_h;
    }
    float px = floorf((W - pw) / 2), py = floorf((H - ph) / 2);
    if (py < 8) py = 8;

    /* the panel, and the darker box the rows and the button are in */
    soft(px + 8, py + 10, pw - 16, ph - 8, 24, RGBA(0, 0, 0, 110));
    rbox_edge(px, py, pw, ph, 8, 2, M_PANEL_EDGE, M_PANEL);
    float ix = px + 8, iw = pw - 16, iy = py + 8 + tabs_h, ih = ph - footer_h - 8 - tabs_h - 4;
    rbox_edge(ix, iy, iw, ih, 6, 1, M_INNER_EDGE, M_INNER);

    /* the tabs: side by side in the middle, just above the box */
    float y = py + 8;
    if (tabs) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", title);
        char *name[2] = {buf, strchr(buf, '\t') + 1};
        name[1][-1] = 0;
        /* a third field: the key that changes pages - "Tab" after the other page's name, or a pad's
         * shoulder buttons ("LB|RB"), each in a cap beside the tabs */
        char *key = strchr(name[1], '\t'), *right = NULL;
        if (key) *key++ = 0;
        if (!key || !*key) key = "Tab";
        if ((right = strchr(key, '|'))) *right++ = 0;
        char shown_name[2][48];
        float tw = 0;
        for (int t = 0; t < 2; t++) {
            int on = name[t][0] == '*';
            if (on) snprintf(shown_name[t], sizeof shown_name[t], "[ %s ]", name[t] + 1);
            else if (right) snprintf(shown_name[t], sizeof shown_name[t], "%s", name[t]);
            else snprintf(shown_name[t], sizeof shown_name[t], "%s (%s)", name[t], key);
            float l = text_w(shown_name[t], 1, 1) + 56;
            if (l > tw) tw = l;
        }
        float tx0 = floorf(W / 2 - tw), th = tabs_h - 6;
        rbox(tx0 - 4, y, 2 * tw + 8, th + 4, 8, M_INNER, M_INNER);
        for (int t = 0; t < 2; t++) {
            int on = name[t][0] == '*';
            float tx = tx0 + t * tw;
            if (on) {
                rbox(tx, y + 2, tw, th, 6, RGBA(30, 32, 37, 255), RGBA(26, 28, 32, 255));
                rbox(tx + 14, y + th - 1, tw - 28, 3, 1.5f, M_GREEN, M_GREEN);
            }
            unsigned c = on ? M_GREEN : M_DIM;
            text_ex(tx + (tw - text_w(shown_name[t], 1, 1)) / 2, y + 2 + (th - CELL_H) / 2 - 1, 1, 1, c, c, 0, shown_name[t]);
        }
        if (right) {   /* the shoulder buttons' caps: the left one before the tabs, the right one after */
            float mid = y + (th + 4) / 2;
            shoulder_cap(tx0 - 4 - 16 - (text_w(key, 1, 0.5f) + 26), mid, key);
            shoulder_cap(tx0 + 2 * tw + 4 + 16, mid, right);
        }
    } else text_c(W / 2, y + (tabs_h - CELL_H) / 2, 1, 0, M_TEXT, title ? title : "");
    y = iy + 10;

    /* the rows */
    float rx = ix + 10, rw = iw - 20;
    float vx = rx + 16 + label_w, vw = rx + rw - 12 - vx;
    if (vw < 160) { vx = rx + rw - 172; vw = 160; }
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
        float ry = y, rh = row_h + (kind == 6 || kind == 7 ? more : 0), mid = ry + rh / 2, ty = mid - CELL_H / 2.0f;
        r[0] = rx, r[1] = ry, r[2] = rw, r[3] = rh;
        if (kind >= 6 && kind <= 8) {   /* the Network page's list */
            if (kind == 6) list_heading(rx, ry, rw, rh, labels[i], val);
            else if (kind == 7) list_room(rx, ry, rw, rh, sel, val);
            else list_pages(rx, ry, rw, rh, vx, vw, sel, val);
            y += rh + gap;
            continue;
        }

        if (kind >= 3 && kind <= 5) {   /* a callout across the row */
            unsigned edge = kind == 3 ? RGBA(255, 180, 60, 255) : kind == 4 ? M_GREEN : RGBA(120, 160, 255, 255);
            rbox_edge(rx, ry, rw, rh, 4, 1, fade(edge, 0.35f), mix(M_ROW, edge, 0.08f));
            rbox(rx + 4, ry + 5, 3, rh - 10, 1.5f, edge, edge);
            if (kind == 3) icon_warning(rx + 28, mid, 20, edge);
            else if (kind == 4) icon_ok(rx + 28, mid, 20, edge);
            else icon_info(rx + 28, mid, 20, edge);
            unsigned tc = kind == 3 ? RGBA(255, 226, 170, 255) : kind == 4 ? RGBA(200, 250, 190, 255) : RGBA(206, 216, 255, 255);
            text_ex(rx + 50, ty, 1, 0, tc, tc, 0, fit(val, rw - 66, cut, sizeof cut));
            y += rh + gap;
            continue;
        }
        if (sel) rbox_edge(rx - 1, ry - 1, rw + 2, rh + 2, 5, 3, M_GREEN, M_GREEN_DARK);
        else rbox_edge(rx, ry, rw, rh, 4, 1, M_ROW_EDGE, M_ROW);
        unsigned lc = kind == 1 ? M_DIM : sel ? RGBA(255, 255, 255, 255) : M_TEXT;
        text_ex(rx + 18, ty, 1, 0, lc, lc, 0, labels[i]);
        if (kind == 1) {   /* information: as it is */
            text_ex(vx + 8, ty, 1, 0, RGBA(176, 182, 192, 255), RGBA(176, 182, 192, 255), 0, fit(val, vw - 16, cut, sizeof cut));
        } else if (kind == 2) {   /* a text field */
            float fh = rh - 10;
            rbox_edge(vx, mid - fh / 2, vw, fh, 4, 1, sel ? M_GREEN : RGBA(64, 68, 76, 255), RGBA(10, 11, 13, 255));
            const char *t = fit(val, vw - 30, cut, sizeof cut);
            float end = text_ex(vx + 12, ty, 1, 0, M_TEXT, M_TEXT, 0, t);
            if (caret && (frame / 30) % 2 == 0) rbox(end + 1, mid - CELL_H / 2.0f + 2, 2, CELL_H - 4, 1, M_GREEN, M_GREEN);
        } else {   /* a choice: between arrows */
            int locked = strstr(val, "(set by ") != NULL;
            const char *t = fit(val, vw - 96, cut, sizeof cut);
            unsigned tc = locked ? M_DIM : sel ? RGBA(255, 255, 255, 255) : M_TEXT;
            if (locked) icon_lock(vx + 4, mid - 8, 16, M_DIM);
            else {
                unsigned ac = sel ? RGBA(255, 255, 255, 255) : M_TEXT;
                text_ex(vx + 4, ty, 1, 0, ac, ac, 0, "<");
                text_ex(vx + vw - 4 - CELL_W, ty, 1, 0, ac, ac, 0, ">");
            }
            float end = text_ex(vx + 64, ty, 1, 0, tc, tc, 0, t);
            if (caret && (frame / 30) % 2 == 0) rbox(end + 1, mid - CELL_H / 2.0f + 2, 2, CELL_H - 4, 1, M_GREEN, M_GREEN);
        }
        y += row_h + gap;
    }

    /* the button */
    if (button >= 0 && button < n && shown[button]) {
        int sel = selected == button;
        y += 10;
        float bw = 250, bh = play_h, bx = floorf((W - bw) / 2);
        if (sel) {
            soft(bx + 10, y + 8, bw - 20, bh - 12, 16, RGBA(70, 220, 60, 50));
            rbox(bx, y, bw, bh, 6, RGBA(120, 236, 96, 255), RGBA(120, 236, 96, 255));
            rbox(bx + 2, y + 2, bw - 4, bh - 4, 5, RGBA(64, 176, 48, 255), RGBA(40, 140, 34, 255));
            rbox(bx + 2, y + bh - 5, bw - 4, 3, 1.5f, RGBA(24, 90, 22, 255), RGBA(24, 90, 22, 255));
        } else rbox_edge(bx, y, bw, bh, 6, 2, RGBA(54, 120, 46, 255), RGBA(22, 48, 22, 255));
        const char *label = labels[button];
        float ls = 1.25f, lw = text_w(label, ls, 1);
        unsigned c = sel ? RGBA(244, 255, 240, 255) : RGBA(150, 220, 140, 255);
        text_ex(bx + (bw - lw) / 2, y + (bh - CELL_H * ls) / 2 - 1, ls, 1, c, c, sel ? 140 : 0, label);
        float *r = rects + 4 * button;
        r[0] = bx, r[1] = y, r[2] = bw, r[3] = bh;
        y += bh;
    } else if (button >= 0 && button < n) {
        float *r = rects + 4 * button;
        r[0] = r[1] = r[2] = r[3] = 0;
    }

    /* the footer: the countdown's bar, the hints */
    float fy = py + ph - footer_h;
    if (countdown >= 0) {
        float f = countdown > 1 ? 1 : countdown, tw = pw - 96;
        rbox(px + 48, fy + 4, tw, 3, 1.5f, RGBA(255, 255, 255, 22), RGBA(255, 255, 255, 22));
        if (f > 0) rbox(px + 48, fy + 4, tw * f, 3, 1.5f, M_GREEN, M_GREEN);
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
