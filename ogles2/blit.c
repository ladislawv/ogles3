/*
 * glBlitFramebuffer (ES 3.0 4.3.3; docs/GLES3_PLAN.md stage B item 11).
 *
 * Colour on the CPU, for every format and either filter: the source
 * rectangle of the read buffer is read back (the read framebuffer's
 * colour image, or the default framebuffer's bitmap) as RGBA floats or
 * integers, each destination pixel inside the scissor box takes its
 * sample (nearest, or the four texels around it for GL_LINEAR, clamped to
 * the read framebuffer), and every draw buffer's image is written again
 * (texture.c's RAM copy and GPU copy, or the bitmap). sRGB images are
 * decoded when read and encoded when written. Pixels whose sample lies
 * outside the read framebuffer are left as they are. The jobs that draw
 * into either framebuffer render first.
 *
 * Depth and stencil between renderbuffers of the same format, nearest, on
 * the CPU through their images (fbo.c, ogles2_fbo_blit_zs); the default
 * framebuffer's and depth textures are not blitted (logged once). Mesa's
 * GPU paths (TLB and TFU blits, a textured quad) come later.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"

/* An sRGB-encoded value in [0, 1] as linear (ES 3.0 3.8.16): x^2.4 is
 * x^2 times the fifth root of x^2, by Newton's method (no libm). */
static GLfloat srgb_linear(GLfloat c)
{
    GLfloat x, a, y = 1.0f;
    int n;
    if (c <= 0.04045f)
        return c / 12.92f;
    x = (c + 0.055f) / 1.055f;
    a = x * x;
    for (n = 0; n < 40; n++) {
        GLfloat y2 = y * y, y4 = y2 * y2;
        y -= (y4 * y - a) / (5.0f * y4);
    }
    return a * y;
}

/* The read buffer's box as 16-byte texels (floats, or with *ints 32-bit
 * integers): the read framebuffer's image, or the default framebuffer's
 * bitmap. FALSE: nothing to read. */
