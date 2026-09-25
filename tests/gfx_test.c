/* The graphics back end without the game (R3.2): renders into an offscreen target through gfx.h,
 * reads the pixels back and checks them - clears, D3D's pixel-center rules for XYZRHW vertices,
 * texturing and the texture formats, fixed-function lighting, fog, alpha test, blending, the
 * vs.1.1 / ps.1.1 translation - then builds a sweep of fixed-function keys (every texture op,
 * argument modifier, fog mode, light type, texture coordinate source) so a generator change that
 * emits bad MSL fails here rather than in the game.
 *
 * usage: gfx_test            (exit status 0 when every check passes)
 *        gfx_test --window   the same on a device made for an SDL window, then two seconds of
 *                            frames presented to it (the CAMetalLayer path) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gfx.h"

#define W 32
#define H 32

static int g_fails;

#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_fails++;                                                                                                \
        }                                                                                                             \
    } while (0)

static GfxTex *g_rt, *g_ds;
static uint32_t g_px[W * H];

static void identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void readback(void)
{
    gfx_tex_read(g_rt, 0, 0, g_px, W * 4);
}

/* A8R8G8B8 as the D3D value */
static uint32_t px(int x, int y) { return g_px[y * W + x]; }

static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 32; s += 8)
    {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}

static const uint32_t VP[6] = { 0, 0, W, H, 0, 0x3F800000u };

/* a draw with the device defaults the front end would produce */
static void defaults(GfxDraw* d)
{
    memset(d, 0, sizeof *d);
    identity(d->u.wvp), identity(d->u.wv), identity(d->u.wvit);
    for (int i = 0; i < 8; ++i)
        identity(d->u.texm[i]);
    d->u.vp[0] = 0, d->u.vp[1] = 0, d->u.vp[2] = W, d->u.vp[3] = H;
    d->u.tfactor[0] = d->u.tfactor[1] = d->u.tfactor[2] = d->u.tfactor[3] = 1;
    d->pipe.write_mask = 0xF;
    d->cull = 1;
    d->depth.zfunc = 4;
    memcpy(d->vp, VP, sizeof VP);
    /* stage 0: MODULATE(TEXTURE, CURRENT), SELECTARG1(TEXTURE) alpha - D3D's defaults */
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){ 4, 2, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2 };
}

/* XYZRHW | DIFFUSE | TEX1 (FVF 0x144), the game's UI vertex */
typedef struct
{
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} UiVert;

static void layout_ui(GfxDraw* d)
{
    d->vs.rhw = 1;
    d->vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT4, 0 };
    d->vs.el[5] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d->vs.ntex = 1;
    d->u.stride[0] = sizeof(UiVert);
    d->u.offset[0] = 0, d->u.offset[5] = 16, d->u.offset[7] = 20;
}

static void quad(UiVert* v, float x0, float y0, float x1, float y1, uint32_t c, float z)
{
    v[0] = (UiVert){ x0, y0, z, 1, c, 0, 0 };
    v[1] = (UiVert){ x1, y0, z, 1, c, 1, 0 };
    v[2] = (UiVert){ x0, y1, z, 1, c, 0, 1 };
    v[3] = (UiVert){ x1, y1, z, 1, c, 1, 1 };
}

static void draw_ui(GfxDraw* d, UiVert* v)
{
    d->data[0] = v, d->size[0] = 4 * sizeof(UiVert);
    d->prim = GFX_TRIANGLESTRIP, d->count = 2;
    gfx_draw(d);
}

static void test_clear(void)
{
    gfx_clear(0, NULL, 3, 0xFFFF0000u, 1.0f, 0, VP);
    readback();
    CHECK(px(0, 0) == 0xFFFF0000u && px(W - 1, H - 1) == 0xFFFF0000u, "full clear: %08x", px(0, 0));
    int32_t r[4] = { 4, 4, 8, 8 };
    gfx_clear(1, r, 1, 0xFF00FF00u, 1.0f, 0, VP);
    readback();
    CHECK(px(4, 4) == 0xFF00FF00u && px(7, 7) == 0xFF00FF00u, "rect clear inside: %08x %08x", px(4, 4), px(7, 7));
    CHECK(px(3, 4) == 0xFFFF0000u && px(8, 8) == 0xFFFF0000u, "rect clear outside: %08x %08x", px(3, 4), px(8, 8));
}

