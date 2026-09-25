/* The graphics back end on Metal (macOS, R3.2): what gfx.h asks for, on one MTLDevice.
 *
 *   - Frames: one command buffer at a time, committed at Present (or when the CPU needs a result);
 *     up to three frames in flight, each with its own upload ring - vertex, index and uniform bytes
 *     are copied into the ring per draw, so the game may rewrite a buffer the moment a draw returns,
 *     as D3D lets it.
 *   - Render passes open on the first draw or clear after the targets change, and close when they
 *     change again, at Present, or for a blit. A clear of the whole target becomes the next pass's
 *     load action; a partial one draws a quad.
 *   - Pipelines come from the generated MSL (gfx_msl.c), cached by key; so are depth-stencil states
 *     and samplers.
 *   - Textures: sampled ones are shared storage, filled with replaceRegion when the GPU is done with
 *     them and through a blit otherwise; render targets and depth are private. Formats Metal has no
 *     match for (the 16-bit color ones) are widened to BGRA8 on upload.
 *
 * Built without ARC: every object here is retained and released by hand, and each entry point runs
 * in its own autorelease pool (the callers are guest threads with none). */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_msl.h"

#if __has_feature(objc_arc)
#error gfx_metal.m manages its references by hand: build it with -fno-objc-arc
#endif

#define FRAMES 3
#define GFX_PROBES 8     /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)

/* D3DFORMAT values the back end maps */
enum
{
    F_A8R8G8B8 = 21,
    F_X8R8G8B8 = 22,
    F_R5G6B5 = 23,
    F_X1R5G5B5 = 24,
    F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26,
    F_A8 = 28,
    F_L8 = 50,
    F_A8L8 = 51,
    F_V8U8 = 60,
    F_D24S8 = 75,
    F_D24X8 = 77,
    F_D16 = 80,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum
{
    CONV_NONE,
    CONV_565,
    CONV_X555,
    CONV_1555,
    CONV_4444,
};

struct GfxTex
{
    id<MTLTexture> tex;  /* what is rendered to and uploaded into */
    id<MTLTexture> view; /* what is sampled: tex, or a swizzled view of it */
    int type, use, conv;
    uint32_t fmt, w, h, levels;
    uint32_t block;  /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel;  /* bytes per texel in Metal's layout */
    uint64_t used;   /* the last frame serial that referenced it */
    int has_stencil, x8;
    /* asynchronous readbacks (gfx_tex_read_async): staging buffers and the frame each was recorded in */
    id<MTLBuffer> rb[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
};

typedef struct Chunk
{
    id<MTLBuffer> buf;
    uint32_t used, size;
} Chunk;

typedef struct Frame
{
    Chunk* chunks;
    uint32_t nchunks, cur;
} Frame;

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static CAMetalLayer* g_layer;
static SDL_MetalView g_view;
static SDL_Window* g_window;
static id<MTLCommandBuffer> g_cmd;
static id<MTLRenderCommandEncoder> g_enc;
static dispatch_semaphore_t g_frames_sem;
static Frame g_frames[FRAMES];
static uint32_t g_frame;              /* index into g_frames */
static uint64_t g_serial = 1;         /* the frame being recorded */
static _Atomic uint64_t g_completed;  /* the last frame the GPU finished */
static int g_frame_open;              /* the semaphore was taken for g_serial */
static uint32_t g_cmd_draws;          /* draws in the command buffer being recorded */
static _Atomic uint64_t g_gpu_ns;      /* GPU time of the committed command buffers (profile) */

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static uint32_t g_pending_clear; /* D3DCLEAR flags for the next pass's load actions */
static float g_clear_color[4], g_clear_z;
static uint32_t g_clear_stencil;
static id<MTLBuffer> g_dummy;
static id<MTLRenderPipelineState> g_present_pipe, g_overlay_pipe;
/* the frame-rate overlay: presents counted over half-second windows */
static int g_overlay = 1;
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";
static id<MTLSamplerState> g_present_samp;
static id<MTLTexture> g_scratch_depth;

/* --- small hash maps (key bytes -> object) ------------------------------------------------------------- */
typedef struct MapEnt
{
    uint64_t hash;
    void* key;
    size_t klen;
    id obj;
} MapEnt;

typedef struct Map
{
    MapEnt* e;
    uint32_t cap, n;
} Map;

static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h ? h : 1;
}

static MapEnt* map_find(Map* m, const void* key, size_t klen, uint64_t h)
{
    if (!m->cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        MapEnt* e = &m->e[i];
        if (!e->hash)
            return NULL;
        if (e->hash == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e;
    }
}

static id map_get(Map* m, const void* key, size_t klen)
{
    MapEnt* e = map_find(m, key, klen, fnv(key, klen));
    return e ? e->obj : nil;
}

static void map_put(Map* m, const void* key, size_t klen, id obj) /* takes the reference */
{
    if ((m->n + 1) * 2 > m->cap)
    {
        Map old = *m;
        m->cap = m->cap ? m->cap * 2 : 256;
        m->e = (MapEnt*)calloc(m->cap, sizeof *m->e);
        m->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].hash)
            {
                uint32_t j = (uint32_t)old.e[i].hash & (m->cap - 1);
                while (m->e[j].hash)
                    j = (j + 1) & (m->cap - 1);
                m->e[j] = old.e[i];
                m->n++;
            }
        free(old.e);
    }
    uint64_t h = fnv(key, klen);
    uint32_t j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].hash)
        j = (j + 1) & (m->cap - 1);
    m->e[j].hash = h;
    m->e[j].key = malloc(klen);
    memcpy(m->e[j].key, key, klen);
    m->e[j].klen = klen;
    m->e[j].obj = obj;
    m->n++;
}

static Map g_libs, g_pipes, g_depths, g_samplers, g_clear_pipes;

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, drawable_ns, present_ns, last_ns, since_ns;
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;