static BOOL read_raw(struct OGLES2Context *ctx, int x, int y, int w, int h, UBYTE *box, BOOL *ints)
{
    struct OGLES2RtFormat rf;
    ULONG row = (ULONG)w * 16;
    UBYTE *bytes;
    BOOL srgb = FALSE;
    int i, j;
    *ints = FALSE;
    if (ctx->draw_fb) {
        if (!ogles2_fbo_read_format(ctx, &rf))
            return FALSE;
        *ints = rf.kind == OGLES2_RT_INT || rf.kind == OGLES2_RT_UINT;
        srgb = rf.out == 0; /* V3D_OUTPUT_IMAGE_FORMAT_SRGB8ALPHA8 */
        if (ogles2_fbo_read_wide(ctx, x, y, w, h, box, row))
            return TRUE; /* floats or integers as stored */
    }
    /* RGBA bytes (an 8-bit image, the bitmap) at the box's end, turned into
     * floats in place from the start: float k covers bytes no later than
     * byte k, all read already */
    bytes = box + row * (ULONG)h - (ULONG)w * 4 * (ULONG)h;
    if (ctx->draw_fb) {
        if (!ogles2_fbo_read(ctx, x, y, w, h, bytes, (ULONG)w * 4))
            return FALSE;
    } else {
        ogles2_display_read(&ctx->display, (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, bytes, (ULONG)w * 4);
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w * 4; i++) {
            GLfloat v = (GLfloat)bytes[(ULONG)j * w * 4 + i] / 255.0f;
            if (srgb && (i & 3) != 3)
                v = srgb_linear(v);
            ((GLfloat *)box)[(ULONG)j * w * 4 + i] = v;
        }
    return TRUE;
}

/* read_raw's box, resolved from a multisampled read buffer (stage C item
 * 2): each pixel's four samples, a 2x2 square of the image (fbo.c), read
 * as floats (sRGB decoded) and averaged, as the tile buffer resolves in
 * its 8-bit or 16F type. Integer formats have no samples. */
static BOOL read_box(struct OGLES2Context *ctx, int x, int y, int w, int h, UBYTE *box, BOOL *ints)
{
    GLfloat *all, *out = (GLfloat *)box;
    ULONG row = (ULONG)w * 8; /* floats in a row of samples */
    BOOL ok;
    int i, j, c;
    if (!ctx->draw_fb || !ogles2_fbo_samples(ctx->draw_fb))
        return read_raw(ctx, x, y, w, h, box, ints);
    if (!(all = AllocVec((ULONG)w * (ULONG)h * 64, MEMF_ANY)))
        return FALSE;
    ok = read_raw(ctx, 2 * x, 2 * y, 2 * w, 2 * h, (UBYTE *)all, ints);
    for (j = 0; ok && j < h; j++)
        for (i = 0; i < w; i++)
            for (c = 0; c < 4; c++) {
                const GLfloat *s = all + (ULONG)2 * j * row + (ULONG)i * 8 + c;
                out[((ULONG)j * w + i) * 4 + c] = (s[0] + s[4] + s[row] + s[row + 4]) * 0.25f;
            }
    FreeVec(all);
    return ok;
}

/* The colour blit: src a box of the read buffer (sx, sy, sw, sh in the
 * read framebuffer, clamped), the destination rectangle (dx0..dx1,
 * dy0..dy1, flips allowed) mapped onto the source rectangle (sx0..sx1,
 * sy0..sy1), written where the scissor box and the draw framebuffer allow. */
static void blit_color(struct OGLES2Context *ctx, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                       GLint dx1, GLint dy1, BOOL linear)
{
    struct OGLES2Framebuffer *saved;
    LONG rx0, ry0, rx1, ry1, ox0, oy0, ox1, oy1, x, y;
    ULONG rw, rh, ow, oh;
    UBYTE *src = 0, *dst = 0, *mask = 0;
    BOOL ints = FALSE, ok;
    GLfloat scx = (GLfloat)(sx1 - sx0) / (GLfloat)(dx1 - dx0), scy = (GLfloat)(sy1 - sy0) / (GLfloat)(dy1 - dy0);
    int rt;

    /* the source: what the rectangle covers of the read framebuffer */
    saved = ogles2_fbo_read_begin(ctx);
    rx0 = sx0 < sx1 ? sx0 : sx1;
    rx1 = sx0 < sx1 ? sx1 : sx0;
    ry0 = sy0 < sy1 ? sy0 : sy1;
    ry1 = sy0 < sy1 ? sy1 : sy0;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > (LONG)ctx->fb_w) rx1 = ctx->fb_w;
    if (ry1 > (LONG)ctx->fb_h) ry1 = ctx->fb_h;
    ok = rx0 < rx1 && ry0 < ry1;
    rw = ok ? (ULONG)(rx1 - rx0) : 0;
    rh = ok ? (ULONG)(ry1 - ry0) : 0;
    if (ok && !(src = AllocVec(rw * rh * 16, MEMF_ANY))) {
        ogles2_fbo_read_end(ctx, saved);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    ok = ok && read_box(ctx, (int)rx0, (int)ry0, (int)rw, (int)rh, src, &ints);
    ogles2_fbo_read_end(ctx, saved);
    if (!ok) {
        if (src)
            FreeVec(src);
        return;
    }

    /* the destination: the rectangle within the draw framebuffer and the scissor box */
    ox0 = dx0 < dx1 ? dx0 : dx1;
    ox1 = dx0 < dx1 ? dx1 : dx0;
    oy0 = dy0 < dy1 ? dy0 : dy1;
    oy1 = dy0 < dy1 ? dy1 : dy0;
    if (ox0 < 0) ox0 = 0;
    if (oy0 < 0) oy0 = 0;
    if (ox1 > (LONG)ctx->fb_w) ox1 = ctx->fb_w;
    if (oy1 > (LONG)ctx->fb_h) oy1 = ctx->fb_h;
    if (ctx->state.caps & OGLES2_CAP_SCISSOR_TEST) {
        if (ctx->scissor[0] > ox0) ox0 = ctx->scissor[0];
        if (ctx->scissor[1] > oy0) oy0 = ctx->scissor[1];
        if (ctx->scissor[0] + ctx->scissor[2] < ox1) ox1 = ctx->scissor[0] + ctx->scissor[2];
        if (ctx->scissor[1] + ctx->scissor[3] < oy1) oy1 = ctx->scissor[1] + ctx->scissor[3];
    }
    if (ox0 >= ox1 || oy0 >= oy1) {
        FreeVec(src);
        return;
    }
    ow = (ULONG)(ox1 - ox0);
    oh = (ULONG)(oy1 - oy0);
    if (!(dst = AllocVec(ow * oh * 16, MEMF_ANY)) || !(mask = AllocVec(ow * oh, MEMF_ANY | MEMF_CLEAR))) {
        if (dst)
            FreeVec(dst);
        FreeVec(src);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (y = oy0; y < oy1; y++)
        for (x = ox0; x < ox1; x++) {
            /* the pixel's centre in the source (ES 3.0 4.3.3) */
            GLfloat u = (GLfloat)sx0 + ((GLfloat)x + 0.5f - (GLfloat)dx0) * scx;
            GLfloat v = (GLfloat)sy0 + ((GLfloat)y + 0.5f - (GLfloat)dy0) * scy;
            ULONG o = (ULONG)(y - oy0) * ow + (ULONG)(x - ox0);
            ULONG *d = (ULONG *)(dst + o * 16);
            LONG iu = (LONG)(u < 0.0f ? u - 1.0f : u), iv = (LONG)(v < 0.0f ? v - 1.0f : v);
            int c;
            if (iu < rx0 || iu >= rx1 || iv < ry0 || iv >= ry1)
                continue; /* outside the read framebuffer: left as it is */
            mask[o] = 1;
            if (!linear || ints) {
                memcpy(d, src + ((ULONG)(iv - ry0) * rw + (ULONG)(iu - rx0)) * 16, 16);
                continue;
            }
            {
                /* bilinear between the four texels around (u, v), clamped to the source box */
                GLfloat fu = u - 0.5f, fv = v - 0.5f, au, av;
                LONG u0 = (LONG)(fu < 0.0f ? fu - 1.0f : fu), v0 = (LONG)(fv < 0.0f ? fv - 1.0f : fv);
                LONG u1 = u0 + 1, v1 = v0 + 1;
                const GLfloat *t00, *t10, *t01, *t11;
                au = fu - (GLfloat)u0;
                av = fv - (GLfloat)v0;
                if (u0 < rx0) u0 = rx0;
                if (u1 >= rx1) u1 = rx1 - 1;
                if (v0 < ry0) v0 = ry0;
                if (v1 >= ry1) v1 = ry1 - 1;
                if (u0 > u1) u0 = u1;
                if (v0 > v1) v0 = v1;
                t00 = (const GLfloat *)(src + ((ULONG)(v0 - ry0) * rw + (ULONG)(u0 - rx0)) * 16);
                t10 = (const GLfloat *)(src + ((ULONG)(v0 - ry0) * rw + (ULONG)(u1 - rx0)) * 16);
                t01 = (const GLfloat *)(src + ((ULONG)(v1 - ry0) * rw + (ULONG)(u0 - rx0)) * 16);
                t11 = (const GLfloat *)(src + ((ULONG)(v1 - ry0) * rw + (ULONG)(u1 - rx0)) * 16);
                for (c = 0; c < 4; c++)
                    ((GLfloat *)d)[c] = (t00[c] * (1.0f - au) + t10[c] * au) * (1.0f - av) +
                                        (t01[c] * (1.0f - au) + t11[c] * au) * av;
            }
        }
    FreeVec(src);
    /* every draw buffer (the default framebuffer: its one) */
    for (rt = 0; rt < OGLES2_MAX_DRAW_BUFFERS; rt++)
        if (ctx->fb_draw_mask & (1 << rt))
            ogles2_fbo_write(ctx, rt, (int)ox0, (int)oy0, (int)ow, (int)oh, dst, ow * 16, mask);
    FreeVec(mask);
    FreeVec(dst);
}

/* The destination box: the rectangle within the draw framebuffer and the
 * scissor box; FALSE: empty. */
static BOOL dst_box(struct OGLES2Context *ctx, GLint dx0, GLint dy0, GLint dx1, GLint dy1, LONG *b)
{
    b[0] = dx0 < dx1 ? dx0 : dx1;
    b[2] = dx0 < dx1 ? dx1 : dx0;
    b[1] = dy0 < dy1 ? dy0 : dy1;
    b[3] = dy0 < dy1 ? dy1 : dy0;
    if (b[0] < 0) b[0] = 0;
    if (b[1] < 0) b[1] = 0;
    if (b[2] > (LONG)ctx->fb_w) b[2] = ctx->fb_w;
    if (b[3] > (LONG)ctx->fb_h) b[3] = ctx->fb_h;
    if (ctx->state.caps & OGLES2_CAP_SCISSOR_TEST) {
        if (ctx->scissor[0] > b[0]) b[0] = ctx->scissor[0];
        if (ctx->scissor[1] > b[1]) b[1] = ctx->scissor[1];
        if (ctx->scissor[0] + ctx->scissor[2] < b[2]) b[2] = ctx->scissor[0] + ctx->scissor[2];
        if (ctx->scissor[1] + ctx->scissor[3] < b[3]) b[3] = ctx->scissor[1] + ctx->scissor[3];
    }
    return b[0] < b[2] && b[1] < b[3];
}

/* A colour image's kind for the format checks (integers against the
 * rest): the read buffer's, or draw buffer rt's. */
static UBYTE kind_of(struct OGLES2Context *ctx, int rt)
{
    if (rt < 0) {
        struct OGLES2RtFormat rf;
        struct OGLES2Framebuffer *saved = ogles2_fbo_read_begin(ctx);
        UBYTE k = !ctx->draw_fb ? OGLES2_RT_UNORM : ogles2_fbo_read_format(ctx, &rf) ? rf.kind : OGLES2_RT_NONE;
        ogles2_fbo_read_end(ctx, saved);
        return k;
    }
    return ctx->fb_rt[rt].kind;
}

void ogles2_glBlitFramebuffer(struct OGLES2Context *ctx, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
{
    struct OGLES2Framebuffer *saved;
    UBYTE rk;
    int rt;
    if (!ogles2_es3_entry(ctx))
        return;
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (filter != GL_NEAREST && filter != GL_LINEAR) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && filter == GL_LINEAR) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* both framebuffers complete (GL_INVALID_FRAMEBUFFER_OPERATION) */
    saved = ogles2_fbo_read_begin(ctx);
    if (!ogles2_fb_ready(ctx)) {
        ogles2_fbo_read_end(ctx, saved);
        return;
    }
    ogles2_fbo_read_end(ctx, saved);
    if (!ogles2_fb_ready(ctx))
        return;
    /* multisampled renderbuffers: never drawn into; read into the same
     * format and rectangle only (a resolve) */
    if (ogles2_fbo_samples(ctx->draw_fb) ||
        (ogles2_fbo_samples(ctx->read_fb) &&
         (srcX0 != dstX0 || srcY0 != dstY0 || srcX1 != dstX1 || srcY1 != dstY1 ||
          ((mask & GL_COLOR_BUFFER_BIT) && !ogles2_fbo_blit_same_format(ctx, ctx->fb_draw_mask))))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* colour: integers to integers only, and never filtered */
    if (mask & GL_COLOR_BUFFER_BIT) {
        rk = kind_of(ctx, -1);
        for (rt = 0; rt < OGLES2_MAX_DRAW_BUFFERS && rk != OGLES2_RT_NONE; rt++) {
            UBYTE dk;
            BOOL ri = rk == OGLES2_RT_INT || rk == OGLES2_RT_UINT, di;
            if (!(ctx->fb_draw_mask & (1 << rt)))
                continue;
            dk = kind_of(ctx, rt);
            di = dk == OGLES2_RT_INT || dk == OGLES2_RT_UINT;
            if (ri != di || (ri && dk != rk) || (ri && filter == GL_LINEAR)) {
                ogles2_error(ctx, GL_INVALID_OPERATION);
                return;
            }
        }
        if (rk == OGLES2_RT_NONE)
            mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT; /* no read buffer: nothing */
    }
    /* depth and stencil: the same format both sides (ES 3.0 4.3.3); a
     * side without one blits nothing */
    for (rt = 0; rt < 2; rt++) {
        GLbitfield bit = rt ? GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
        GLenum rf, df;
        if (!(mask & bit))
            continue;
        rf = ogles2_fbo_zs_format(ctx, ctx->read_fb, rt);
        df = ogles2_fbo_zs_format(ctx, ctx->draw_fb, rt);
        if (rf && df && rf != df) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (!rf || !df)
            mask &= ~bit;
    }
    if (srcX0 == srcX1 || srcY0 == srcY1 || dstX0 == dstX1 || dstY0 == dstY1)
        return;
    /* the jobs that draw into either framebuffer render first */
    ogles2_job_flush(ctx, FALSE);
    ogles2_job_finish(ctx);
    if (mask & GL_COLOR_BUFFER_BIT)
        blit_color(ctx, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, filter == GL_LINEAR);
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        LONG b[4];
        if (dst_box(ctx, dstX0, dstY0, dstX1, dstY1, b))
            for (rt = 0; rt < 2; rt++)
                if (mask & (rt ? GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT))
                    ogles2_fbo_blit_zs(ctx, rt, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, b[0], b[1],
                                       b[2], b[3]);
    }
}