static void test_rhw_edges(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    UiVert v[4];
    quad(v, 8, 8, 24, 24, 0xFF0000FFu, 0.5f);
    draw_ui(&d, v);
    readback();
    /* D3D: pixel centers on integers, so [8, 24) covers pixels 8..23 */
    CHECK(px(8, 8) == 0xFF0000FFu && px(23, 23) == 0xFF0000FFu, "quad inside: %08x %08x", px(8, 8), px(23, 23));
    CHECK(px(7, 8) == 0xFF000000u && px(24, 23) == 0xFF000000u && px(8, 7) == 0xFF000000u && px(8, 24) == 0xFF000000u,
        "quad edges: %08x %08x %08x %08x", px(7, 8), px(24, 23), px(8, 7), px(8, 24));
}

static void test_texture(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    /* 2x2: red green / blue white, point sampled over a 16x16 quad */
    uint32_t tex[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 2, 2, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, tex, 8);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0].tex = 1;
    d.tex[0] = t;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0 };
    UiVert v[4];
    quad(v, 0, 0, 16, 16, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(2, 2) == 0xFFFF0000u && px(12, 2) == 0xFF00FF00u && px(2, 12) == 0xFF0000FFu && px(12, 12) == 0xFFFFFFFFu,
        "texture: %08x %08x %08x %08x", px(2, 2), px(12, 2), px(2, 12), px(12, 12));

    /* modulated by the vertex color */
    quad(v, 16, 0, 32, 16, 0xFF808080u, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(28, 12), 0xFF808080u, 1), "modulate: %08x", px(28, 12));
    gfx_tex_destroy(t);

    /* R5G6B5 and DXT1 */
    uint16_t t565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    t = gfx_tex_create(GFX_TEX_2D, 23, 2, 2, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, t565, 4);
    d.tex[0] = t;
    quad(v, 0, 16, 16, 32, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(2, 18) == 0xFFFF0000u && px(12, 18) == 0xFF00FF00u && px(2, 28) == 0xFF0000FFu, "R5G6B5: %08x %08x %08x", px(2, 18),
        px(12, 18), px(2, 28));
    gfx_tex_destroy(t);

    /* one DXT1 block, every texel color0 = pure green (0x07E0) */
    uint8_t dxt[8] = { 0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0 };
    t = gfx_tex_create(GFX_TEX_2D, 0x31545844u /* DXT1 */, 4, 4, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, dxt, 8);
    d.tex[0] = t;
    quad(v, 16, 16, 32, 32, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(24, 24) == 0xFF00FF00u, "DXT1: %08x", px(24, 24));
    gfx_tex_destroy(t);
}

static void test_alpha(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* diffuse only */
    /* alpha test GREATER 0x80: a 0x40-alpha quad is discarded */
    d.fs.alpha_func = 5;
    d.u.params[1] = 128;
    UiVert v[4];
    quad(v, 0, 0, 16, 16, 0x40FFFFFFu, 0.5f);
    draw_ui(&d, v);
    /* blending SRCALPHA / INVSRCALPHA at 50% over black */
    d.fs.alpha_func = 8;
    d.pipe.blend = 1, d.pipe.src = 5, d.pipe.dst = 6, d.pipe.op = 1;
    quad(v, 16, 0, 32, 16, 0x80FFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(8, 8) == 0xFF000000u, "alpha test: %08x", px(8, 8));
    CHECK(near(px(24, 8) & 0xFFFFFF, 0x808080u, 2), "blend: %08x", px(24, 8));
}

static void test_depth(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    UiVert v[4];
    quad(v, 0, 0, 32, 32, 0xFFFF0000u, 0.25f);
    draw_ui(&d, v);
    quad(v, 0, 0, 16, 16, 0xFF00FF00u, 0.75f); /* behind: hidden */
    draw_ui(&d, v);
    quad(v, 16, 16, 32, 32, 0xFF0000FFu, 0.1f); /* in front */
    draw_ui(&d, v);
    readback();
    CHECK(px(8, 8) == 0xFFFF0000u && px(24, 24) == 0xFF0000FFu, "depth: %08x %08x", px(8, 8), px(24, 24));
}

/* XYZ | NORMAL (FVF 0x012) through the transform and one directional light */
static void test_lighting(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    float vert[4][6] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 24, d.u.offset[3] = 12;
    d.vs.lighting = 1, d.vs.nlights = 1, d.vs.light_type[0] = 3, d.vs.normalize = 1;
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    float half[4] = { 0.5f, 0.5f, 0.5f, 1 };
    memcpy(d.u.mat_d, half, 16);
    /* light straight at the surface (toward the light: -z), white; ambient 0.25 on a 1.0 material */
    d.u.light[0].diffuse[0] = d.u.light[0].diffuse[1] = d.u.light[0].diffuse[2] = 1;
    d.u.light[0].dir[2] = -1;
    d.u.ambient[0] = d.u.ambient[1] = d.u.ambient[2] = 0.25f;
    d.u.mat_a[0] = d.u.mat_a[1] = d.u.mat_a[2] = 1;
    d.data[0] = vert, d.size[0] = sizeof vert;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
    readback();
    /* 0.25 ambient + 0.5 diffuse = 0.75 */
    CHECK(near(px(16, 16), 0xFFBFBFBFu, 2), "lighting: %08x", px(16, 16));
}