uint64_t gfx_now_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { if (reason >= 0 && reason < GFX_NSKIPS) g_prof.skips[reason]++; }

static pthread_t g_present_thread;

void gfx_prof_shim(uint64_t ns)
{
    if (pthread_equal(pthread_self(), g_present_thread))
        g_prof.shim_ns += ns;
}

/* at each Present: every two seconds, the average frame and where it went */
static void prof_frame(uint64_t present_start)
{
    uint64_t now = gfx_now_ns();
    g_prof.present_ns += now - present_start;
    g_prof.frames++;
    if (!g_prof.since_ns)
        g_prof.since_ns = now;
    if (now - g_prof.since_ns < 2000000000ull)
        return;
    double f = (double)g_prof.frames, ms = 1e-6 / f;
    double frame = (double)(now - g_prof.since_ns) * ms;
    double front = (double)g_prof.front_ns * ms, enc = (double)g_prof.draw_ns * ms;
    double sem = (double)g_prof.sem_ns * ms, drw = (double)g_prof.drawable_ns * ms, pres = (double)g_prof.present_ns * ms;
    double shims = (double)g_prof.shim_ns * ms, probe = (double)g_prof.probe_ns * ms;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode %.2f], probe wait %.2f, "
            "present %.2f [drawable %.2f], other %.2f) | GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, enc, probe, pres, drw,
            shims - front - probe - pres, (double)atomic_exchange(&g_gpu_ns, 0) * ms, (double)g_prof.draws / f,
            (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    else
    fprintf(stderr,
        "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, drawable %.2f) | "
        "GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
        f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, drw,
        (double)atomic_exchange(&g_gpu_ns, 0) * ms,
        (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "no pipeline", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}
static uint32_t g_failures;

uint32_t gfx_failures(void) { return g_failures; }

/* --- frames and the upload ring ----------------------------------------------------------------------- */
static void frame_begin(void)
{
    if (g_frame_open)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    dispatch_semaphore_wait(g_frames_sem, DISPATCH_TIME_FOREVER);
    if (gfx_profiling)
        g_prof.sem_ns += gfx_now_ns() - t0;
    g_frame_open = 1;
    Frame* f = &g_frames[g_frame];
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = 0;
    f->cur = 0;
}

static id<MTLCommandBuffer> cmd(void)
{
    frame_begin();
    if (!g_cmd)
        g_cmd = [[g_queue commandBuffer] retain];
    return g_cmd;
}

/* n bytes of this frame's ring; the buffer and offset for binding */
static void* ring(size_t n, size_t align, id<MTLBuffer>* buf, NSUInteger* off)
{
    frame_begin();
    Frame* f = &g_frames[g_frame];
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            uint32_t size = n + align > RING_CHUNK ? (uint32_t)(n + align) : RING_CHUNK;
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            f->chunks[f->nchunks].buf = [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
            f->chunks[f->nchunks].size = size;
            f->chunks[f->nchunks].used = 0;
            f->nchunks++;
        }
        Chunk* c = &f->chunks[f->cur];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + n <= c->size)
        {
            c->used = at + (uint32_t)n;
            g_prof.bytes += n;
            *buf = c->buf;
            *off = at;
            return (uint8_t*)[c->buf contents] + at;
        }
    }
}

static void end_pass(void);
static int begin_pass(void);

/* closes the render pass, first opening one if a clear is still pending, so the clear happens
 * before whatever comes next (a blit, a readback, other targets) */
static void flush_pass(void)
{
    if (g_pending_clear)
        begin_pass();
    end_pass();
}

static void commit_cmd(int wait)
{
    if (gfx_profiling)
        [g_cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
            atomic_fetch_add(&g_gpu_ns, (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9));
        }];
    [g_cmd commit];
    if (wait)
        [g_cmd waitUntilCompleted];
    [g_cmd release];
    g_cmd = nil;
    g_cmd_draws = 0;
}

/* commits what is recorded; the frame stays open (its ring is still in use) */
static void submit(int wait)
{
    flush_pass();
    if (!g_cmd)
        return;
    commit_cmd(wait);
}

/* a chunk of the frame to the GPU (the render pass is already closed) */
static void commit_chunk(void)
{
    if (g_cmd)
        commit_cmd(0);
}

/* --- formats ---------------------------------------------------------------------------------------------- */
static MTLPixelFormat pixel_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, MTLTextureSwizzleChannels* sw,
    int* swizzled)
{
    *conv = CONV_NONE, *block = 0, *texel = 4, *swizzled = 0;
    *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? MTLPixelFormatDepth16Unorm : MTLPixelFormatDepth32Float_Stencil8;
    switch (fmt)
    {
    case F_A8R8G8B8: return MTLPixelFormatBGRA8Unorm;
    case F_X8R8G8B8:
        sw->alpha = MTLTextureSwizzleOne, *swizzled = 1;
        return MTLPixelFormatBGRA8Unorm;
    case F_R5G6B5: *conv = CONV_565; return MTLPixelFormatBGRA8Unorm;
    case F_X1R5G5B5: *conv = CONV_X555; return MTLPixelFormatBGRA8Unorm;
    case F_A1R5G5B5: *conv = CONV_1555; return MTLPixelFormatBGRA8Unorm;
    case F_A4R4G4B4: *conv = CONV_4444; return MTLPixelFormatBGRA8Unorm;
    case F_A8: *texel = 1; return MTLPixelFormatA8Unorm;
    case F_L8:
        *texel = 1, *swizzled = 1;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        return MTLPixelFormatR8Unorm;
    case F_A8L8:
        *texel = 2, *swizzled = 1;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen);
        return MTLPixelFormatRG8Unorm;
    case F_V8U8: *texel = 2; return MTLPixelFormatRG8Snorm;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, MTLPixelFormatBC1_RGBA;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, MTLPixelFormatBC2_RGBA;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, MTLPixelFormatBC3_RGBA;
    return MTLPixelFormatBGRA8Unorm;
}

