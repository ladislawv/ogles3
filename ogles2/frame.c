/*
 * Frame-level entries: aglSwapBuffers, glClear, glFlush, glFinish,
 * glViewport, glScissor, and the agl parameter calls (plan sections 6.4,
 * 7.2, 7.7).
 */
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <libraries/ogles2.h>
#include "ogles2_internal.h"

/* A state-only (test) context has no surface: frame entries do nothing. */
static BOOL has_surface(struct OGLES2Context *ctx)
{
    return ogles2_context_valid(ctx) && ctx->v3d;
}

void ogles2_aglSwapBuffers(struct OGLES2Context *ctx)
{
    ULONG t0, t1;
    if (!has_surface(ctx))
        return;
    /* Vsync on a screen of our own, or the one a window is all of: the
     * frame goes out at the refresh (display.c). */
    if (ctx->display.vsync && (ctx->display.own_screen || ctx->display.direct))
        ogles2_display_wait_refresh(&ctx->display);
    ogles2_job_flush(ctx, TRUE);
    ogles2_display_wait_done(&ctx->display);
    t0 = ogles2_clock_us();
    ogles2_display_present(&ctx->display);
    t1 = ogles2_clock_us();
    ctx->sum_present += (double)(ULONG)(t1 - t0);
    if (ctx->n_swaps++)
        ctx->sum_frame += (double)(ULONG)(t1 - ctx->t_last_swap);
    ctx->t_last_swap = t1;
    /* Probe P: the last 256 frames, to see a program slow down over time. */
    if ((ctx->probe & OGLES2_PROBE_PERIODIC) && !(ctx->n_swaps & 255)) {
        ULONG jobs = ctx->n_jobs - ctx->p_jobs;
        if (!jobs)
            jobs = 1;
        ogles2_log("periodic: frames to %lu: %lu us a frame, %lu jobs; per job %lu us of CPU work, %lu us "
                   "render list, %lu us cache, %lu us render wait; %lu textures renamed; %lu bytes of GPU memory",
                   ctx->n_swaps, (ULONG)((ctx->sum_frame - ctx->p_frame) / 256), jobs,
                   (ULONG)((ctx->sum_build - ctx->p_build) / jobs), (ULONG)((ctx->sum_list - ctx->p_list) / jobs),
                   (ULONG)((ctx->sum_pre - ctx->p_pre) / jobs), (ULONG)((ctx->sum_render - ctx->p_render) / jobs),
                   ctx->n_renames - ctx->p_renames, ogles2_device_bytes());
        ogles2_log("periodic: per frame %lu draws, %lu us in them, %lu us in glBuffer(Sub)Data, %lu us in "
                   "glTex(Sub)Image2D",
                   (ctx->n_draws - ctx->p_draws) / 256, (ULONG)((ctx->sum_draw - ctx->p_draw) / 256),
                   (ULONG)((ctx->sum_bufdata - ctx->p_bufdata) / 256),
                   (ULONG)((ctx->sum_teximage - ctx->p_teximage) / 256));
        ogles2_log("periodic: per frame in draws: %lu us checks, %lu shaders, %lu textures, %lu uniforms, "
                   "%lu vertex data, %lu record, %lu indices, %lu binning list",
                   (ULONG)((ctx->stage[0] - ctx->p_stage[0]) / 256), (ULONG)((ctx->stage[1] - ctx->p_stage[1]) / 256),
                   (ULONG)((ctx->stage[2] - ctx->p_stage[2]) / 256), (ULONG)((ctx->stage[3] - ctx->p_stage[3]) / 256),
                   (ULONG)((ctx->stage[4] - ctx->p_stage[4]) / 256), (ULONG)((ctx->stage[5] - ctx->p_stage[5]) / 256),
                   (ULONG)((ctx->stage[6] - ctx->p_stage[6]) / 256), (ULONG)((ctx->stage[7] - ctx->p_stage[7]) / 256));
        {
            int k;
            for (k = 0; k < 8; k++)
                ctx->p_stage[k] = ctx->stage[k];
        }
        ctx->p_draws = ctx->n_draws;
        ctx->p_draw = ctx->sum_draw;
        ctx->p_bufdata = ctx->sum_bufdata;
        ctx->p_teximage = ctx->sum_teximage;
        ctx->p_frame = ctx->sum_frame;
        ctx->p_build = ctx->sum_build;
        ctx->p_list = ctx->sum_list;
        ctx->p_pre = ctx->sum_pre;
        ctx->p_render = ctx->sum_render;
        ctx->p_jobs = ctx->n_jobs;
        ctx->p_renames = ctx->n_renames;
    }
}

