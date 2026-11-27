/* The debug overlay: frame rate, a graph of recent frame times and the renderer's statistics,
 * drawn in the corner of each presented frame - with Direct3D 11.
 *
 * The quads themselves and the glyph atlas are gpu_overlay_quads.c's; this draws them from one
 * dynamic vertex buffer, the atlas a one-channel texture, and the menu's pictures
 * (overlay_texture_add) each a texture of its own, made the first time it is drawn, batch by batch. */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include "gpu_overlay.h"
#include "gpu_overlay_quads.h"

#define MAX_QUADS 16384   /* (gpu_overlay_quads.c's) */

static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vbuf, *cbuf;
static ID3D11ShaderResourceView *atlas;
static ID3D11SamplerState *sampler;
static ID3D11BlendState *blend;
static ID3D11RasterizerState *raster;
static int state;   /* 0 not yet, 1 ready, -1 failed */
#define MAX_PICTURES 256
static struct { int id; ID3D11ShaderResourceView *srv; } pics[MAX_PICTURES];

static const char HLSL[] =
    "cbuffer C : register(b0) { float2 inv_size; float mode; };\n"
    "struct I { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "O vs(I i) { O o; o.p = float4(i.p * inv_size * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = i.uv; o.c = i.c; return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(O i) : SV_Target {\n"
    "  float4 x = t.Sample(s, i.uv);\n"
    "  if (mode < 0.5) return float4(i.c.rgb, i.c.a * x.r);\n"   /* the glyphs: coverage */
    "  if (mode > 1.5) x.rgb = dot(x.rgb, float3(0.299, 0.587, 0.114));\n"   /* a picture in greys */
    "  return x * i.c;\n"
    "}\n";

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(HLSL, sizeof HLSL - 1, "overlay", NULL, NULL, entry, target, 0, 0, &code, &errors))) {
        if (errors) { fprintf(stderr, "[overlay] %s\n", (const char *)ID3D10Blob_GetBufferPointer(errors)); ID3D10Blob_Release(errors); }
        return NULL;
    }
    return code;
}

static int make_atlas(ID3D11Device *dev) {
    static unsigned char coverage[OVERLAY_ATLAS_W * OVERLAY_ATLAS_H];
    if (!overlay_atlas(coverage)) return 0;
    D3D11_TEXTURE2D_DESC td = {OVERLAY_ATLAS_W, OVERLAY_ATLAS_H, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                               D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {coverage, OVERLAY_ATLAS_W, 0};
    ID3D11Texture2D *tex;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex))) return 0;
    HRESULT hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &atlas);
    ID3D11Texture2D_Release(tex);
    return SUCCEEDED(hr);
}

static int init(ID3D11Device *dev) {
    ID3DBlob *v = compile("vs", "vs_5_0"), *p = compile("ps", "ps_5_0");
    if (!v || !p) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, el, 3, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), &layout);
    ID3D10Blob_Release(v);
    ID3D10Blob_Release(p);
    D3D11_BUFFER_DESC vb = {MAX_QUADS * 6 * sizeof(OverlayVertex), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    D3D11_BUFFER_DESC cb = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &vb, NULL, &vbuf);
    ID3D11Device_CreateBuffer(dev, &cb, NULL, &cbuf);
    D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                             D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, 0};
    ID3D11Device_CreateSamplerState(dev, &sd, &sampler);
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                                                          D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 15};
    ID3D11Device_CreateBlendState(dev, &bd, &blend);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11Device_CreateRasterizerState(dev, &rd, &raster);
    return vs && ps && layout && vbuf && cbuf && sampler && blend && raster && make_atlas(dev);
}