static uint32_t d3d_bpp(uint32_t fmt)
{
    switch (fmt)
    {
    case F_A8:
    case F_L8: return 1;
    case F_R5G6B5:
    case F_X1R5G5B5:
    case F_A1R5G5B5:
    case F_A4R4G4B4:
    case F_A8L8:
    case F_V8U8: return 2;
    default: return 4;
    }
}

static uint32_t expand(uint32_t v, int bits)
{
    return bits == 1 ? (v ? 255u : 0u) : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}

/* one row of a 16-bit D3D format into BGRA8 */
static void convert_row(int conv, const uint8_t* src, uint8_t* dst, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t p = (uint32_t)src[2 * x] | ((uint32_t)src[2 * x + 1] << 8), r, g, b, a;
        switch (conv)
        {
        case CONV_565: r = expand(p >> 11, 5), g = expand((p >> 5) & 63, 6), b = expand(p & 31, 5), a = 255; break;
        case CONV_X555: r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = 255; break;
        case CONV_1555:
            r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = (p & 0x8000) ? 255 : 0;
            break;
        default: r = (p >> 8 & 15) * 17, g = (p >> 4 & 15) * 17, b = (p & 15) * 17, a = (p >> 12) * 17; break;
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* --- static buffers --------------------------------------------------------------------------------------------- */
struct GfxBuf
{
    id<MTLBuffer> b;
    uint32_t size;
    uint64_t used; /* the last frame serial that drew from it */
};

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = size;
    b->b = [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    [b->b release]; /* command buffers that draw from it hold their own references */
    free(b);
}

void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b)
        return;
    if (size > b->size)
        size = b->size;
    if (b->used > atomic_load(&g_completed))
    {
        /* recorded or running work still reads the old contents: rename */
        [b->b release];
        b->b = [g_dev newBufferWithLength:b->size options:MTLResourceStorageModeShared];
    }
    memcpy([b->b contents], data, size);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    @autoreleasepool
    {
        GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
        MTLTextureSwizzleChannels sw;
        int swizzled;
        MTLPixelFormat pf = pixel_format(fmt, use, &t->conv, &t->block, &t->texel, &sw, &swizzled);
        t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
        t->has_stencil = pf == MTLPixelFormatDepth32Float_Stencil8;
        t->x8 = fmt == F_X8R8G8B8;
        MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
        d.textureType = type == GFX_TEX_CUBE ? MTLTextureTypeCube : MTLTextureType2D;
        d.pixelFormat = pf;
        d.width = t->w;
        d.height = type == GFX_TEX_CUBE ? t->w : t->h;
        d.mipmapLevelCount = t->levels;
        if (use == GFX_USE_SAMPLE)
        {
            d.storageMode = MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead | (swizzled ? MTLTextureUsagePixelFormatView : 0);
        }
        else
        {
            d.storageMode = MTLStorageModePrivate;
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | (swizzled ? MTLTextureUsagePixelFormatView : 0);
        }
        t->tex = [g_dev newTextureWithDescriptor:d];
        [d release];
        if (!t->tex)
        {
            fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
            free(t);
            return NULL;
        }
        if (swizzled && use != GFX_USE_DEPTH)
            t->view = [t->tex newTextureViewWithPixelFormat:pf textureType:t->tex.textureType levels:NSMakeRange(0, t->levels)
                                                     slices:NSMakeRange(0, type == GFX_TEX_CUBE ? 6 : 1) swizzle:sw];
        else
            t->view = [t->tex retain];
        return t;
    }
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        end_pass(), g_rt = NULL;
    if (g_ds == t)
        end_pass(), g_ds = NULL;
    [t->view release];
    [t->tex release]; /* the command buffers that use it hold their own references */
    for (int i = 0; i < GFX_READBACKS; ++i)
        [t->rb[i] release];
    free(t);
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = (t->type == GFX_TEX_CUBE ? t->w : t->h) >> level;
    if (!*h)
        *h = 1;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels || !w || !h)
        return;
    @autoreleasepool
    {
        uint32_t lw, lh;
        level_size(t, level, &lw, &lh);
        if (x >= lw || y >= lh)
            return;
        if (x + w > lw)
            w = lw - x;
        if (y + h > lh)
            h = lh - y;
        uint32_t rows = t->block ? (h + 3) / 4 : h;
        uint32_t row_bytes = t->block ? ((w + 3) / 4) * t->block : w * t->texel;
        /* the bytes in Metal's layout: as given, or widened */
        const uint8_t* data = (const uint8_t*)src;
        uint8_t* conv = NULL;
        uint32_t data_pitch = pitch;
        if (t->conv)
        {
            conv = (uint8_t*)malloc((size_t)row_bytes * rows);
            for (uint32_t r = 0; r < rows; ++r)
                convert_row(t->conv, data + (size_t)r * pitch, conv + (size_t)r * row_bytes, w);
            data = conv, data_pitch = row_bytes;
        }
        MTLRegion region = MTLRegionMake2D(x, y, w, h);
        int busy = t->used > atomic_load(&g_completed);
        if (t->use == GFX_USE_SAMPLE && !busy)
            [t->tex replaceRegion:region mipmapLevel:level slice:face withBytes:data bytesPerRow:data_pitch bytesPerImage:0];
        else
        {
            /* in use by recorded or running work, or private: through the ring, in order */
            id<MTLBuffer> buf;
            NSUInteger off;
            uint8_t* dst = (uint8_t*)ring((size_t)row_bytes * rows, 256, &buf, &off);
            for (uint32_t r = 0; r < rows; ++r)
                memcpy(dst + (size_t)r * row_bytes, data + (size_t)r * data_pitch, row_bytes);
            flush_pass();
            id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
            [blit copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:row_bytes sourceBytesPerImage:(NSUInteger)row_bytes * rows
                      sourceSize:MTLSizeMake(w, h, 1) toTexture:t->tex destinationSlice:face destinationLevel:level
               destinationOrigin:MTLOriginMake(x, y, 0)];
            [blit endEncoding];
            t->used = g_serial;
        }
        free(conv);
    }
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back in Metal's layout -> D3D's */
static void copy_out(const GfxTex* t, const uint8_t* s, uint32_t w, uint32_t h, uint32_t row, void* dst, uint32_t pitch)
{
    uint32_t bpp = d3d_bpp(t->fmt);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* d = (uint8_t*)dst + (size_t)y * pitch;
        if (!t->conv)
            memcpy(d, s + (size_t)y * row, (size_t)w * bpp);
        else /* 16-bit color targets: back from BGRA8 */
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = s + (size_t)y * row + 4 * x;
                uint32_t b = p[0], g = p[1], r = p[2], a = p[3], v;
                switch (t->conv)
                {
                case CONV_565: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
                case CONV_4444: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
                default: v = (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
                }
                d[2 * x] = (uint8_t)v, d[2 * x + 1] = (uint8_t)(v >> 8);
            }
    }
}