void ogles2_glFlush(struct OGLES2Context *ctx)
{
    if (has_surface(ctx))
        ogles2_job_flush(ctx, FALSE);
}

void ogles2_glFinish(struct OGLES2Context *ctx)
{
    if (has_surface(ctx)) {
        ogles2_job_flush(ctx, FALSE);
        ogles2_job_finish(ctx); /* a flush may leave its render running (M6) */
    }
}

/* RGBA / UNSIGNED_BYTE, which is also the implementation's read format
 * (plan 7.2), and EXT_read_format_bgra's BGRA with unsigned bytes or the
 * two reversed 16-bit packings: the open job is rendered first, then the
 * rows are read from the render bitmap bottom-up (GL's origin is the
 * bottom-left, 7.11), or from a framebuffer object's colour image (fbo.c;
 * its row 0 is GL's). Pixels outside the framebuffer are left as they are.
 * ES 3.0's (4.3.1, stage B item 10): a float read buffer reads as
 * RGBA/FLOAT, a signed or unsigned integer one as RGBA_INTEGER with INT or
 * UNSIGNED_INT, RGB10_A2 also as its packed type; each one's pair is its
 * implementation read format (ogles2_read_format). */
static BOOL read_format(BOOL es3, GLenum f)
{
    return f == GL_ALPHA || f == GL_RGB || f == GL_RGBA || f == GL_LUMINANCE || f == GL_LUMINANCE_ALPHA ||
           f == GL_BGRA_EXT ||
           (es3 && (f == GL_RED || f == GL_RG || f == GL_RED_INTEGER || f == GL_RG_INTEGER ||
                    f == GL_RGB_INTEGER || f == GL_RGBA_INTEGER));
}

static BOOL read_type(BOOL es3, GLenum t)
{
    return t == GL_UNSIGNED_BYTE || t == GL_UNSIGNED_SHORT_5_6_5 || t == GL_UNSIGNED_SHORT_4_4_4_4 ||
           t == GL_UNSIGNED_SHORT_5_5_5_1 || t == GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT ||
           t == GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT ||
           (es3 && (t == GL_BYTE || t == GL_UNSIGNED_SHORT || t == GL_SHORT || t == GL_UNSIGNED_INT ||
                    t == GL_INT || t == GL_HALF_FLOAT || t == GL_FLOAT || t == GL_UNSIGNED_INT_2_10_10_10_REV ||
                    t == GL_UNSIGNED_INT_10F_11F_11F_REV || t == GL_UNSIGNED_INT_5_9_9_9_REV));
}

/* The implementation read format and type of the current read buffer
 * (state.c's GL_IMPLEMENTATION_COLOR_READ_*): RGBA/UNSIGNED_BYTE but for
 * ES 3.0's float and integer buffers and RGB10_A2. */
void ogles2_read_format(struct OGLES2Context *ctx, GLenum *format, GLenum *type)
{
    struct OGLES2RtFormat rf;
    BOOL ok;
    *format = GL_RGBA;
    *type = GL_UNSIGNED_BYTE;
    if (!ogles2_es3(ctx) || !ctx->read_fb)
        return;
    {
        struct OGLES2Framebuffer *saved = ogles2_fbo_read_begin(ctx);
        ok = ogles2_fbo_read_format(ctx, &rf);
        ogles2_fbo_read_end(ctx, saved);
    }
    if (!ok)
        return;
    if (rf.kind == OGLES2_RT_F16 || rf.kind == OGLES2_RT_F32) {
        *type = GL_FLOAT;
    } else if (rf.kind == OGLES2_RT_INT || rf.kind == OGLES2_RT_UINT) {
        *format = GL_RGBA_INTEGER;
        *type = rf.kind == OGLES2_RT_INT ? GL_INT : GL_UNSIGNED_INT;
    } else if (rf.bits == 10) { /* RGB10_A2 */
        *type = GL_UNSIGNED_INT_2_10_10_10_REV;
    }
}