static void test_fog(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    /* linear table fog from 0 to 2 on w = 1 (rhw 1): half fogged toward white */
    d.fs.fog = 3;
    d.u.params[2] = 0, d.u.params[3] = 2;
    d.u.fogcolor[0] = d.u.fogcolor[1] = d.u.fogcolor[2] = 1;
    UiVert v[4];
    quad(v, 0, 0, 32, 32, 0xFF000000u, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(16, 16), 0xFF808080u, 2), "fog: %08x", px(16, 16));
}

static void test_shaders(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    /* vs.1.1: m4x4 oPos, v0, c0 / mov oD0, c4 / mov oT0, v7 */
    static const uint32_t vs[] = {
        0xFFFE0101u,
        20, 0x80000000u | (4u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16), 0x80000000u | (2u << 28) | (0xE4u << 16),
        1, 0x80000000u | (5u << 28) | (0xFu << 16), 0x80000000u | (2u << 28) | (0xE4u << 16) | 4,
        1, 0x80000000u | (6u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16) | 7,
        0x0000FFFFu,
    };
    /* ps.1.1: tex t0 / mul r0, t0, v0 */
    static const uint32_t ps[] = {
        0xFFFF0101u,
        66, 0x80000000u | (3u << 28) | (0xFu << 16),
        5, 0x80000000u | (0xFu << 16), 0x80000000u | (3u << 28) | (0xE4u << 16), 0x80000000u | (1u << 28) | (0xE4u << 16),
        0x0000FFFFu,
    };
    GfxDraw d;
    defaults(&d);
    d.vs.prog = 1, d.fs.prog = 1, d.vs_tokens = vs, d.ps_tokens = ps;
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d.vs.ntex = 8;
    d.u.stride[0] = 20, d.u.offset[7] = 12;
    identity(&d.u.vsc[0][0]); /* c0..c3 */
    d.u.vsc[4][0] = 1, d.u.vsc[4][1] = 0.5f, d.u.vsc[4][2] = 0, d.u.vsc[4][3] = 1;
    uint32_t white = 0xFFFFFFFFu;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, &white, 4);
    d.tex[0] = t;
    d.fs.st[0].tex = 1;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0 };
    float vert[4][5] = { { -1, 1, 0.5f, 0, 0 }, { 1, 1, 0.5f, 1, 0 }, { -1, -1, 0.5f, 0, 1 }, { 1, -1, 0.5f, 1, 1 } };
    d.data[0] = vert, d.size[0] = sizeof vert;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
    readback();
    CHECK(near(px(16, 16), 0xFFFF8000u, 2), "vs/ps 1.1: %08x", px(16, 16));
    gfx_tex_destroy(t);
}