/* a blit of the level into buf, recorded with the frame's other work */
static void queue_readback(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, uint32_t row, id<MTLBuffer> buf)
{
    flush_pass();
    id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
    [blit copyFromTexture:t->tex sourceSlice:face sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:(NSUInteger)row * h];
    [blit endEncoding];
    t->used = g_serial;
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block)
        return;
    @autoreleasepool
    {
        uint32_t w, h;
        level_size(t, level, &w, &h);
        uint32_t row = w * t->texel;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:(NSUInteger)row * h options:MTLResourceStorageModeShared];
        queue_readback(t, face, level, w, h, row, buf);
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        submit(1);
        if (gfx_profiling)
            g_prof.probe_ns += gfx_now_ns() - t0;
        copy_out(t, (const uint8_t*)[buf contents], w, h, row, dst, pitch);
        [buf release];
    }
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one
 * probe: copy a region, lock, read; copy another, lock, read) keeps each read's history apart:
 * the k-th read this frame gets the k-th read of the newest frame the GPU has finished, never
 * another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || level >= t->levels || t->use == GFX_USE_DEPTH || t->block)
        return;
    @autoreleasepool
    {
        uint32_t w, h;
        level_size(t, level, &w, &h);
        uint32_t row = w * t->texel;
        if (t->rb_frame != g_serial)
            t->rb_frame = g_serial, t->rb_count = 0;
        uint32_t index = t->rb_count++;
        if (index >= GFX_PROBES)
        {
            gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
            return;
        }
        uint64_t done = atomic_load(&g_completed);
        int best = -1;
        for (int i = 0; i < GFX_READBACKS; ++i)
            if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] <= done && t->rb_index[i] == index && t->rb_face[i] == face &&
                t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
                best = i;
        if (best < 0)
            gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
        else
            copy_out(t, (const uint8_t*)[t->rb[best] contents], w, h, row, dst, pitch);
        /* this read's copy for a later frame: a slot the GPU is done with, not the one just read */
        int slot = -1;
        for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
            if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
                slot = i;
        if (slot < 0)
            return;
        if (!t->rb[slot] || [t->rb[slot] length] < (NSUInteger)row * h)
        {
            [t->rb[slot] release];
            t->rb[slot] = [g_dev newBufferWithLength:(NSUInteger)row * h options:MTLResourceStorageModeShared];
        }
        queue_readback(t, face, level, w, h, row, t->rb[slot]);
        t->rb_serial[slot] = g_serial, t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
    }
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src->tex.pixelFormat != dst->tex.pixelFormat || !w || !h)
        return;
    @autoreleasepool
    {
        flush_pass();
        id<MTLBlitCommandEncoder> blit = [cmd() blitCommandEncoder];
        [blit copyFromTexture:src->tex sourceSlice:sface sourceLevel:slevel sourceOrigin:MTLOriginMake(sx, sy, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toTexture:dst->tex destinationSlice:dface destinationLevel:dlevel
            destinationOrigin:MTLOriginMake(dx, dy, 0)];
        [blit endEncoding];
        src->used = dst->used = g_serial;
    }
}

/* --- render passes --------------------------------------------------------------------------------------------- */
/* Draws recorded since the last commit. A frame goes to the GPU in chunks - the command buffer is
 * committed when a render pass ends with this many behind it - so the GPU works while the game
 * builds the rest, and a mid-frame readback (the game's per-frame probe) waits only for the tail. */
#define CHUNK_DRAWS 96
#define SPLIT_DRAWS 320
static void commit_chunk(void);

static void end_pass(void)
{
    if (g_enc)
    {
        [g_enc endEncoding];
        [g_enc release];
        g_enc = nil;
        if (g_cmd_draws >= CHUNK_DRAWS)
            commit_chunk();
    }
}

static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the depth attachment for the current color target: the bound one, or a scratch one of the color
 * target's size when they differ (D3D lets depth be larger; Metal wants them equal) */
static id<MTLTexture> depth_attachment(void)
{
    if (!g_ds || !g_rt)
        return nil;
    uint32_t w, h;
    color_size(&w, &h);
    if (g_ds->w == w && g_ds->h == h)
        return g_ds->tex;
    if (!g_scratch_depth || g_scratch_depth.width != w || g_scratch_depth.height != h ||
        g_scratch_depth.pixelFormat != g_ds->tex.pixelFormat)
    {
        [g_scratch_depth release];
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_ds->tex.pixelFormat width:w height:h
                                                                                mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget;
        g_scratch_depth = [g_dev newTextureWithDescriptor:d];
    }
    return g_scratch_depth;
}