static void read_pixels(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, void *pixels);

/* ES 3.0: from the read framebuffer (fbo.c, ogles2_fbo_read_begin). */
void ogles2_glReadPixels(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels)
{
    struct OGLES2Framebuffer *saved;
    if (!ogles2_context_valid(ctx))
        return;
    saved = ogles2_fbo_read_begin(ctx);
    read_pixels(ctx, x, y, width, height, format, type, pixels);
    ogles2_fbo_read_end(ctx, saved);
}

static void read_pixels(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, void *pixels)
{
    LONG x0, y0, x1, y1;
    ULONG stride, bpp, rgba_stride;
    UBYTE *rgba, *first;
    GLenum impl_f, impl_t;
    BOOL ok = TRUE, es3, wide;
    if (!ogles2_context_valid(ctx))
        return;
    es3 = ogles2_es3(ctx);
    if (!read_format(es3, format) || !read_type(es3, type)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* RGBA bytes for a fixed-point buffer (or a GLES2 half-float one),
     * EXT_read_format_bgra's, and ES 3.0's implementation pair of the
     * buffer read */
    ogles2_read_format(ctx, &impl_f, &impl_t);
    if (!(format == impl_f && type == impl_t) &&
        !(format == GL_RGBA && type == GL_UNSIGNED_BYTE && impl_t != GL_FLOAT && impl_f != GL_RGBA_INTEGER) &&
        !(format == GL_BGRA_EXT && impl_t != GL_FLOAT && impl_f != GL_RGBA_INTEGER &&
          (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT ||
           type == GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    wide = type == GL_FLOAT || type == GL_INT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_2_10_10_10_REV;
    bpp = type == GL_FLOAT || type == GL_INT || type == GL_UNSIGNED_INT ? 16
        : type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_2_10_10_10_REV ? 4 : 2;
    /* ES 3.0's GL_PACK_ROW_LENGTH and _SKIP_ROWS, _SKIP_PIXELS (4.3.1) */
    stride = ((ULONG)(ctx->pack.row_length ? ctx->pack.row_length : width) * bpp + ctx->pack.alignment - 1) &
             ~(ULONG)(ctx->pack.alignment - 1);
    /* ES 3.0's pixel pack buffer: `pixels` is an offset into it. */
    if (width && height) {
        BOOL bad;
        UBYTE *dst = ogles2_buffer_pixels(ctx, TRUE, (ULONG)pixels,
                                          (ULONG)(ctx->pack.skip_rows + height - 1) * stride +
                                              (ULONG)(ctx->pack.skip_pixels + width) * bpp, &bad);
        if (bad) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (dst)
            pixels = dst;
    }
    if (!has_surface(ctx) || !ogles2_fbo_read_ready(ctx) || !pixels || !ctx->display.bitmap)
        return;
    if (es3 && !ctx->draw_fb && ogles2_fbo_default_read_none(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION); /* ES 3.0 4.3.1: GL_READ_BUFFER is GL_NONE */
        return;
    }
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + width > (LONG)ctx->fb_w ? (LONG)ctx->fb_w : x + width;
    y1 = y + height > (LONG)ctx->fb_h ? (LONG)ctx->fb_h : y + height;
    if (x0 >= x1 || y0 >= y1)
        return;
    ogles2_job_flush(ctx, FALSE);
    ogles2_job_finish(ctx);
    first = (UBYTE *)pixels + (ULONG)(y0 - y + ctx->pack.skip_rows) * stride +
            (ULONG)(x0 - x + ctx->pack.skip_pixels) * bpp;
    if (wide) {
        /* 16-byte texels: straight into the application's memory, or (the
         * packed RGB10_A2) through a copy */
        LONG i, j;
        if (bpp == 16) {
            if (!ogles2_fbo_read_wide(ctx, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), first, stride))
                ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        rgba_stride = (ULONG)(x1 - x0) * 16;
        if (!(rgba = AllocVec(rgba_stride * (ULONG)(y1 - y0), MEMF_ANY))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (!ogles2_fbo_read_wide(ctx, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), rgba, rgba_stride))
            ogles2_error(ctx, GL_INVALID_OPERATION);
        else
            for (j = 0; j < y1 - y0; j++) {
                const GLfloat *s = (const GLfloat *)(rgba + (ULONG)j * rgba_stride);
                ULONG *d = (ULONG *)(first + (ULONG)j * stride);
                for (i = 0; i < x1 - x0; i++, s += 4) /* GL's packing: red in the low bits */
                    d[i] = (ULONG)(s[0] * 1023.0f + 0.5f) | (ULONG)(s[1] * 1023.0f + 0.5f) << 10 |
                           (ULONG)(s[2] * 1023.0f + 0.5f) << 20 | (ULONG)(s[3] * 3.0f + 0.5f) << 30;
            }
        FreeVec(rgba);
        return;
    }
    /* RGBA bytes: straight into the application's memory; the 16-bit
     * packings through a copy of the region. */
    if (bpp == 4) {
        rgba = first;
        rgba_stride = stride;
    } else {
        rgba_stride = (ULONG)(x1 - x0) * 4;
        if (!(rgba = AllocVec(rgba_stride * (ULONG)(y1 - y0), MEMF_ANY))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
    }
    if (ctx->draw_fb)
        ok = ogles2_fbo_read(ctx, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), rgba, rgba_stride);
    else
        ogles2_display_read(&ctx->display, (UWORD)x0, (UWORD)y0, (UWORD)(x1 - x0), (UWORD)(y1 - y0),
                            rgba, rgba_stride);
    if (!ok)
        ogles2_error(ctx, GL_INVALID_OPERATION); /* an object without a colour image */
    else if (format == GL_BGRA_EXT) {
        LONG i, j;
        for (j = 0; j < y1 - y0; j++) {
            UBYTE *s = rgba + (ULONG)j * rgba_stride, *d = first + (ULONG)j * stride;
            for (i = 0; i < x1 - x0; i++, s += 4) {
                UBYTE r = s[0], g = s[1], b = s[2], a = s[3];
                UWORD v;
                if (bpp == 4) { /* in place: B, G, R, A */
                    s[0] = b;
                    s[2] = r;
                    continue;
                }
                /* The reversed packings hold blue in the lowest bits (EXT_read_format_bgra). */
                if (type == GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT)
                    v = (UWORD)((b >> 4) | ((g >> 4) << 4) | ((r >> 4) << 8) | ((a >> 4) << 12));
                else
                    v = (UWORD)((b >> 3) | ((g >> 3) << 5) | ((r >> 3) << 10) | ((a >> 7) << 15));
                *(UWORD *)d = v; /* the 68k's own byte order, as GL's packed types are */
                d += 2;
            }
        }
    }
    if (bpp != 4)
        FreeVec(rgba);
}

void ogles2_glHint(struct OGLES2Context *ctx, GLenum target, GLenum mode)
{
    if (!ogles2_context_valid(ctx))
        return;
    if ((target != GL_GENERATE_MIPMAP_HINT && target != GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES) ||
        (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* glGenerateMipmap has one way, and the compiled shaders one way of
     * computing derivatives (OES_standard_derivatives): kept for the queries. */
    if (target == GL_GENERATE_MIPMAP_HINT)
        ctx->state.generate_mipmap_hint = mode;
    else
        ctx->state.derivative_hint = mode;
}

/* A clear of `mask` the tile buffer cannot do: confined by the scissor
 * box or by write masks (plan 7.7), or depth or stencil alone where both
 * are loaded (job.c: the tile buffer clears both or neither). Depth and
 * stencil are cleared together at the start of every tile; unless they
 * were loaded (kept from a mid-frame flush, or a framebuffer object's) they
 * hold nothing before a job's first draw, so clearing only one there is
 * still exact. */
static BOOL clear_partial(struct OGLES2Context *ctx, GLbitfield mask)
{
    const struct OGLES2State *s = &ctx->state;
    return ((s->caps & OGLES2_CAP_SCISSOR_TEST) &&
            (ctx->scissor[0] > 0 || ctx->scissor[1] > 0 ||
             ctx->scissor[0] + ctx->scissor[2] < (GLint)ctx->fb_w ||
             ctx->scissor[1] + ctx->scissor[3] < (GLint)ctx->fb_h)) ||
           ((mask & GL_COLOR_BUFFER_BIT) &&
            !(s->color_mask[0] && s->color_mask[1] && s->color_mask[2] && s->color_mask[3])) ||
           ((mask & GL_DEPTH_BUFFER_BIT) && !s->depth_mask) ||
           ((mask & GL_STENCIL_BUFFER_BIT) && (s->stencil_write_mask[0] & 0xff) != 0xff) ||
           ((ctx->draw_fb || ctx->zs_saved) && ctx->fb_stencil_bits && ctx->fb_depth_bits &&
            (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) &&
            (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
}

/* glClear and glClearBuffer*: `mask`'s buffers, the colour of the draw
 * buffers rts, with these values (cv floats' bits, or with is_int
 * integers). The tile buffer clears whole buffers at the start of the
 * job's render, before any draw; a partial clear, or one after a draw of
 * this job, is drawn as a rectangle instead (ogles2_draw_clear), but for
 * 32-bit float and integer targets, which its program cannot write: the
 * CPU fills their box once the jobs drawing into them have rendered. */
static void clear(struct OGLES2Context *ctx, GLbitfield mask, UBYTE rts, const ULONG *cv, BOOL is_int,
                  GLfloat depth, GLint stencil)
{
    GLfloat color[4];
    UBYTE cpu = 0;
    int k;
    if (!ctx->v3d || !ogles2_fb_ready(ctx))
        return;
    /* GL_RASTERIZER_DISCARD (ES 3.0 4.2.3) discards clears too, as Mesa does
     * (main/clear.c). */
    if (ctx->state.caps & OGLES2_CAP_RASTERIZER_DISCARD)
        return;
    /* EXT_draw_buffers: colour clears the draw buffers, none with GL_NONE. */
    rts &= ctx->fb_draw_mask;
    if (!rts)
        mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    if (!mask)
        return;
    if (!clear_partial(ctx, mask) && !(ogles2_job_has_draws(ctx) && ctx->job_fb == ctx->draw_fb)) {
        ogles2_job_clear(ctx, mask, rts, cv, is_int, depth, stencil);
        return;
    }
    for (k = 0; k < 4; k++) {
        union { ULONG u; GLfloat f; } x;
        x.u = cv[k];
        color[k] = is_int ? (GLfloat)(LONG)cv[k] : x.f;
        /* beside a 32-bit float or integer target every target: the
         * built-in clears for several targets would write it too, at the
         * wrong width (draw.c) */
        if ((mask & GL_COLOR_BUFFER_BIT) && (rts & (1 << k)) && ctx->fb_soft_blend &&
            ctx->fb_rt[k].kind != OGLES2_RT_NONE)
            cpu |= (UBYTE)(1 << k);
    }
    if (cpu) {
        LONG x0 = 0, y0 = 0, x1 = ctx->fb_w, y1 = ctx->fb_h;
        if (ctx->state.caps & OGLES2_CAP_SCISSOR_TEST) {
            if (ctx->scissor[0] > x0) x0 = ctx->scissor[0];
            if (ctx->scissor[1] > y0) y0 = ctx->scissor[1];
            if (ctx->scissor[0] + ctx->scissor[2] < x1) x1 = ctx->scissor[0] + ctx->scissor[2];
            if (ctx->scissor[1] + ctx->scissor[3] < y1) y1 = ctx->scissor[1] + ctx->scissor[3];
        }
        /* any open job: another framebuffer's may draw into the same image
         * (a texture attached to several; none open: nothing) */
        ogles2_job_flush(ctx, FALSE);
        ogles2_job_finish(ctx);
        for (k = 0; k < 4 && x0 < x1 && y0 < y1; k++)
            if (cpu & (1 << k))
                ogles2_fbo_fill(ctx, k, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), cv, is_int,
                                ctx->state.color_mask);
        rts &= (UBYTE)~cpu;
        if (!rts)
            mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    }
    if (mask)
        ogles2_draw_clear(ctx, mask, rts, color, depth, stencil);
}

void ogles2_glClear(struct OGLES2Context *ctx, GLbitfield mask)
{
    ULONG cv[4];
    int k;
    if (!ogles2_context_valid(ctx))
        return;
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < 4; k++) {
        union { ULONG u; GLfloat f; } x;
        x.f = ctx->state.clear_color[k];
        cv[k] = x.u;
    }
    clear(ctx, mask, 0x0F, cv, FALSE, ctx->state.clear_depth, ctx->state.clear_stencil);
}

/* ES 3.0's glClearBuffer* (4.2.3): one draw buffer's colour (floats, or
 * signed or unsigned integers, raw), the depth (clamped to [0, 1]), the
 * stencil, or both; GL_INVALID_ENUM for a buffer the entry does not take,
 * GL_INVALID_VALUE for a draw buffer out of range (depth and stencil: 0). */
static BOOL clear_buffer_ok(struct OGLES2Context *ctx, GLenum buffer, GLint drawbuffer, BOOL ok_buffer)
{
    if (!ok_buffer) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return FALSE;
    }
    if (buffer == GL_COLOR ? drawbuffer < 0 || drawbuffer >= OGLES2_MAX_DRAW_BUFFERS : drawbuffer != 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return FALSE;
    }
    return TRUE;
}

static GLfloat clamp_depth(GLfloat d)
{
    return d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
}

void ogles2_glClearBufferiv(struct OGLES2Context *ctx, GLenum buffer, GLint drawbuffer, const GLint *value)
{
    if (!ogles2_es3_entry(ctx) ||
        !clear_buffer_ok(ctx, buffer, drawbuffer, buffer == GL_COLOR || buffer == GL_STENCIL) || !value)
        return;
    if (buffer == GL_COLOR)
        clear(ctx, GL_COLOR_BUFFER_BIT, (UBYTE)(1 << drawbuffer), (const ULONG *)value, TRUE, 0.0f, 0);
    else
        clear(ctx, GL_STENCIL_BUFFER_BIT, 0, 0, FALSE, 0.0f, value[0]);
}

void ogles2_glClearBufferuiv(struct OGLES2Context *ctx, GLenum buffer, GLint drawbuffer, const GLuint *value)
{
    if (!ogles2_es3_entry(ctx) || !clear_buffer_ok(ctx, buffer, drawbuffer, buffer == GL_COLOR) || !value)
        return;
    clear(ctx, GL_COLOR_BUFFER_BIT, (UBYTE)(1 << drawbuffer), (const ULONG *)value, TRUE, 0.0f, 0);
}

void ogles2_glClearBufferfv(struct OGLES2Context *ctx, GLenum buffer, GLint drawbuffer, const GLfloat *value)
{
    if (!ogles2_es3_entry(ctx) ||
        !clear_buffer_ok(ctx, buffer, drawbuffer, buffer == GL_COLOR || buffer == GL_DEPTH) || !value)
        return;
    if (buffer == GL_COLOR)
        clear(ctx, GL_COLOR_BUFFER_BIT, (UBYTE)(1 << drawbuffer), (const ULONG *)value, FALSE, 0.0f, 0);
    else
        clear(ctx, GL_DEPTH_BUFFER_BIT, 0, 0, FALSE, clamp_depth(value[0]), 0);
}

void ogles2_glClearBufferfi(struct OGLES2Context *ctx, GLenum buffer, GLint drawbuffer, GLfloat depth,
                            GLint stencil)
{
    if (!ogles2_es3_entry(ctx) || !clear_buffer_ok(ctx, buffer, drawbuffer, buffer == GL_DEPTH_STENCIL))
        return;
    clear(ctx, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, 0, 0, FALSE, clamp_depth(depth), stencil);
}

void ogles2_glViewport(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* Clamped to the implementation maximum (GL_MAX_VIEWPORT_DIMS). */
    ctx->viewport[0] = x;
    ctx->viewport[1] = y;
    ctx->viewport[2] = width > OGLES2_MAX_VIEWPORT ? OGLES2_MAX_VIEWPORT : width;
    ctx->viewport[3] = height > OGLES2_MAX_VIEWPORT ? OGLES2_MAX_VIEWPORT : height;
    ctx->state_ver++;
}

void ogles2_glScissor(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->scissor[0] = x;
    ctx->scissor[1] = y;
    ctx->scissor[2] = width;
    ctx->scissor[3] = height;
    ctx->state_ver++;
}

/* Retarget a bitmap context (OGLES2_CCT_BITMAP): the open frame is rendered
 * into the old bitmap first; from then on the context renders into the new
 * one, its default framebuffer resized to it. As after an EGL surface
 * resize, the viewport and scissor stay as the application set them. */
void ogles2_aglSetBitmap(struct OGLES2Context *ctx, struct BitMap *bitmap)
{
    struct OGLES2Display old;
    if (!has_surface(ctx))
        return;
    if (bitmap == ctx->display.bitmap)
        return;
    ogles2_job_flush(ctx, TRUE);
    ogles2_job_finish(ctx);
    old = ctx->display;
    if (!ogles2_display_set_bitmap(&ctx->display, bitmap))
        return;
    if (ctx->display.width != old.width || ctx->display.height != old.height) {
        ogles2_job_free(ctx); /* the depth/stencil save is the old size */
        if (!ogles2_device_context_resize(ctx->v3d, ctx->display.width, ctx->display.height)) {
            ctx->display = old; /* keep rendering into the old bitmap */
            return;
        }
        ctx->width = ctx->display.width;
        ctx->height = ctx->display.height;
    }
    ogles2_fb_update(ctx);
    ogles2_log("aglSetBitmap: bitmap 0x%08lx, %ldx%ld", (ULONG)bitmap, ctx->width, ctx->height);
}

/* aglSetParams2(OGLES2_CCT_WINDOW): another window, or the same one
 * resized; the default framebuffer follows its size, as aglSetBitmap. */
static void set_window(struct OGLES2Context *ctx, struct Window *w)
{
    struct OGLES2Display old;
    if (!has_surface(ctx))
        return;
    ogles2_job_flush(ctx, TRUE);
    ogles2_job_finish(ctx);
    old = ctx->display;
    if (!ogles2_display_set_window(&ctx->display, w))
        return;
    if (ctx->display.window == old.window && ctx->display.bitmap == old.bitmap &&
        ctx->display.width == old.width && ctx->display.height == old.height)
        return; /* the same window at the same size (a screen's bitmap, unlike
                 * an offscreen one, can be the same for another window) */
    if (ctx->display.width != old.width || ctx->display.height != old.height) {
        ogles2_job_free(ctx); /* the depth/stencil save is the old size */
        if (!ogles2_device_context_resize(ctx->v3d, ctx->display.width, ctx->display.height)) {
            ogles2_display_drop(&ctx->display); /* the new bitmap; the old display stays */
            ctx->display = old;
            return;
        }
        ctx->width = ctx->display.width;
        ctx->height = ctx->display.height;
    }
    ogles2_display_drop(&old);
    ogles2_fb_update(ctx);
    ogles2_log("aglSetParams2: window 0x%08lx, %ldx%ld%s", (ULONG)w, ctx->width, ctx->height,
               (ULONG)(ctx->display.direct ? ", all of its screen (no copy)" : ""));
}

void ogles2_aglSetParams2(struct OGLES2Context *ctx, struct TagItem *tags)
{
    struct TagItem *state = tags, *t;
    if (!ogles2_context_valid(ctx))
        return;
    while ((t = NextTagItem(&state))) {
        switch (t->ti_Tag) {
        case OGLES2_CCT_VSYNC:
            ogles2_display_set_vsync(&ctx->display, t->ti_Data ? TRUE : FALSE);
            break;
        case OGLES2_CCT_WINDOW:
            set_window(ctx, (struct Window *)t->ti_Data);
            break;
        case OGLES2_CCT_GET_WIDTH:
            if (t->ti_Data)
                *(ULONG *)t->ti_Data = ctx->width;
            break;
        case OGLES2_CCT_GET_HEIGHT:
            if (t->ti_Data)
                *(ULONG *)t->ti_Data = ctx->height;
            break;
        case OGLES2_CCT_PISTORM_GET_WINDOW:
            if (t->ti_Data)
                *(struct Window **)t->ti_Data =
                    ctx->display.input_window ? ctx->display.input_window : ctx->display.window;
            break;
        default:
            /* Retargeting and resizing come later in M1. */
            ogles2_log("aglSetParams2: tag 0x%08lx = 0x%08lx not supported yet", t->ti_Tag,
                       t->ti_Data);
            break;
        }
    }
}