/* The picture id as a texture: made from its data the first time; NULL when gone (or no room) */
static ID3D11ShaderResourceView *picture(ID3D11Device *dev, int id) {
    int slot = -1;
    for (int i = 0; i < MAX_PICTURES; i++) {
        if (pics[i].id == id) return pics[i].srv;
        if (!pics[i].id && slot < 0) slot = i;
    }
    int format;
    unsigned w, h;
    void *data;
    size_t size;
    if (slot < 0 || !overlay_texture_take(id, &format, &w, &h, &data, &size) || !data) return NULL;
    static const DXGI_FORMAT F[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC7_UNORM};
    int bc = format != OVERLAY_TEX_RGBA8;
    UINT pitch = bc ? (w + 3) / 4 * (format == OVERLAY_TEX_BC1 ? 8 : 16) : w * 4;
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, F[format >= 0 && format < 4 ? format : 0], {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex))) {
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv);
        ID3D11Texture2D_Release(tex);
    }
    free(data);
    if (srv) pics[slot].id = id, pics[slot].srv = srv;
    return srv;
}

/* The quads built last, over rtv at (x0, y0, w x h). Leaves the pipeline state changed. */
static void flush(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w, unsigned h) {
    for (int id; (id = overlay_texture_dead());)   /* (the pictures freed: in-flight draws keep their own reference) */
        for (int i = 0; i < MAX_PICTURES; i++)
            if (pics[i].id == id) { ID3D11ShaderResourceView_Release(pics[i].srv); pics[i].id = 0, pics[i].srv = NULL; }
    unsigned nverts;
    const OverlayVertex *verts = overlay_quads(&nverts);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, verts, nverts * sizeof *verts);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vbuf, 0);
    UINT stride = sizeof(OverlayVertex), offset = 0;
    D3D11_VIEWPORT vp = {(float)x0, (float)y0, (float)w, (float)h, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, raster);
    ID3D11DeviceContext_OMSetBlendState(ctx, blend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbuf, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sampler);
    unsigned nb;
    const OverlayBatch *b = overlay_batches(&nb);
    for (unsigned k = 0; k < nb; k++) {   /* (the glyphs with the atlas, each picture with its own) */
        ID3D11ShaderResourceView *srv = b[k].texture ? picture(dev, b[k].texture) : atlas;
        if (!b[k].count || !srv) continue;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float inv[4] = {1.0f / (float)w, 1.0f / (float)h, (float)b[k].mode, 0};
            memcpy(m.pData, inv, sizeof inv);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);
        }
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_Draw(ctx, b[k].count, b[k].first);
    }
    ID3D11Buffer *none_buf = NULL;
    ID3D11ShaderResourceView *none_srv = NULL;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &none_buf, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none_srv);
}

static int ready(ID3D11Device *dev) {
    if (!state) state = init(dev) ? 1 : -1;
    return state > 0;
}

/* ---- the frame ---------------------------------------------------------------------- */
void overlay_present(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w,
                     unsigned h, int show, const char *detail) {
    if (!overlay_build_present(show, detail) || !ready(dev)) return;
    flush(dev, ctx, rtv, x0, y0, w, h);
}

void overlay_progress(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned w, unsigned h,
                      const char *title, unsigned done, unsigned total, const char *status) {
    if (!ready(dev)) return;
    overlay_build_progress(w, h, title, done, total, status);
    flush(dev, ctx, rtv, 0, 0, w, h);
}

void overlay_menu(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned w, unsigned h, const char *title, int n,
                  const char *const labels[], const char *const values[], const int shown[], int selected, int button, const char *footer,
                  float *rects, float *value_x) {
    if (!ready(dev)) return;
    overlay_build_menu(w, h, title, n, labels, values, shown, selected, button, footer, rects, value_x);
    flush(dev, ctx, rtv, 0, 0, w, h);
}

void overlay_draw_built(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w,
                        unsigned h) {
    if (!ready(dev)) return;
    flush(dev, ctx, rtv, x0, y0, w, h);
}

void overlay_notice(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w, unsigned h,
                    const char *note) {
    if (!ready(dev)) return;
    overlay_build_notice(w, h, note);
    flush(dev, ctx, rtv, x0, y0, w, h);
}

void overlay_bar(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned w, unsigned h, unsigned bar, const char *mode,
                 int button[4]) {
    if (!ready(dev)) return;
    overlay_build_bar(w, h, bar, mode, button);
    flush(dev, ctx, rtv, 0, 0, w, h);
}