/* every texture op and argument form, fog mode, light type and coordinate source: all must build */
static void test_sweep(void)
{
    uint32_t before = gfx_failures();
    GfxDraw d;
    UiVert v[4];
    quad(v, 0, 0, 1, 1, 0xFFFFFFFFu, 0.5f);
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    GfxTex* cube = gfx_tex_create(GFX_TEX_CUBE, 21, 1, 1, 1, GFX_USE_SAMPLE);
    for (int op = 1; op <= 26; ++op)
        for (int form = 0; form < 3; ++form)
        {
            defaults(&d);
            layout_ui(&d);
            int mod = form == 1 ? 0x10 : form == 2 ? 0x20 : 0;
            d.fs.nstages = 2;
            d.fs.st[0] = (GfxStage){ (uint8_t)op, (uint8_t)(2 | mod), (uint8_t)(0 | mod), 3, (uint8_t)(op > 1 ? op : 2), 2, 0, 4, 1, 1, 0, 2 };
            d.fs.st[1] = (GfxStage){ (uint8_t)op, 1, (uint8_t)(5 | mod), 4, 1, 1, 1, 1, 5, 2, 0, 3 };
            d.tex[0] = t, d.tex[1] = cube;
            d.fs.specular_add = (uint8_t)(op & 1);
            d.fs.fog = (uint8_t)(op % 5);
            d.fs.alpha_func = (uint8_t)(op % 9);
            d.vs.ntex = 2;
            draw_ui(&d, v);
        }
    for (int lt = 1; lt <= 3; ++lt)
        for (int gen = 0; gen < 4; ++gen)
        {
            defaults(&d);
            float vert[3][8] = { { 0 } };
            d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
            d.u.stride[0] = 32, d.u.offset[3] = 12, d.u.offset[7] = 24;
            d.vs.lighting = 1, d.vs.specular = 1, d.vs.localviewer = (uint8_t)(gen & 1);
            d.vs.nlights = 3, d.vs.light_type[0] = (uint8_t)lt, d.vs.light_type[1] = 3, d.vs.light_type[2] = 1;
            d.vs.src_diffuse = (uint8_t)(gen % 3);
            d.vs.fog_vertex = (uint8_t)gen, d.vs.range_fog = (uint8_t)(lt & 1);
            d.vs.ntex = 2, d.vs.tci[0] = (uint8_t)(gen << 4), d.vs.ttf[0] = (uint8_t)(gen ? 3 : 2), d.vs.tci[1] = 0, d.vs.ttf[1] = 0x82;
            d.fs.nstages = 2;
            d.fs.st[1] = (GfxStage){ 4, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 3 };
            d.tex[0] = d.tex[1] = t;
            d.fs.st[0].tex = 1;
            d.fs.fog = (uint8_t)(gen ? 4 : 0);
            d.vs.flat = d.fs.flat = (uint8_t)(lt == 2);
            d.data[0] = vert, d.size[0] = sizeof vert;
            d.prim = GFX_TRIANGLEFAN, d.count = 1;
            gfx_draw(&d);
        }
    gfx_finish();
    gfx_tex_destroy(t);
    gfx_tex_destroy(cube);
    CHECK(gfx_failures() == before, "%u keys failed to build", gfx_failures() - before);
}

/* frames to a window: a back buffer the size of the window, cleared and drawn, presented */
static void run_window(SDL_Window* win)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    GfxTex* bb = gfx_tex_create(GFX_TEX_2D, 22, (uint32_t)ww, (uint32_t)wh, 1, GFX_USE_RT);
    uint32_t vp[6] = { 0, 0, (uint32_t)ww, (uint32_t)wh, 0, 0x3F800000u };
    gfx_set_targets(bb, 0, 0, NULL);
    Uint64 end = SDL_GetTicks() + 2000;
    for (int frame = 0; SDL_GetTicks() < end; ++frame)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            ;
        gfx_clear(0, NULL, 1, 0xFF203040u, 1.0f, 0, vp);
        GfxDraw d;
        defaults(&d);
        layout_ui(&d);
        d.u.vp[2] = (float)ww, d.u.vp[3] = (float)wh;
        memcpy(d.vp, vp, sizeof vp);
        d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
        float x = (float)(frame % 120) / 120.0f * (float)(ww - 64);
        UiVert v[4];
        quad(v, x, (float)wh / 2 - 32, x + 64, (float)wh / 2 + 32, 0xFFFFA000u, 0.5f);
        draw_ui(&d, v);
        gfx_present(bb);
    }
    gfx_tex_destroy(bb);
}

int main(int argc, char** argv)
{
    SDL_Window* win = NULL;
    if (argc > 1 && !strcmp(argv[1], "--window"))
    {
        if (!SDL_Init(SDL_INIT_VIDEO) || !(win = SDL_CreateWindow("gfx_test", 640, 360, 0)))
        {
            printf("no window: %s\n", SDL_GetError());
            return 1;
        }
    }
    gfx_set_sync_pipelines(1);
    if (!gfx_init(win, 1))
    {
        printf("no graphics back end\n");
        return 1;
    }
    g_rt = gfx_tex_create(GFX_TEX_2D, 21, W, H, 1, GFX_USE_RT);
    g_ds = gfx_tex_create(GFX_TEX_2D, 75, W, H, 1, GFX_USE_DEPTH);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    test_clear();
    test_rhw_edges();
    test_texture();
    test_alpha();
    test_depth();
    test_lighting();
    test_fog();
    test_shaders();
    test_sweep();
    gfx_present(NULL);
    if (win)
    {
        run_window(win);
        CHECK(gfx_failures() == 0, "window frames: %u failures", gfx_failures());
        SDL_DestroyWindow(win);
    }
    printf(g_fails ? "gfx_test: %d failed\n" : "gfx_test: ok\n", g_fails);
    return g_fails != 0;
}