static int begin_pass(void)
{
    if (g_enc)
        return 1;
    if (!g_rt)
        return 0;
    MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = g_rt->tex;
    p.colorAttachments[0].slice = g_rt_face;
    p.colorAttachments[0].level = g_rt_level;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (g_pending_clear & 1)
    {
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(g_clear_color[0], g_clear_color[1], g_clear_color[2], g_clear_color[3]);
    }
    else
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    id<MTLTexture> depth = depth_attachment();
    if (depth)
    {
        p.depthAttachment.texture = depth;
        p.depthAttachment.storeAction = MTLStoreActionStore;
        p.depthAttachment.loadAction = (g_pending_clear & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        p.depthAttachment.clearDepth = g_clear_z;
        if (g_ds->has_stencil)
        {
            p.stencilAttachment.texture = depth;
            p.stencilAttachment.storeAction = MTLStoreActionStore;
            p.stencilAttachment.loadAction = (g_pending_clear & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
            p.stencilAttachment.clearStencil = g_clear_stencil;
        }
    }
    g_pending_clear = 0;
    g_enc = [[cmd() renderCommandEncoderWithDescriptor:p] retain];
    g_rt->used = g_serial;
    if (g_ds)
        g_ds->used = g_serial;
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    @autoreleasepool
    {
        flush_pass(); /* a clear nothing drew after still happens */
    }
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
}

/* --- pipelines ------------------------------------------------------------------------------------------------- */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

typedef struct PipeKey
{
    LibKey lib;
    GfxPipeKey pipe;
    uint32_t color, depth, stencil; /* MTLPixelFormat */
    uint8_t x8, pad[3];
} PipeKey;

static MTLBlendFactor blend_factor(uint32_t f, int x8)
{
    switch (f)
    {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return x8 ? MTLBlendFactorOne : MTLBlendFactorDestinationAlpha;
    case 8: return x8 ? MTLBlendFactorZero : MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static id<MTLLibrary> compile(const char* src)
{
    NSError* err = nil;
    NSString* s = [[NSString alloc] initWithUTF8String:src];
    MTLCompileOptions* o = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:s options:o error:&err];
    [o release];
    [s release];
    if (!lib)
        g_failures++, fprintf(stderr, "[recomp] gfx: MSL compile failed: %s\n%s\n", err ? [[err localizedDescription] UTF8String] : "?", src);
    return lib;
}

static id<MTLLibrary> library(const GfxDraw* d)
{
    LibKey k;
    memset(&k, 0, sizeof k);
    k.vs = d->vs, k.fs = d->fs;
    id lib = map_get(&g_libs, &k, sizeof k);
    if (lib)
        return lib == (id)[NSNull null] ? nil : lib;
    char* src = gfx_msl_generate(&d->vs, &d->fs, d->vs_tokens, d->ps_tokens);
    lib = src ? compile(src) : nil;
    if (!src)
        g_failures++;
    free(src);
    map_put(&g_libs, &k, sizeof k, lib ? lib : [[NSNull null] retain]);
    return lib;
}

static id<MTLRenderPipelineState> pipeline(const GfxDraw* d)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.color = (uint32_t)g_rt->tex.pixelFormat;
    id<MTLTexture> depth = depth_attachment();
    k.depth = depth ? (uint32_t)depth.pixelFormat : 0;
    k.stencil = depth && g_ds->has_stencil ? k.depth : 0;
    k.x8 = (uint8_t)g_rt->x8;
    id p = map_get(&g_pipes, &k, sizeof k);
    if (p)
        return p == (id)[NSNull null] ? nil : p;
    id<MTLLibrary> lib = library(d);
    if (lib)
    {
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> vf = [lib newFunctionWithName:@"vs_main"], ff = [lib newFunctionWithName:@"fs_main"];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
        MTLRenderPipelineColorAttachmentDescriptor* c = pd.colorAttachments[0];
        c.pixelFormat = (MTLPixelFormat)k.color;
        uint32_t wm = d->pipe.write_mask;
        c.writeMask = ((wm & 1) ? MTLColorWriteMaskRed : 0) | ((wm & 2) ? MTLColorWriteMaskGreen : 0) |
            ((wm & 4) ? MTLColorWriteMaskBlue : 0) | ((wm & 8) ? MTLColorWriteMaskAlpha : 0);
        if (d->pipe.blend)
        {
            uint32_t s = d->pipe.src, t = d->pipe.dst;
            if (s == 12) /* BOTHSRCALPHA */
                s = 5, t = 6;
            else if (s == 13) /* BOTHINVSRCALPHA */
                s = 6, t = 5;
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = blend_factor(s, k.x8);
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = blend_factor(t, k.x8);
            c.rgbBlendOperation = c.alphaBlendOperation = blend_op(d->pipe.op);
        }
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth;
        pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k.stencil;
        NSError* err = nil;
        g_prof.pipelines++;
        p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p)
            g_failures++, fprintf(stderr, "[recomp] gfx: pipeline failed: %s\n", err ? [[err localizedDescription] UTF8String] : "?");
        [vf release];
        [ff release];
        [pd release];
    }
    map_put(&g_pipes, &k, sizeof k, p ? p : [[NSNull null] retain]);
    return p;
}

static MTLCompareFunction compare(uint32_t f)
{
    switch (f)
    {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

static id<MTLDepthStencilState> depth_state(const GfxDepthKey* k)
{
    id s = map_get(&g_depths, k, sizeof *k);
    if (s)
        return s;
    MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = k->zenable ? compare(k->zfunc) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = k->zenable && k->zwrite;
    if (k->stencil)
    {
        MTLStencilDescriptor* st = [[MTLStencilDescriptor alloc] init];
        st.stencilCompareFunction = compare(k->sfunc);
        st.stencilFailureOperation = stencil_op(k->sfail);
        st.depthFailureOperation = stencil_op(k->szfail);
        st.depthStencilPassOperation = stencil_op(k->spass);
        st.readMask = k->sread;
        st.writeMask = k->swrite;
        d.frontFaceStencil = st;
        d.backFaceStencil = st;
        [st release];
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    [d release];
    map_put(&g_depths, k, sizeof *k, s);
    return s;
}

static MTLSamplerAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

static id<MTLSamplerState> sampler(const GfxSampler* k)
{
    id s = map_get(&g_samplers, k, sizeof *k);
    if (s)
        return s;
    MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
    d.sAddressMode = address(k->addr_u);
    d.tAddressMode = address(k->addr_v);
    d.rAddressMode = address(k->addr_w);
    d.magFilter = k->mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = k->min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = k->mip == 0 ? MTLSamplerMipFilterNotMipmapped : k->mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1)
        d.maxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    d.lodMinClamp = k->max_level;
    uint32_t a = k->border >> 24, rgb = k->border & 0xFFFFFF;
    d.borderColor = a < 128 ? MTLSamplerBorderColorTransparentBlack
        : rgb >= 0x808080 ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    s = [g_dev newSamplerStateWithDescriptor:d];
    [d release];
    map_put(&g_samplers, k, sizeof *k, s);
    return s;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static void set_viewport(const uint32_t vp[6])
{
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    uint32_t w, h;
    color_size(&w, &h);
    double x = vp[0], y = vp[1], vw = vp[2], vh = vp[3];
    if (x > w)
        x = w;
    if (y > h)
        y = h;
    if (x + vw > w)
        vw = w - x;
    if (y + vh > h)
        vh = h - y;
    [g_enc setViewport:(MTLViewport){ x, y, vw, vh, zmin, zmax }];
}

/* index count (Metal) for a D3D primitive count */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP: return n + 2;
    case GFX_TRIANGLEFAN: return n * 3; /* as a list */
    default: return 0;
    }
}

static MTLPrimitiveType metal_prim(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return MTLPrimitiveTypePoint;
    case GFX_LINELIST: return MTLPrimitiveTypeLine;
    case GFX_LINESTRIP: return MTLPrimitiveTypeLineStrip;
    case GFX_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

static void draw_encode(const GfxDraw* d);

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    draw_encode(d);
    g_cmd_draws++;
    /* a long pass (the 3D scene) goes to the GPU in pieces too: end it here, commit, and the next
     * draw resumes it with its contents loaded */
    if (g_cmd_draws >= SPLIT_DRAWS && g_enc)
        end_pass();
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

static void draw_encode(const GfxDraw* d)
{
    @autoreleasepool
    {
        if (!begin_pass())
        {
            gfx_prof_skip(GFX_SKIP_NO_TARGET);
            return;
        }
        id<MTLRenderPipelineState> p = pipeline(d);
        if (!p)
        {
            gfx_prof_skip(GFX_SKIP_PIPELINE);
            return;
        }
        [g_enc setRenderPipelineState:p];
        GfxDepthKey dk = d->depth;
        if (!depth_attachment())
            memset(&dk, 0, sizeof dk);
        [g_enc setDepthStencilState:depth_state(&dk)];
        if (dk.stencil)
            [g_enc setStencilReferenceValue:d->stencil_ref];
        /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
        [g_enc setFrontFacingWinding:MTLWindingClockwise];
        [g_enc setCullMode:d->cull == 3 ? MTLCullModeBack : d->cull == 2 ? MTLCullModeFront : MTLCullModeNone];
        [g_enc setTriangleFillMode:d->fill == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        [g_enc setDepthBias:-(float)d->zbias slopeScale:-(float)d->zbias * 0.5f clamp:0];
        set_viewport(d->vp);

        id<MTLBuffer> buf;
        NSUInteger off;
        /* the uniforms the draw's functions read: the lights only when lit, the vertex shader's
         * constants only for a vertex shader, the pixel shader's only for a pixel shader (the ring
         * keeps room for the whole struct: that is what the functions are compiled against) */
        size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
        if (d->vs.prog)
            need = offsetof(GfxU, psc);
        if (d->fs.prog)
            need = sizeof(GfxU);
        void* u = ring(sizeof(GfxU), 256, &buf, &off);
        memcpy(u, &d->u, need);
        [g_enc setVertexBuffer:buf offset:off atIndex:4];
        [g_enc setFragmentBuffer:buf offset:off atIndex:4];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
        {
            if (d->buf[s])
            {
                [g_enc setVertexBuffer:d->buf[s]->b offset:d->buf_off[s] atIndex:(NSUInteger)s];
                d->buf[s]->used = g_serial;
            }
            else if (d->data[s] && d->size[s])
            {
                void* v = ring(d->size[s], 16, &buf, &off);
                memcpy(v, d->data[s], d->size[s]);
                [g_enc setVertexBuffer:buf offset:off atIndex:(NSUInteger)s];
            }
            else
                [g_enc setVertexBuffer:g_dummy offset:0 atIndex:(NSUInteger)s];
        }
        for (int i = 0; i < 8; ++i)
        {
            GfxTex* t = d->tex[i];
            int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
            if (!wanted || !t)
                continue;
            [g_enc setFragmentTexture:t->view atIndex:(NSUInteger)i];
            [g_enc setFragmentSamplerState:sampler(&d->samp[i]) atIndex:(NSUInteger)i];
            t->used = g_serial;
        }

        uint32_t n = vertex_count(d->prim, d->count);
        MTLPrimitiveType mp = metal_prim(d->prim);
        if (d->prim == GFX_TRIANGLEFAN)
        {
            /* no fans in Metal: a list with the same vertices */
            uint32_t* idx = (uint32_t*)ring((size_t)n * 4, 16, &buf, &off);
            for (uint32_t i = 0; i < d->count; ++i)
            {
                uint32_t k[3] = { 0, i + 1, i + 2 };
                for (int j = 0; j < 3; ++j)
                {
                    if (!d->indices)
                        idx[3 * i + j] = d->vertex_start + k[j];
                    else if (d->index_size == 2)
                        idx[3 * i + j] = ((const uint16_t*)d->indices)[k[j]];
                    else
                        idx[3 * i + j] = ((const uint32_t*)d->indices)[k[j]];
                }
            }
            [g_enc drawIndexedPrimitives:mp indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:buf indexBufferOffset:off];
        }
        else if (d->ibuf)
        {
            d->ibuf->used = g_serial;
            [g_enc drawIndexedPrimitives:mp indexCount:n indexType:d->index_size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                             indexBuffer:d->ibuf->b indexBufferOffset:d->ibuf_off];
        }
        else if (d->indices)
        {
            void* idx = ring((size_t)n * d->index_size, 16, &buf, &off);
            memcpy(idx, d->indices, (size_t)n * d->index_size);
            [g_enc drawIndexedPrimitives:mp indexCount:n indexType:d->index_size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                             indexBuffer:buf indexBufferOffset:off];
        }
        else
            [g_enc drawPrimitives:mp vertexStart:d->vertex_start vertexCount:n];
    }
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
static const char CLEAR_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct CU { float4 rect; float4 color; float z; };\n"
    "struct CO { float4 pos [[position]]; };\n"
    "vertex CO clear_vs(uint vid [[vertex_id]], constant CU& u [[buffer(0)]]) {\n"
    "  float2 c = float2((vid & 1) ? u.rect.z : u.rect.x, (vid & 2) ? u.rect.w : u.rect.y);\n"
    "  CO o; o.pos = float4(c, u.z, 1.0); return o;\n"
    "}\n"
    "fragment float4 clear_fs(constant CU& u [[buffer(0)]]) { return u.color; }\n"
    "struct PO { float4 pos [[position]]; float2 uv; };\n"
    "vertex PO present_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture */
    "struct OU { float4 rect; float scale; uint n; uint pad0, pad1; uint4 text[8]; };\n"
    "constant uchar FONT[17 * 7] = {\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00 };\n"
    "struct OO { float4 pos [[position]]; };\n"
    "vertex OO overlay_vs(uint vid [[vertex_id]], constant OU& u [[buffer(0)]], constant float2& size [[buffer(1)]]) {\n"
    "  float2 c = u.rect.xy + float2((vid & 1) ? u.rect.z : 0.0, (vid & 2) ? u.rect.w : 0.0);\n"
    "  OO o; o.pos = float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1); return o;\n"
    "}\n"
    "fragment float4 overlay_fs(OO in [[stage_in]], constant OU& u [[buffer(0)]]) {\n"
    "  float2 p = (in.pos.xy - u.rect.xy) / u.scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  if (p.x >= 0.0 && cell < int(u.n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = u.text[cell >> 2][cell & 3];\n"
    "    if ((FONT[ch * 7 + uint(gy)] >> (4 - gx)) & 1) return float4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "  return float4(0, 0, 0, 0.55);\n"
    "}\n"
    "fragment float4 present_fs(PO in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, in.uv).rgb, 1.0);\n"
    "}\n";

static id<MTLLibrary> g_util;

static id<MTLRenderPipelineState> clear_pipeline(uint32_t flags)
{
    uint32_t k[4] = { (uint32_t)g_rt->tex.pixelFormat, 0, 0, flags & 1 };
    id<MTLTexture> depth = depth_attachment();
    k[1] = depth ? (uint32_t)depth.pixelFormat : 0;
    k[2] = depth && g_ds->has_stencil ? k[1] : 0;
    id p = map_get(&g_clear_pipes, k, sizeof k);
    if (p)
        return p;
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [g_util newFunctionWithName:@"clear_vs"], ff = [g_util newFunctionWithName:@"clear_fs"];
    pd.vertexFunction = vf;
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = (MTLPixelFormat)k[0];
    pd.colorAttachments[0].writeMask = (flags & 1) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)k[1];
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k[2];
    p = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
    [vf release];
    [ff release];
    [pd release];
    map_put(&g_clear_pipes, k, sizeof k, p);
    return p;
}

void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags)
        return;
    @autoreleasepool
    {
        float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
        uint32_t w, h;
        color_size(&w, &h);
        int whole = !nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
        if (whole)
        {
            /* the next pass starts cleared: close this one (what it drew to cleared attachments is
             * overwritten anyway) */
            end_pass();
            g_pending_clear |= flags;
            if (flags & 1)
                memcpy(g_clear_color, c, sizeof c);
            if (flags & 2)
                g_clear_z = z;
            if (flags & 4)
                g_clear_stencil = stencil;
            return;
        }
        if (!begin_pass())
            return;
        /* the viewport, intersected with each rectangle, as a quad at depth z */
        int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
        int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
        if (!nrects)
            rects = whole_rect, nrects = 1;
        [g_enc setRenderPipelineState:clear_pipeline(flags)];
        GfxDepthKey dk;
        memset(&dk, 0, sizeof dk);
        dk.zenable = (flags & 2) != 0, dk.zwrite = 1, dk.zfunc = 8;
        if (flags & 4)
            dk.stencil = 1, dk.sfunc = 8, dk.sfail = dk.szfail = dk.spass = 3, dk.sread = dk.swrite = 0xFF;
        [g_enc setDepthStencilState:depth_state(&dk)];
        [g_enc setStencilReferenceValue:stencil];
        [g_enc setCullMode:MTLCullModeNone];
        [g_enc setTriangleFillMode:MTLTriangleFillModeFill];
        [g_enc setDepthBias:0 slopeScale:0 clamp:0];
        [g_enc setViewport:(MTLViewport){ 0, 0, w, h, 0, 1 }];
        for (uint32_t i = 0; i < nrects; ++i)
        {
            int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
            int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
            if (x1 <= x0 || y1 <= y0)
                continue;
            struct
            {
                float rect[4], color[4], z, pad[3];
            } cu = { { x0 * 2.0f / w - 1, 1 - y0 * 2.0f / h, x1 * 2.0f / w - 1, 1 - y1 * 2.0f / h }, { c[0], c[1], c[2], c[3] }, z, { 0 } };
            [g_enc setVertexBytes:&cu length:sizeof cu atIndex:0];
            [g_enc setFragmentBytes:&cu length:sizeof cu atIndex:0];
            [g_enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
static void frame_end(void)
{
    id<MTLCommandBuffer> c = cmd();
    uint64_t serial = g_serial;
    dispatch_semaphore_t sem = g_frames_sem;
    [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store(&g_completed, serial);
        dispatch_semaphore_signal(sem);
    }];
    submit(0);
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_serial++;
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
static void fps_tick(void)
{
    double now = CACurrentMediaTime();
    if (!g_fps_since)
        g_fps_since = now;
    g_fps_frames++;
    double dt = now - g_fps_since;
    if (dt >= 0.5)
    {
        double fps = g_fps_frames / dt;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS %.1fms", fps, dt * 1000.0 / g_fps_frames);
        g_fps_since = now, g_fps_frames = 0;
    }
}

static void draw_overlay(id<MTLRenderCommandEncoder> e, double w, double h)
{
    struct
    {
        float rect[4], scale;
        uint32_t n, pad[2], text[32];
    } u;
    memset(&u, 0, sizeof u);
    for (const char* c = g_fps_text; *c && u.n < 32; ++c)
    {
        uint32_t k = *c >= '0' && *c <= '9' ? (uint32_t)(*c - '0') : *c == 'F' ? 10 : *c == 'P' ? 11 : *c == 'S' ? 12
            : *c == 'm' ? 13 : *c == 's' ? 14 : *c == '.' ? 15 : 16;
        u.text[u.n++] = k;
    }
    u.scale = (float)(h >= 1400 ? 3 : 2);
    u.rect[0] = u.rect[1] = 4 * u.scale;
    u.rect[2] = (float)(u.n * 6 + 3) * u.scale, u.rect[3] = 11 * u.scale;
    float size[2] = { (float)w, (float)h };
    [e setRenderPipelineState:g_overlay_pipe];
    [e setVertexBytes:&u length:sizeof u atIndex:0];
    [e setVertexBytes:size length:sizeof size atIndex:1];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = pthread_self();
    @autoreleasepool
    {
        flush_pass();
        if (g_layer && bb)
        {
            int pw = 0, ph = 0;
            if (g_window)
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
            if (pw > 0 && ph > 0 && (g_layer.drawableSize.width != pw || g_layer.drawableSize.height != ph))
                g_layer.drawableSize = CGSizeMake(pw, ph);
            uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
            id<CAMetalDrawable> drawable = [g_layer nextDrawable];
            if (gfx_profiling)
                g_prof.drawable_ns += gfx_now_ns() - t0;
            if (drawable)
            {
                MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = drawable.texture;
                p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cmd() renderCommandEncoderWithDescriptor:p];
                [e setRenderPipelineState:g_present_pipe];
                [e setFragmentTexture:bb->view atIndex:0];
                [e setFragmentSamplerState:g_present_samp atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                if (g_overlay && g_overlay_pipe)
                    draw_overlay(e, drawable.texture.width, drawable.texture.height);
                [e endEncoding];
                [cmd() presentDrawable:drawable];
                bb->used = g_serial;
            }
        }
        fps_tick();
        frame_end();
    }
    if (gfx_profiling)
        prof_frame(present_start);
}

void gfx_finish(void)
{
    if (!g_dev)
        return;
    @autoreleasepool
    {
        submit(1);
    }
}

void gfx_resize(uint32_t w, uint32_t h)
{
    if (g_layer)
        g_layer.drawableSize = CGSizeMake(w, h);
}

int gfx_init(void* window, int vsync)
{
    if (g_dev)
        return 1;
    @autoreleasepool
    {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev)
        {
            fprintf(stderr, "[recomp] gfx: no Metal device\n");
            return 0;
        }
        g_queue = [g_dev newCommandQueue];
        g_frames_sem = dispatch_semaphore_create(FRAMES);
        g_dummy = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        g_util = compile(CLEAR_MSL);
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> vf = [g_util newFunctionWithName:@"present_vs"], ff = [g_util newFunctionWithName:@"present_fs"];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_present_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
        [vf release];
        [ff release];
        [pd release];
        /* the overlay: blended over the frame; FFXI_FPS=0 turns it off */
        const char* prof = getenv("FFXI_PROFILE");
        gfx_profiling = prof && prof[0] && prof[0] != '0';
        const char* show = getenv("FFXI_FPS");
        g_overlay = !(show && show[0] == '0');
        pd = [[MTLRenderPipelineDescriptor alloc] init];
        vf = [g_util newFunctionWithName:@"overlay_vs"], ff = [g_util newFunctionWithName:@"overlay_fs"];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        g_overlay_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
        [vf release];
        [ff release];
        [pd release];
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        g_present_samp = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];
        if (window)
        {
            g_window = (SDL_Window*)window;
            g_view = SDL_Metal_CreateView(g_window);
            g_layer = g_view ? (CAMetalLayer*)SDL_Metal_GetLayer(g_view) : nil;
            if (g_layer)
            {
                g_layer.device = g_dev;
                g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                g_layer.framebufferOnly = YES;
                g_layer.displaySyncEnabled = vsync ? YES : NO;
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
                if (pw > 0 && ph > 0)
                    g_layer.drawableSize = CGSizeMake(pw, ph);
            }
            else
                fprintf(stderr, "[recomp] gfx: no Metal layer for the window: %s\n", SDL_GetError());
        }
        fprintf(stderr, "[recomp] gfx: Metal on %s\n", [[g_dev name] UTF8String]);
        return 1;
    }
}
