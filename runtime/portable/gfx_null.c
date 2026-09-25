/* The graphics back end where there is none yet (Windows x64, Linux): everything is accepted and
 * nothing is drawn, as before the Metal back end existed. */
#include <stddef.h>

#include "gfx.h"

int gfx_init(void* sdl_window, int vsync)
{
    (void)sdl_window, (void)vsync;
    return 0;
}
void gfx_resize(uint32_t w, uint32_t h) { (void)w, (void)h; }
GfxBuf* gfx_buf_create(uint32_t size)
{
    (void)size;
    return NULL;
}
void gfx_buf_destroy(GfxBuf* b) { (void)b; }
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size) { (void)b, (void)data, (void)size; }
GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    (void)type, (void)d3dfmt, (void)w, (void)h, (void)levels, (void)use;
    return NULL;
}
void gfx_tex_destroy(GfxTex* t) { (void)t; }
void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)src, (void)pitch;
}
void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)x, (void)y, (void)w, (void)h, (void)src, (void)pitch;
}
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)dst, (void)pitch;
}
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)dst, (void)pitch;
}
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    (void)src, (void)sface, (void)slevel, (void)sx, (void)sy, (void)w, (void)h, (void)dst, (void)dface, (void)dlevel, (void)dx, (void)dy;
}
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth) { (void)color, (void)face, (void)level, (void)depth; }
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    (void)nrects, (void)rects, (void)flags, (void)color, (void)z, (void)stencil, (void)vp;
}
void gfx_draw(const GfxDraw* d) { (void)d; }
void gfx_present(GfxTex* backbuffer) { (void)backbuffer; }
void gfx_finish(void) {}
uint32_t gfx_failures(void) { return 0; }
int gfx_profiling;
uint64_t gfx_now_ns(void) { return 0; }
void gfx_prof_front(uint64_t ns) { (void)ns; }
void gfx_prof_skip(int reason) { (void)reason; }
void gfx_prof_shim(uint64_t ns) { (void)ns; }
void gfx_set_sync_pipelines(int on) { (void)on; }
