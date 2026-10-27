/*
 * Framebuffer and renderbuffer objects (plan section 7.7, M5): names,
 * binding, attachments, completeness (GLES 2.0 4.4.5) and queries, and what
 * a job needs to render into one (ogles2_fbo_target) or glReadPixels to read
 * from one.
 *
 * - Colour images: a texture level (or cube face), or a colour renderbuffer,
 *   whose storage is a texture of its own (texture.c, ogles2_texture_hidden).
 *   The job stores straight into the level's own layout (Mesa's
 *   store_general), so the result is sampled as it is; the texture's RAM
 *   copy is read back only when something needs it (texture.c, sync).
 * - Depth and stencil: renderbuffers, DEPTH_COMPONENT16, STENCIL_INDEX8 and,
 *   OES_packed_depth_stencil, DEPTH24_STENCIL8; ES 3.0's DEPTH_COMPONENT24,
 *   DEPTH_COMPONENT32F and DEPTH32F_STENCIL8 (its stencil an S8 image of its
 *   own, as Mesa's separate stencil: the tile buffer has no packed 32F
 *   depth with stencil). Only the GPU reads them, so
 *   they are laid out as the GPU likes, UIF without XOR, padded as Mesa pads
 *   a UIF level (job.c's depth save does the same). Every job stores them and
 *   the next loads them. And depth textures (OES_depth_texture; a
 *   DEPTH_STENCIL_OES one for both): the job stores into the level's own
 *   layout, as for colour, and the TMU samples it from there.
 * - No Y flip: GL's row 0 is the image's row 0, which is what sampling the
 *   texture later expects (plan 7.11).
 * - Jobs, v1: one open job per context; drawing into another framebuffer
 *   renders the open one first (job.c, ogles2_job_begin).
 * - Deleting a texture or renderbuffer detaches it from the deleting
 *   context's bound framebuffer only (GLES 2.0 4.4.3); attached to another,
 *   it lives on without its name until its last attachment goes (both count
 *   the attachment points that refer to them).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_hw.h"

#ifndef GL_RGB8_OES
#define GL_RGB8_OES  0x8051 /* OES_rgb8_rgba8 */
#define GL_RGBA8_OES 0x8058
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT  0x80E1 /* EXT_texture_format_BGRA8888 */
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif
#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A /* EXT_color_buffer_half_float */
#define GL_RGB16F_EXT  0x881B
#endif
#ifndef GL_R16F_EXT
#define GL_R16F_EXT  0x822D
#define GL_RG16F_EXT 0x822F
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT 0x8211
#define GL_UNSIGNED_NORMALIZED_EXT                   0x8C17
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0 /* OES_packed_depth_stencil */
#endif

/* Attachment points: colour 0..3 (EXT_draw_buffers, NV_fbo_color_attachments),
 * then depth and stencil. */
#define ATT_COLOR   0
#define ATT_DEPTH   OGLES2_MAX_DRAW_BUFFERS
#define ATT_STENCIL (OGLES2_MAX_DRAW_BUFFERS + 1)
#define NATT        (OGLES2_MAX_DRAW_BUFFERS + 2)

struct OGLES2Renderbuffer
{
    GLuint name;
    BOOL bound_once;
    GLenum format;              /* GL_RGBA4 until storage is given (GLES 2.0 table 6.23) */
    UWORD w, h;
    struct OGLES2Texture *color; /* a colour format's storage */
    v3d_mem mem;                /* a depth or stencil format's, GPU only */
    ULONG base, ub, bytes;      /* 4096-aligned image; padded height in UIF blocks; size */
    v3d_mem smem;               /* DEPTH32F_STENCIL8's stencil, S8 */
    ULONG sbase, sub, sbytes;
    UBYTE cpp;
    /* glRenderbufferStorageMultisample's: 0, or 4 for any count asked of 1
     * to 4. Multisampled storage holds all four samples, each pixel a 2x2
     * square of them (Mesa's msaa_scale, v3d_resource.c): twice w and h. */
    UBYTE samples;
    UWORD refs;                 /* framebuffer attachment points that refer to it */
    BOOL orphan;                /* deleted while attached: freed with the last of them */
};

struct OGLES2Attachment
{
    GLenum type;                /* GL_NONE, GL_TEXTURE, GL_RENDERBUFFER */
    struct OGLES2Texture *tex;
    UWORD face;                 /* a cube map's face, a 2D array's layer, a 3D texture's slice */
    UBYTE level;
    struct OGLES2Renderbuffer *rb;
};

struct OGLES2Framebuffer
{
    GLuint name;
    BOOL bound_once;
    UBYTE discarded;            /* OGLES2_DISCARD_*: undefined, the next job need not load them */
    struct OGLES2Attachment att[NATT];
    GLenum draw_buf[OGLES2_MAX_DRAW_BUFFERS]; /* glDrawBuffersEXT: COLOR_ATTACHMENTi or NONE */
    v3d_mem pool_alloc, pool_state; /* tile pools of its own when bigger than the context's */
    ULONG pool_tiles;
    GLenum read_buf;            /* ES 3.0's glReadBuffer: COLOR_ATTACHMENTi or NONE */
};

struct OGLES2FboState
{
    struct OGLES2Framebuffer **fbs;  /* the context's own (GLES 2.0: not shared) */
    GLuint nfbs;
    struct OGLES2Share *sh;          /* the renderbuffers: sh->rbs, sh->nrbs (share.c) */
    struct OGLES2Renderbuffer *rb_bound;
    BOOL warned_incomplete;
    struct OGLES2FarName *far_fb;    /* fbs' names past OGLES2_NEAR_NAMES (share.c) */
    struct OGLES2Framebuffer *copy_fb; /* glCopyTex*'s target on the GPU (copy.c): never named */
    BOOL default_read_none;          /* the default framebuffer's glReadBuffer is GL_NONE (else GL_BACK) */
};

static struct OGLES2FboState *fs_get(struct OGLES2Context *ctx)
{
    if (!ctx->fbo && (ctx->fbo = AllocVec(sizeof(struct OGLES2FboState), MEMF_ANY | MEMF_CLEAR)))
        ctx->fbo->sh = ctx->share;
    return ctx->fbo;
}

/* A framebuffer target's bindings: GL_FRAMEBUFFER both, ES 3.0's
 * GL_DRAW_FRAMEBUFFER and GL_READ_FRAMEBUFFER one each; 0: not a target. */
#define FB_DRAW 1
#define FB_READ 2
static int fb_target(struct OGLES2Context *ctx, GLenum target)
{
    if (target == GL_FRAMEBUFFER)
        return FB_DRAW | FB_READ;
    if (ogles2_es3(ctx) && target == GL_DRAW_FRAMEBUFFER)
        return FB_DRAW;
    if (ogles2_es3(ctx) && target == GL_READ_FRAMEBUFFER)
        return FB_READ;
    return 0;
}

/* The framebuffer a target's attachments are changed or asked in (the
 * draw framebuffer for GL_FRAMEBUFFER); NULL: the default one. */
static struct OGLES2Framebuffer *target_fb(struct OGLES2Context *ctx, int which)
{
    return which == FB_READ ? ctx->read_fb : ctx->draw_fb;
}

/* Makes `name` an index of a name table of `*n` pointers. */
static BOOL table_room(void ***tab, GLuint *n, GLuint name)
{
    GLuint nn, j;
    void **t;
    if (name < *n)
        return TRUE;
    nn = *n ? *n : 16;
    while (nn <= name)
        nn *= 2;
    if (!(t = AllocVec(nn * sizeof(void *), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < *n; j++)
        t[j] = (*tab)[j];
    if (*tab)
        FreeVec(*tab);
    *tab = t;
    *n = nn;
    return TRUE;
}

static struct OGLES2Framebuffer *fb_by_name(struct OGLES2FboState *fs, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&fs->far_fb, name, FALSE) : OGLES2_NO_SLOT;
    return slot < fs->nfbs ? fs->fbs[slot] : 0;
}

static struct OGLES2Renderbuffer *rb_by_name(struct OGLES2FboState *fs, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&fs->sh->far_rb, name, FALSE) : OGLES2_NO_SLOT;
    return slot < fs->sh->nrbs ? fs->sh->rbs[slot] : 0;
}

/* ---- renderbuffer storage ---- */

static void rb_free_storage(struct OGLES2Context *ctx, struct OGLES2Renderbuffer *rb)
{
    if (rb->color)
        ogles2_texture_hidden_free(ctx, rb->color);
    rb->color = 0;
    if (rb->mem.hostptr)
        v3d_mem_free(ogles2_device(), &rb->mem);
    if (rb->smem.hostptr)
        v3d_mem_free(ogles2_device(), &rb->smem);
    memset(&rb->mem, 0, sizeof(rb->mem));
    memset(&rb->smem, 0, sizeof(rb->smem));
    rb->base = rb->ub = rb->bytes = 0;
    rb->sbase = rb->sub = rb->sbytes = 0;
}

static BOOL is_depth(GLenum f)
{
    return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH24_STENCIL8_OES || f == GL_DEPTH_COMPONENT24 ||
           f == GL_DEPTH_COMPONENT32F || f == GL_DEPTH32F_STENCIL8;
}

static BOOL is_stencil(GLenum f)
{
    return f == GL_STENCIL_INDEX8 || f == GL_DEPTH24_STENCIL8_OES || f == GL_DEPTH32F_STENCIL8;
}

/* ES 3.0's colour-renderable sized formats (table 3.13), with
 * EXT_color_buffer_float's floats: stored as glTexStorage2D stores them
 * (texture.c, ogles2_texture_hidden_sized). RGB integer formats are not. */
static BOOL es3_color(GLenum f)
{
    switch (f) {
    case GL_R8: case GL_RG8: case GL_RGB8: case GL_RGBA8: case GL_SRGB8_ALPHA8: case GL_RGB10_A2:
    case GL_RGB10_A2UI: case GL_RGBA4: case GL_RGB5_A1: case GL_RGB565:
    case GL_R8I: case GL_R8UI: case GL_R16I: case GL_R16UI: case GL_R32I: case GL_R32UI:
    case GL_RG8I: case GL_RG8UI: case GL_RG16I: case GL_RG16UI: case GL_RG32I: case GL_RG32UI:
    case GL_RGBA8I: case GL_RGBA8UI: case GL_RGBA16I: case GL_RGBA16UI: case GL_RGBA32I: case GL_RGBA32UI:
    case GL_R16F: case GL_RG16F: case GL_RGBA16F: case GL_R32F: case GL_RG32F: case GL_RGBA32F:
    case GL_R11F_G11F_B10F:
        return TRUE;
    default:
        return FALSE;
    }
}

/* EXT_color_buffer_half_float's renderbuffer formats: each stored as an
 * RGBA half-float texture, the components the format lacks masked when
 * read (ogles2_fbo_read). */
static BOOL is_half(GLenum f)
{
    return f == GL_RGBA16F_EXT || f == GL_RGB16F_EXT || f == GL_RG16F_EXT || f == GL_R16F_EXT;
}

/* GLES2's colour renderbuffer formats: BGRA_EXT and BGRA8_EXT
 * (EXT_texture_format_BGRA8888) are kept as RGBA8; the 16F ones
 * (EXT_color_buffer_half_float) as RGBA16F. */
static BOOL gles2_color(GLenum f)
{
    return f == GL_RGBA4 || f == GL_RGB5_A1 || f == GL_RGB565 || f == GL_RGB8_OES || f == GL_RGBA8_OES ||
           f == GL_BGRA_EXT || f == GL_BGRA8_EXT || is_half(f);
}

static BOOL is_color(GLenum f)
{
    return gles2_color(f) || es3_color(f);
}

/* A GLES2 context's renderbuffer formats (the others: GL_INVALID_ENUM). */
static BOOL gles2_format(GLenum f)
{
    return gles2_color(f) || f == GL_DEPTH_COMPONENT16 || f == GL_STENCIL_INDEX8 || f == GL_DEPTH24_STENCIL8_OES;
}

/* Depth and stencil: UIF without XOR, Mesa's padding for a UIF level
 * (also DEPTH32F_STENCIL8 textures' stencil, texture.c). */
BOOL ogles2_fbo_zs_alloc(UWORD w, UWORD h, ULONG cpp, v3d_mem *mem, ULONG *base, ULONG *ub, ULONG *bytes)
{
    ULONG ub_w = 2 * v3d_utile_width((int)cpp), ub_h = 2 * v3d_utile_height((int)cpp);
    ULONG pw = (w + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ULONG ph = (h + ub_h - 1) / ub_h * ub_h;
    if (v3d_mem_alloc(ogles2_device(), mem, pw * ph * cpp + 4096) < 0)
        return FALSE;
    *base = ((ULONG)mem->hostptr + 4095) & ~4095UL;
    *ub = ph / ub_h;
    *bytes = pw * ph * cpp;
    return TRUE;
}

/* The storage's width and height: twice the size's when multisampled. */
#define RB_SW(rb) ((UWORD)((rb)->w << ((rb)->samples ? 1 : 0)))
#define RB_SH(rb) ((UWORD)((rb)->h << ((rb)->samples ? 1 : 0)))

static BOOL rb_zs_storage(struct OGLES2Renderbuffer *rb)
{
    ULONG cpp = rb->format == GL_DEPTH_COMPONENT16 ? 2 : rb->format == GL_STENCIL_INDEX8 ? 1 : 4;
    if (!ogles2_fbo_zs_alloc(RB_SW(rb), RB_SH(rb), cpp, &rb->mem, &rb->base, &rb->ub, &rb->bytes))
        return FALSE;
    rb->cpp = (UBYTE)cpp;
    if (rb->format == GL_DEPTH32F_STENCIL8 &&
        !ogles2_fbo_zs_alloc(RB_SW(rb), RB_SH(rb), 1, &rb->smem, &rb->sbase, &rb->sub, &rb->sbytes))
        return FALSE; /* rb_free_storage frees the depth */
    return TRUE;
}

/* ---- the job of the framebuffer an attachment change touches ---- */

/* The open job draws into fb: render it before fb changes; and a running
 * render into an object finishes first (M6 overlap). */
static void before_fb_change(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb)
{
    if (ctx->v3d && ctx->v3d->frame_active && ctx->job_fb == fb && fb)
        ogles2_job_flush(ctx, FALSE);
    if (ctx->pend && ctx->pend_fb)
        ogles2_job_finish(ctx);
}

/* The open job draws into a framebuffer that uses rb. */
static void before_rb_change(struct OGLES2Context *ctx, struct OGLES2Renderbuffer *rb)
{
    struct OGLES2Framebuffer *fb = ctx->job_fb;
    int a;
    if (ctx->pend && ctx->pend_fb)
        ogles2_job_finish(ctx);
    if (!ctx->v3d || !ctx->v3d->frame_active || !fb)
        return;
    for (a = 0; a < NATT; a++)
        if (fb->att[a].type == GL_RENDERBUFFER && fb->att[a].rb == rb) {
            ogles2_job_flush(ctx, FALSE);
            ogles2_job_finish(ctx);
            return;
        }
}

/* fb draws into texture t: a colour image (a renderbuffer's own
 * included), or a depth texture. */
static BOOL draws_into(const struct OGLES2Framebuffer *fb, const struct OGLES2Texture *t)
{
    int a;
    if (!fb)
        return FALSE;
    for (a = 0; a < NATT; a++)
        if ((fb->att[a].type == GL_TEXTURE && fb->att[a].tex == t) ||
            (a < ATT_DEPTH && fb->att[a].type == GL_RENDERBUFFER && fb->att[a].rb && fb->att[a].rb->color == t))
            return TRUE;
    return FALSE;
}

void ogles2_fbo_texture_changing(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    if (ctx->v3d && ctx->v3d->frame_active && draws_into(ctx->job_fb, t))
        ogles2_job_flush(ctx, FALSE);
    if (ctx->pend && draws_into(ctx->pend_fb, t)) /* the running render writes it */
        ogles2_job_finish(ctx);
}

/* A deleted renderbuffer's memory goes: the jobs that use it rendered first
 * (`now`: at aglDestroyContext, they are finished). */
static void rb_destroy(struct OGLES2Context *ctx, struct OGLES2Renderbuffer *rb, BOOL now)
{
    struct OGLES2Context *m;
    if (!now)
        for (m = ctx->share->first; m; m = m->share_next)
            before_rb_change(m, rb);
    rb_free_storage(ctx, rb);
    FreeVec(rb);
}

/* An attachment point lets go of its image; an orphan goes with its last. */
static void att_clear(struct OGLES2Context *ctx, struct OGLES2Attachment *at, BOOL now)
{
    if (at->type == GL_TEXTURE && at->tex) {
        ogles2_texture_detach(ctx, at->tex, now);
    } else if (at->type == GL_RENDERBUFFER && at->rb) {
        struct OGLES2Renderbuffer *rb = at->rb;
        if (rb->refs)
            rb->refs--;
        if (!rb->refs && rb->orphan)
            rb_destroy(ctx, rb, now);
    }
    memset(at, 0, sizeof(*at));
}

/* glDeleteTextures: detached from the bound framebuffer, which renders first. */
void ogles2_fbo_texture_deleted(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    struct OGLES2Framebuffer *fbs[2];
    int a, k;
    if (!ctx->fbo)
        return;
    ogles2_fbo_texture_changing(ctx, t);
    fbs[0] = ctx->draw_fb;
    fbs[1] = ctx->read_fb != ctx->draw_fb ? ctx->read_fb : 0; /* ES 3.0: both bindings */
    for (k = 0; k < 2; k++)
        if (fbs[k])
            for (a = 0; a < NATT; a++)
                if (fbs[k]->att[a].type == GL_TEXTURE && fbs[k]->att[a].tex == t) {
                    before_fb_change(ctx, fbs[k]);
                    att_clear(ctx, &fbs[k]->att[a], FALSE);
                }
    ogles2_fb_update(ctx);
}

static GLenum status_of(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb);

/* glCopyTexImage2D and glCopyTexSubImage2D on the GPU (copy.c): an object
 * of the context's own, never named or bound by the application, its colour
 * attachment 0 the level the copy draws into. The attachment holds the
 * texture, as any does, until a copy into another level or
 * aglDestroyContext; the job or render of a copy into the old one is
 * rendered first. */
struct OGLES2Framebuffer *ogles2_fbo_copy_target(struct OGLES2Context *ctx, struct OGLES2Texture *t, int face,
                                                 int level)
{
    struct OGLES2FboState *fs = fs_get(ctx);
    struct OGLES2Framebuffer *fb;
    struct OGLES2Attachment *at;
    if (!fs)
        return 0;
    if (!(fb = fs->copy_fb)) {
        if (!(fb = AllocVec(sizeof(*fb), MEMF_ANY | MEMF_CLEAR)))
            return 0;
        fb->draw_buf[0] = GL_COLOR_ATTACHMENT0;
        fb->read_buf = GL_COLOR_ATTACHMENT0;
        fs->copy_fb = fb;
    }
    at = &fb->att[ATT_COLOR];
    if (at->type == GL_TEXTURE && at->tex == t && at->face == face && at->level == level)
        return status_of(ctx, fb) == GL_FRAMEBUFFER_COMPLETE ? fb : 0;
    before_fb_change(ctx, fb);
    ogles2_texture_attach(t); /* first: the same texture again must not free an orphan */
    att_clear(ctx, at, FALSE);
    at->type = GL_TEXTURE;
    at->tex = t;
    at->face = (UBYTE)face;
    at->level = (UBYTE)level;
    return status_of(ctx, fb) == GL_FRAMEBUFFER_COMPLETE ? fb : 0; /* no GL error: the CPU copies then */
}

/* The bound framebuffer's colour image for glCopyTex* on the GPU (copy.c):
 * a texture level or a colour renderbuffer's own texture; FALSE without. */
/* The colour attachment reads come from (ES 3.0's glReadBuffer; GLES2: 0); NULL for GL_NONE. */
static const struct OGLES2Attachment *read_attachment(const struct OGLES2Framebuffer *fb)
{
    if (fb->read_buf < GL_COLOR_ATTACHMENT0 || fb->read_buf >= GL_COLOR_ATTACHMENT0 + OGLES2_MAX_DRAW_BUFFERS)
        return 0;
    return &fb->att[ATT_COLOR + (fb->read_buf - GL_COLOR_ATTACHMENT0)];
}

BOOL ogles2_fbo_color_image(struct OGLES2Context *ctx, struct OGLES2Texture **t, int *face, int *level)
{
    const struct OGLES2Attachment *c;
    if (!ctx->draw_fb || !(c = read_attachment(ctx->draw_fb)))
        return FALSE;
    if (c->type == GL_TEXTURE) {
        *t = c->tex;
        *face = c->face;
        *level = c->level;
        return TRUE;
    }
    if (c->type == GL_RENDERBUFFER && c->rb->color) {
        *t = c->rb->color;
        *face = *level = 0;
        return TRUE;
    }
    return FALSE;
}

/* ---- names and binding ---- */

void ogles2_glGenFramebuffers(struct OGLES2Context *ctx, GLsizei n, GLuint *framebuffers)
{
    struct OGLES2FboState *fs;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES2Framebuffer *fb;
        while (i < fs->nfbs && fs->fbs[i])
            i++;
        if (i >= OGLES2_NEAR_NAMES || !table_room((void ***)&fs->fbs, &fs->nfbs, i) ||
            !(fb = AllocVec(sizeof(*fb), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        fb->name = i;
        fb->draw_buf[0] = GL_COLOR_ATTACHMENT0;
        fb->read_buf = GL_COLOR_ATTACHMENT0;
        fs->fbs[i] = fb;
        if (framebuffers)
            framebuffers[k] = i;
    }
}

void ogles2_glGenRenderbuffers(struct OGLES2Context *ctx, GLsizei n, GLuint *renderbuffers)
{
    struct OGLES2FboState *fs;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES2Renderbuffer *rb;
        while (i < fs->sh->nrbs && fs->sh->rbs[i])
            i++;
        if (i >= OGLES2_NEAR_NAMES || !table_room((void ***)&fs->sh->rbs, &fs->sh->nrbs, i) ||
            !(rb = AllocVec(sizeof(*rb), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        rb->name = i;
        rb->format = GL_RGBA4;
        fs->sh->rbs[i] = rb;
        if (renderbuffers)
            renderbuffers[k] = i;
    }
}

static void fb_free(struct OGLES2Framebuffer *fb)
{
    if (fb->pool_alloc.hostptr)
        v3d_mem_free(ogles2_device(), &fb->pool_alloc);
    if (fb->pool_state.hostptr)
        v3d_mem_free(ogles2_device(), &fb->pool_state);
    FreeVec(fb);
}

void ogles2_glBindFramebuffer(struct OGLES2Context *ctx, GLenum target, GLuint framebuffer)
{
    struct OGLES2FboState *fs;
    struct OGLES2Framebuffer *fb = 0;
    int which;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(which = fb_target(ctx, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (framebuffer) {
        /* GLES2 lets a program bind a name glGenFramebuffers did not return
         * (and ES 3.0: Mesa's allow_user_names for ES contexts). */
        if (!(fb = fb_by_name(fs, framebuffer))) {
            GLuint slot = ogles2_name_slot(&fs->far_fb, framebuffer, TRUE);
            if (slot == OGLES2_NO_SLOT || !table_room((void ***)&fs->fbs, &fs->nfbs, slot) ||
                !(fb = AllocVec(sizeof(*fb), MEMF_ANY | MEMF_CLEAR))) {
                ogles2_name_forget(&fs->far_fb, framebuffer);
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            fb->name = framebuffer;
            fb->draw_buf[0] = GL_COLOR_ATTACHMENT0;
            fb->read_buf = GL_COLOR_ATTACHMENT0;
            fs->fbs[slot] = fb;
        }
        fb->bound_once = TRUE;
    }
    /* The open job is rendered when something is drawn into the other
     * framebuffer (ogles2_job_begin), not here: binding back and forth
     * without drawing costs nothing. */
    if (which & FB_DRAW)
        ctx->draw_fb = fb;
    if (which & FB_READ)
        ctx->read_fb = fb;
    ogles2_fb_update(ctx);
}

void ogles2_glBindRenderbuffer(struct OGLES2Context *ctx, GLenum target, GLuint renderbuffer)
{
    struct OGLES2FboState *fs;
    struct OGLES2Renderbuffer *rb = 0;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_RENDERBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(fs = fs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (renderbuffer && !(rb = rb_by_name(fs, renderbuffer))) {
        GLuint slot = ogles2_name_slot(&fs->sh->far_rb, renderbuffer, TRUE);
        if (slot == OGLES2_NO_SLOT || !table_room((void ***)&fs->sh->rbs, &fs->sh->nrbs, slot) ||
            !(rb = AllocVec(sizeof(*rb), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_name_forget(&fs->sh->far_rb, renderbuffer);
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        rb->name = renderbuffer;
        rb->format = GL_RGBA4;
        fs->sh->rbs[slot] = rb;
    }
    if (rb)
        rb->bound_once = TRUE;
    fs->rb_bound = rb;
}

void ogles2_glDeleteFramebuffers(struct OGLES2Context *ctx, GLsizei n, const GLuint *framebuffers)
{
    struct OGLES2FboState *fs;
    GLsizei k;
    int a;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = ctx->fbo) || !framebuffers)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Framebuffer *fb = fb_by_name(fs, framebuffers[k]);
        if (!fb)
            continue;
        before_fb_change(ctx, fb); /* what was drawn into it is rendered */
        if (ctx->job_fb == fb)
            ctx->job_fb = 0;
        if (ctx->draw_fb == fb) {   /* GLES 2.0 4.4.1: as if 0 were bound */
            ctx->draw_fb = 0;
            ogles2_fb_update(ctx);
        }
        if (ctx->read_fb == fb)
            ctx->read_fb = 0;
        for (a = 0; a < NATT; a++)
            att_clear(ctx, &fb->att[a], FALSE);
        fs->fbs[ogles2_name_slot(&fs->far_fb, fb->name, FALSE)] = 0;
        ogles2_name_forget(&fs->far_fb, fb->name);
        fb_free(fb);
    }
}

void ogles2_glDeleteRenderbuffers(struct OGLES2Context *ctx, GLsizei n, const GLuint *renderbuffers)
{
    struct OGLES2FboState *fs;
    struct OGLES2Context *m;
    struct OGLES2Framebuffer *fb;
    GLsizei k;
    int a, j;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(fs = fs_get(ctx)) || !renderbuffers)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Renderbuffer *rb = rb_by_name(fs, renderbuffers[k]);
        if (!rb)
            continue;
        /* Every member of the share group unbinds it; it is detached from
         * this context's bound framebuffer (rendered first). Attached to
         * another, it lives on without its name (GLES 2.0 4.4.3). */
        for (m = ctx->share->first; m; m = m->share_next)
            if (m->fbo && m->fbo->rb_bound == rb)
                m->fbo->rb_bound = 0;
        for (j = 0; j < 2; j++) /* the draw and (ES 3.0) the read binding */
            if ((fb = j ? (ctx->read_fb != ctx->draw_fb ? ctx->read_fb : 0) : ctx->draw_fb))
                for (a = 0; a < NATT; a++)
                    if (fb->att[a].type == GL_RENDERBUFFER && fb->att[a].rb == rb) {
                        before_fb_change(ctx, fb);
                        att_clear(ctx, &fb->att[a], FALSE);
                    }
        fs->sh->rbs[ogles2_name_slot(&fs->sh->far_rb, rb->name, FALSE)] = 0;
        ogles2_name_forget(&fs->sh->far_rb, rb->name);
        if (rb->refs)
            rb->orphan = TRUE;
        else
            rb_destroy(ctx, rb, FALSE);
    }
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fb_update(m);
}

GLboolean ogles2_glIsFramebuffer(struct OGLES2Context *ctx, GLuint framebuffer)
{
    struct OGLES2Framebuffer *fb;
    if (!ogles2_context_valid(ctx) || !ctx->fbo)
        return GL_FALSE;
    fb = fb_by_name(ctx->fbo, framebuffer);
    return fb && fb->bound_once ? GL_TRUE : GL_FALSE;
}

GLboolean ogles2_glIsRenderbuffer(struct OGLES2Context *ctx, GLuint renderbuffer)
{
    struct OGLES2Renderbuffer *rb;
    if (!ogles2_context_valid(ctx) || !ctx->fbo)
        return GL_FALSE;
    rb = rb_by_name(ctx->fbo, renderbuffer);
    return rb && rb->bound_once ? GL_TRUE : GL_FALSE;
}

static void rb_storage(struct OGLES2Context *ctx, GLenum target, GLenum internalformat, GLsizei width,
                       GLsizei height, GLsizei samples)
{
    struct OGLES2FboState *fs;
    struct OGLES2Renderbuffer *rb;
    BOOL ok = TRUE;
    if (!(fs = fs_get(ctx)))
        return;
    if (target != GL_RENDERBUFFER ||
        !(is_color(internalformat) || is_depth(internalformat) || is_stencil(internalformat)) ||
        (!ogles2_es3(ctx) && !gles2_format(internalformat))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0 || width > OGLES2_MAX_VIEWPORT || height > OGLES2_MAX_VIEWPORT) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(rb = fs->rb_bound)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    {
        struct OGLES2Context *m; /* any member's job that uses it, first */
        for (m = ctx->share->first; m; m = m->share_next)
            before_rb_change(m, rb);
    }
    rb_free_storage(ctx, rb);
    rb->format = internalformat;
    rb->w = (UWORD)width;
    rb->h = (UWORD)height;
    rb->samples = samples ? 4 : 0;
    if (width && height && ctx->v3d) {
        if (ogles2_es3(ctx) && es3_color(internalformat)) {
            ok = (rb->color = ogles2_texture_hidden_sized(ctx, RB_SW(rb), RB_SH(rb), internalformat)) != 0;
        } else if (is_color(internalformat)) {
            GLenum f = internalformat == GL_RGB565 || internalformat == GL_RGB8_OES ? GL_RGB : GL_RGBA;
            GLenum ty = internalformat == GL_RGBA4    ? GL_UNSIGNED_SHORT_4_4_4_4
                      : internalformat == GL_RGB5_A1  ? GL_UNSIGNED_SHORT_5_5_5_1
                      : internalformat == GL_RGB565   ? GL_UNSIGNED_SHORT_5_6_5
                      : is_half(internalformat)       ? GL_HALF_FLOAT_OES
                                                      : GL_UNSIGNED_BYTE;
            ok = (rb->color = ogles2_texture_hidden(ctx, RB_SW(rb), RB_SH(rb), f, ty)) != 0;
        } else {
            ok = rb_zs_storage(rb);
        }
    }
    if (!ok) {
        rb->w = rb->h = 0;
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    }
    ogles2_fb_update(ctx);
}

void ogles2_glRenderbufferStorage(struct OGLES2Context *ctx, GLenum target, GLenum internalformat,
                                  GLsizei width, GLsizei height)
{
    if (ogles2_context_valid(ctx))
        rb_storage(ctx, target, internalformat, width, height, 0);
}

static BOOL int_color(GLenum f)
{
    return (f >= GL_RGBA32UI && f <= GL_RGB8I) || f == GL_RGB10_A2UI || (f >= GL_R8I && f <= GL_RG32UI);
}

/* A format with no sample counts: the integer ones (ES 3.0's rule, as
 * Mesa's in 3.0 contexts, main/multisample.c:237-249), and the 32-bit float
 * ones, which dEQP-GLES3 allows none (es3fFboTestCase.cpp:281-338) and
 * which the tile buffer could neither blend nor resolve. */
static BOOL no_samples(GLenum f)
{
    return es3_color(f) && (int_color(f) || f == GL_R32F || f == GL_RG32F || f == GL_RGBA32F);
}

/* ES 3.0 6.1.15: a renderbuffer format's sample counts, GL_MAX_SAMPLES (4)
 * for every renderable format but those (none). */
void ogles2_glGetInternalformativ(struct OGLES2Context *ctx, GLenum target, GLenum internalformat, GLenum pname,
                                  GLsizei bufSize, GLint *params)
{
    BOOL none;
    if (!ogles2_es3_entry(ctx))
        return;
    if (target != GL_RENDERBUFFER || (pname != GL_NUM_SAMPLE_COUNTS && pname != GL_SAMPLES) ||
        !(es3_color(internalformat) || is_depth(internalformat) || is_stencil(internalformat))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    none = no_samples(internalformat);
    if (!bufSize || !params)
        return;
    if (pname == GL_NUM_SAMPLE_COUNTS)
        params[0] = none ? 0 : 1;
    else if (!none)
        params[0] = 4;
}

/* ES 3.0 4.4.2.1: up to GL_MAX_SAMPLES (4), none for the formats
 * no_samples names; 1 to 4 samples give 4 (GL_RENDERBUFFER_SAMPLES 4, as
 * Mesa's), the job then in 4x mode (job.c), and glBlitFramebuffer resolves
 * (blit.c; docs/GLES3_PLAN.md stage C item 2). */
void ogles2_glRenderbufferStorageMultisample(struct OGLES2Context *ctx, GLenum target, GLsizei samples,
                                             GLenum internalformat, GLsizei width, GLsizei height)
{
    if (!ogles2_es3_entry(ctx))
        return;
    if (samples < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (samples > 4 || (samples > 0 && no_samples(internalformat))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    rb_storage(ctx, target, internalformat, width, height, samples);
}

/* ---- attachments ---- */

/* An attachment point's index; ES 3.0's GL_DEPTH_STENCIL_ATTACHMENT is
 * ATT_BOTH (depth and stencil); -1: none. */
#define ATT_BOTH NATT
static int attachment_index(struct OGLES2Context *ctx, GLenum attachment)
{
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + OGLES2_MAX_DRAW_BUFFERS)
        return ATT_COLOR + (int)(attachment - GL_COLOR_ATTACHMENT0);
    if (ogles2_es3(ctx) && attachment == GL_DEPTH_STENCIL_ATTACHMENT)
        return ATT_BOTH;
    return attachment == GL_DEPTH_ATTACHMENT  ? ATT_DEPTH
         : attachment == GL_STENCIL_ATTACHMENT ? ATT_STENCIL : -1;
}

/* A colour attachment past the ones there are: ES 3.0's
 * GL_INVALID_OPERATION (GL_MAX_COLOR_ATTACHMENTS), GLES2's
 * GL_INVALID_ENUM. */
static GLenum bad_attachment(struct OGLES2Context *ctx, GLenum attachment)
{
    return ogles2_es3(ctx) && attachment >= GL_COLOR_ATTACHMENT0 + OGLES2_MAX_DRAW_BUFFERS &&
                   attachment < GL_COLOR_ATTACHMENT0 + 32
               ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
}

/* Attachment point a of fb gets renderbuffer rb, or texture t's face or
 * layer and level, or (both 0) nothing; ATT_BOTH sets depth and stencil. */
static void attach(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, int a, struct OGLES2Renderbuffer *rb,
                   struct OGLES2Texture *t, int face, int level)
{
    int k, n = a == ATT_BOTH ? 2 : 1;
    before_fb_change(ctx, fb);
    for (k = 0; k < n; k++) {
        struct OGLES2Attachment *at = &fb->att[a == ATT_BOTH ? ATT_DEPTH + k : a];
        /* first: re-attaching the same one must not free an orphan */
        if (rb)
            rb->refs++;
        if (t)
            ogles2_texture_attach(t);
        att_clear(ctx, at, FALSE);
        if (rb) {
            at->type = GL_RENDERBUFFER;
            at->rb = rb;
        } else if (t) {
            at->type = GL_TEXTURE;
            at->tex = t;
            at->face = (UWORD)face;
            at->level = (UBYTE)level;
        }
    }
    ogles2_fb_update(ctx);
}

void ogles2_glFramebufferRenderbuffer(struct OGLES2Context *ctx, GLenum target, GLenum attachment,
                                      GLenum renderbuffertarget, GLuint renderbuffer)
{
    struct OGLES2FboState *fs;
    struct OGLES2Renderbuffer *rb = 0;
    struct OGLES2Framebuffer *fb;
    int a, which;
    if (!ogles2_context_valid(ctx) || !(fs = fs_get(ctx)))
        return;
    a = attachment_index(ctx, attachment);
    if (!(which = fb_target(ctx, target)) || (renderbuffer && renderbuffertarget != GL_RENDERBUFFER)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (a < 0) {
        ogles2_error(ctx, bad_attachment(ctx, attachment));
        return;
    }
    /* A name is an object once bound (GLES 2.0 4.4.3). */
    if (!(fb = target_fb(ctx, which)) ||
        (renderbuffer && (!(rb = rb_by_name(fs, renderbuffer)) || !rb->bound_once))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    attach(ctx, fb, a, rb, 0, 0, 0);
}

void ogles2_glFramebufferTexture2D(struct OGLES2Context *ctx, GLenum target, GLenum attachment,
                                   GLenum textarget, GLuint texture, GLint level)
{
    struct OGLES2Texture *t = 0;
    struct OGLES2Framebuffer *fb;
    BOOL cube_face = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
    int a, which;
    if (!ogles2_context_valid(ctx) || !fs_get(ctx))
        return;
    a = attachment_index(ctx, attachment);
    if (!(which = fb_target(ctx, target)) ||
        (texture && textarget != GL_TEXTURE_2D && textarget != GL_TEXTURE_RECTANGLE && !cube_face)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (a < 0) {
        ogles2_error(ctx, bad_attachment(ctx, attachment));
        return;
    }
    if (!(fb = target_fb(ctx, which))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (texture) {
        GLenum tt;
        /* GLES 2.0 4.4.3 takes level 0 only; OES_fbo_render_mipmap (offered,
         * as Mesa does) and ES 3.0 any level. */
        if (level < 0 || level > 12 || (textarget == GL_TEXTURE_RECTANGLE && level != 0)) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        /* The texture's own target: 2D, a rectangle (ARB_texture_rectangle),
         * or a face for a cube map. */
        if (!(t = ogles2_texture_get(ctx, texture)) || !(tt = ogles2_texture_target(t)) ||
            (cube_face ? tt != GL_TEXTURE_CUBE_MAP : tt != textarget)) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    attach(ctx, fb, a, 0, t, cube_face ? (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0, level);
}

/* ES 3.0 4.4.2.4: a layer of a 2D array or a slice of a 3D texture. */
void ogles2_glFramebufferTextureLayer(struct OGLES2Context *ctx, GLenum target, GLenum attachment, GLuint texture,
                                      GLint level, GLint layer)
{
    struct OGLES2Texture *t = 0;
    struct OGLES2Framebuffer *fb;
    int a, which;
    if (!ogles2_es3_entry(ctx) || !fs_get(ctx))
        return;
    a = attachment_index(ctx, attachment);
    if (!(which = fb_target(ctx, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (a < 0) {
        ogles2_error(ctx, bad_attachment(ctx, attachment));
        return;
    }
    if (!(fb = target_fb(ctx, which))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (texture) {
        GLenum tt;
        if (!(t = ogles2_texture_get(ctx, texture)) || !(tt = ogles2_texture_target(t)) ||
            (tt != GL_TEXTURE_3D && tt != GL_TEXTURE_2D_ARRAY)) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        /* GL_MAX_3D_TEXTURE_SIZE 2048 (log2: 11 levels), GL_MAX_ARRAY_TEXTURE_LAYERS 2048, 13 levels */
        if (layer < 0 || layer >= 2048 || level < 0 || level > (tt == GL_TEXTURE_3D ? 11 : 12)) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
    }
    attach(ctx, fb, a, 0, t, layer, level);
}

/* ---- EXT_discard_framebuffer, ES 3.0's invalidation ---- */

/* The attachments' bits (OGLES2_DISCARD_*) of an invalidation of fb; FALSE
 * with the error set. */
static BOOL discard_bits(struct OGLES2Context *ctx, const struct OGLES2Framebuffer *fb, GLsizei n,
                         const GLenum *attachments, UBYTE *bits)
{
    GLsizei k;
    *bits = 0;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return FALSE;
    }
    for (k = 0; k < n && attachments; k++) {
        GLenum a = attachments[k];
        if (fb ? a >= GL_COLOR_ATTACHMENT0 && a < GL_COLOR_ATTACHMENT0 + OGLES2_MAX_DRAW_BUFFERS
               : a == 0x1800 /* GL_COLOR(_EXT) */)
            *bits |= OGLES2_DISCARD_COLOR;
        else if (fb ? a == GL_DEPTH_ATTACHMENT : a == 0x1801 /* GL_DEPTH(_EXT) */)
            *bits |= OGLES2_DISCARD_DEPTH;
        else if (fb ? a == GL_STENCIL_ATTACHMENT : a == 0x1802 /* GL_STENCIL(_EXT) */)
            *bits |= OGLES2_DISCARD_STENCIL;
        else if (fb && ogles2_es3(ctx) && a == GL_DEPTH_STENCIL_ATTACHMENT)
            *bits |= OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL;
        else {
            ogles2_error(ctx, fb && ogles2_es3(ctx) && a >= GL_COLOR_ATTACHMENT0 + OGLES2_MAX_DRAW_BUFFERS &&
                                      a < GL_COLOR_ATTACHMENT0 + 32 ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
            return FALSE;
        }
    }
    return TRUE;
}

/* The images' contents become undefined: the open job of that framebuffer
 * does not store them unless it draws or clears again (draw.c, job.c: ES
 * 3.0 programs go on drawing after an invalidation), and with no job open
 * the next one does not load them (Mesa: the job's store and load masks).
 * The default framebuffer: depth and stencil are then not kept across a
 * mid-frame flush; its colour is always kept. */
static void discard(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, UBYTE bits)
{
    if (!fb) { /* one D24S8 image: kept unless both go */
        if ((bits & (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)) ==
            (OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL)) {
            if (ctx->v3d && ctx->v3d->frame_active && !ctx->job_fb)
                ctx->job_discard |= OGLES2_DISCARD_DEPTH | OGLES2_DISCARD_STENCIL;
            else
                ctx->zs_saved = FALSE;
        }
        return;
    }
    if (ctx->v3d && ctx->v3d->frame_active && ctx->job_fb == fb)
        ctx->job_discard |= bits;
    else
        fb->discarded |= bits;
}

void ogles2_glDiscardFramebufferEXT(struct OGLES2Context *ctx, GLenum target, GLsizei numAttachments,
                                    const GLenum *attachments)
{
    UBYTE bits;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_FRAMEBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (discard_bits(ctx, ctx->draw_fb, numAttachments, attachments, &bits))
        discard(ctx, ctx->draw_fb, bits);
}

/* ES 3.0 4.5: EXT_discard_framebuffer's, for either binding; a region
 * (glInvalidateSubFramebuffer) is checked and kept, as the specification
 * allows. */
void ogles2_glInvalidateFramebuffer(struct OGLES2Context *ctx, GLenum target, GLsizei numAttachments,
                                    const GLenum *attachments)
{
    struct OGLES2Framebuffer *fb;
    UBYTE bits;
    int which;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!(which = fb_target(ctx, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    fb = target_fb(ctx, which);
    if (discard_bits(ctx, fb, numAttachments, attachments, &bits))
        discard(ctx, fb, bits);
}

void ogles2_glInvalidateSubFramebuffer(struct OGLES2Context *ctx, GLenum target, GLsizei numAttachments,
                                       const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height)
{
    UBYTE bits;
    int which;
    (void)x;
    (void)y;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!(which = fb_target(ctx, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!discard_bits(ctx, target_fb(ctx, which), numAttachments, attachments, &bits))
        return;
    if (width < 0 || height < 0)
        ogles2_error(ctx, GL_INVALID_VALUE);
}

/* ---- EXT_draw_buffers ----
 * Fragment output i goes to colour attachment i or nowhere (GL_NONE); the
 * default framebuffer takes GL_BACK or GL_NONE for output 0. The state is
 * the framebuffer's own. A change renders the open job (its render targets
 * are set when it starts). */
void ogles2_glDrawBuffersEXT(struct OGLES2Context *ctx, GLsizei n, const GLenum *bufs)
{
    struct OGLES2Framebuffer *fb = ctx ? ctx->draw_fb : 0;
    GLsizei i;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0 || n > OGLES2_MAX_DRAW_BUFFERS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (n && !bufs)
        return;
    for (i = 0; i < n; i++) {
        GLenum b = bufs[i];
        if (b != GL_NONE && b != GL_BACK &&
            !(b >= GL_COLOR_ATTACHMENT0 && b < GL_COLOR_ATTACHMENT0 + 16)) { /* the extension's 16 names */
            ogles2_error(ctx, GL_INVALID_ENUM);
            return;
        }
        if (fb ? b != GL_NONE && b != GL_COLOR_ATTACHMENT0 + (GLenum)i : b != GL_NONE && b != GL_BACK) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    if (!fb) {
        if (n != 1) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (ctx->v3d && ctx->v3d->frame_active && !ctx->job_fb && bufs[0] != ctx->default_draw_buffer)
            ctx->state_ver++; /* the write masks follow it (draw.c) */
        ctx->default_draw_buffer = bufs[0];
        ogles2_fb_update(ctx);
        return;
    }
    before_fb_change(ctx, fb);
    for (i = 0; i < OGLES2_MAX_DRAW_BUFFERS; i++)
        fb->draw_buf[i] = i < n ? bufs[i] : GL_NONE;
    ctx->state_ver++;
    ogles2_fb_update(ctx);
}

/* ES 3.0's name, EXT_draw_buffers' rules. */
void ogles2_glDrawBuffers(struct OGLES2Context *ctx, GLsizei n, const GLenum *bufs)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glDrawBuffersEXT(ctx, n, bufs);
}

/* ES 3.0 4.3.1: the colour buffer glReadPixels and the copies read, the
 * framebuffer's own: GL_BACK or GL_NONE for the default one, a colour
 * attachment or GL_NONE for an object. */
void ogles2_glReadBuffer(struct OGLES2Context *ctx, GLenum src)
{
    struct OGLES2FboState *fs;
    struct OGLES2Framebuffer *fb;
    if (!ogles2_es3_entry(ctx))
        return;
    if (src != GL_NONE && src != GL_BACK && !(src >= GL_COLOR_ATTACHMENT0 && src < GL_COLOR_ATTACHMENT0 + 32)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    fb = ctx->read_fb;
    if (fb ? src == GL_BACK || (src != GL_NONE && src >= GL_COLOR_ATTACHMENT0 + OGLES2_MAX_DRAW_BUFFERS)
           : src != GL_NONE && src != GL_BACK) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (fb)
        fb->read_buf = src;
    else if ((fs = fs_get(ctx)))
        fs->default_read_none = src == GL_NONE;
}

/* The default framebuffer's read buffer is GL_NONE (frame.c). */
BOOL ogles2_fbo_default_read_none(struct OGLES2Context *ctx)
{
    return ctx->fbo && ctx->fbo->default_read_none;
}

/* GL_DRAW_BUFFERi_EXT of the bound framebuffer (state.c). */
GLenum ogles2_fbo_draw_buffer(struct OGLES2Context *ctx, int i)
{
    if (i < 0 || i >= OGLES2_MAX_DRAW_BUFFERS)
        return GL_NONE;
    if (!ctx->draw_fb)
        return i == 0 ? ctx->default_draw_buffer : GL_NONE;
    return ctx->draw_fb->draw_buf[i];
}

/* job.c, at a job's start: what it need not load. */
UBYTE ogles2_fbo_take_discarded(struct OGLES2Framebuffer *fb)
{
    UBYTE b = fb->discarded;
    fb->discarded = 0;
    return b;
}

/* ---- completeness (GLES 2.0 4.4.5) ---- */

/* An attachment's image size; FALSE if it has none. */
static BOOL att_size(const struct OGLES2Attachment *at, UWORD *w, UWORD *h)
{
    GLenum f, ty;
    if (at->type == GL_RENDERBUFFER) {
        *w = at->rb->w;
        *h = at->rb->h;
        return TRUE;
    }
    return ogles2_texture_image(at->tex, at->face, at->level, w, h, &f, &ty);
}

/* An image with depth and stencil in one: a D24S8 renderbuffer, or a
 * DEPTH_STENCIL_OES texture. */
static BOOL packed(const struct OGLES2Attachment *at)
{
    UBYTE bits;
    BOOL st = FALSE;
    if (at->type == GL_RENDERBUFFER)
        return at->rb->format == GL_DEPTH24_STENCIL8_OES || at->rb->format == GL_DEPTH32F_STENCIL8;
    return at->type == GL_TEXTURE && ogles2_texture_depth(at->tex, at->level, &bits, &st) && st;
}

static BOOL same_image(const struct OGLES2Attachment *a, const struct OGLES2Attachment *b)
{
    return a->type == b->type &&
           (a->type == GL_RENDERBUFFER ? a->rb == b->rb : a->tex == b->tex && a->level == b->level);
}

/* A colour attachment's format as a render target; FALSE: none. GLES2's
 * 16F renderbuffers without some components are kept as RGBA16F: their
 * channels the format's. */
static BOOL att_rt(const struct OGLES2Attachment *at, struct OGLES2RtFormat *rf)
{
    if (at->type == GL_TEXTURE)
        return ogles2_texture_rt(at->tex, at->face, at->level, rf);
    if (at->type != GL_RENDERBUFFER || !at->rb->color || !ogles2_texture_rt(at->rb->color, 0, 0, rf))
        return FALSE;
    if (at->rb->format == GL_RGB16F_EXT || at->rb->format == GL_RG16F_EXT || at->rb->format == GL_R16F_EXT) {
        rf->comps = at->rb->format == GL_RGB16F_EXT ? 3 : at->rb->format == GL_RG16F_EXT ? 2 : 1;
        rf->no_alpha = 1;
    }
    return TRUE;
}

/* GLES 2.0 4.4.5; ES 3.0 4.4.4 in ES3 contexts: attachments of different
 * sizes are complete (drawing covers their common area), single- and
 * multisampled ones are not mixed (renderbuffers' sample counts), and
 * depth and stencil, both attached, are one image. */
static GLenum status_of(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb)
{
    const struct OGLES2Attachment *d = &fb->att[ATT_DEPTH], *s = &fb->att[ATT_STENCIL];
    BOOL es3 = ogles2_es3(ctx);
    UWORD w0 = 0, h0 = 0, w, h;
    int a, n = 0, samples = -1;
    for (a = 0; a < NATT; a++) {
        const struct OGLES2Attachment *at = &fb->att[a];
        int ns;
        if (at->type == GL_NONE)
            continue;
        if (!att_size(at, &w, &h) || !w || !h)
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (at->type == GL_RENDERBUFFER) {
            GLenum f = at->rb->format;
            if ((a < ATT_DEPTH && !is_color(f)) || (a == ATT_DEPTH && !is_depth(f)) ||
                (a == ATT_STENCIL && !is_stencil(f)))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        } else if (a < ATT_DEPTH) {
            if (!ogles2_texture_renderable(at->tex, at->face, at->level))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        } else {
            /* OES_depth_texture: a depth image for the depth, and, with
             * OES_packed_depth_stencil, a depth-stencil one for the stencil. */
            UBYTE bits;
            BOOL st;
            if (at->face || !ogles2_texture_depth(at->tex, at->level, &bits, &st) || (a == ATT_STENCIL && !st))
                return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        ns = at->type == GL_RENDERBUFFER ? at->rb->samples : 0;
        if (samples >= 0 && ns != samples)
            return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        samples = ns;
        if (n++ == 0) {
            w0 = w;
            h0 = h;
        } else if (!es3 && (w != w0 || h != h0)) {
            return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        }
    }
    if (!n)
        return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    /* Depth and stencil: one packed image for both, or apart without a
     * packed one (D16 or a depth texture, and S8); ES 3.0: one image. */
    if (d->type != GL_NONE && s->type != GL_NONE && (es3 || packed(d) || packed(s)) && !same_image(d, s))
        return GL_FRAMEBUFFER_UNSUPPORTED;
    return GL_FRAMEBUFFER_COMPLETE;
}

GLenum ogles2_glCheckFramebufferStatus(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2Framebuffer *fb;
    int which;
    if (!ogles2_context_valid(ctx))
        return 0;
    if (!(which = fb_target(ctx, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    fb = target_fb(ctx, which);
    return fb ? status_of(ctx, fb) : GL_FRAMEBUFFER_COMPLETE;
}

GLenum ogles2_fb_status(struct OGLES2Context *ctx)
{
    return ctx->draw_fb ? status_of(ctx, ctx->draw_fb) : GL_FRAMEBUFFER_COMPLETE;
}

/* The fb_* fields: the default framebuffer's, or the bound object's. */
void ogles2_fb_update(struct OGLES2Context *ctx)
{
    struct OGLES2Framebuffer *fb = ctx->draw_fb;
    int a;
    if (!fb) {
        ctx->fb_w = (UWORD)ctx->width;
        ctx->fb_h = (UWORD)ctx->height;
        ctx->fb_flip = TRUE;
        ctx->fb_depth_bits = ctx->depth_bits;
        ctx->fb_stencil_bits = ctx->stencil_bits;
        ctx->fb_depth_format = ctx->depth_format;
        ctx->fb_draw_mask = ctx->default_draw_buffer == GL_NONE ? 0 : 1;
        ctx->fb_nrt = 1;
        ctx->fb_soft_blend = 0;
        memset(ctx->fb_rt, 0, sizeof(ctx->fb_rt));
        ctx->fb_rt[0].out = V3D_OUTPUT_IMAGE_FORMAT_RGBA8; /* the bitmap's */
        ctx->fb_rt[0].type = V3D_INTERNAL_TYPE_8;
        ctx->fb_rt[0].kind = OGLES2_RT_UNORM;
        ctx->fb_rt[0].comps = 4;
        ctx->fb_rt[0].bits = 8;
        return;
    }
    /* EXT_draw_buffers: the colour attachments drawn into, and the tile
     * buffer's render targets (output i goes to target i). */
    ctx->fb_draw_mask = 0;
    ctx->fb_nrt = 1;
    ctx->fb_soft_blend = 0;
    memset(ctx->fb_rt, 0, sizeof(ctx->fb_rt));
    for (a = 0; a < OGLES2_MAX_DRAW_BUFFERS; a++)
        if (fb->draw_buf[a] != GL_NONE && fb->att[ATT_COLOR + a].type != GL_NONE) {
            struct OGLES2RtFormat *rf = &ctx->fb_rt[a];
            ctx->fb_draw_mask |= (UBYTE)(1 << a);
            ctx->fb_nrt = (UBYTE)(a + 1);
            /* Mesa's v3d_fb_needs_soft_blend: a target the tile buffer
             * cannot blend (32-bit floats, integers) */
            if (att_rt(&fb->att[ATT_COLOR + a], rf) && rf->kind != OGLES2_RT_UNORM && rf->kind != OGLES2_RT_F16)
                ctx->fb_soft_blend = 1;
        }
    ctx->fb_w = ctx->fb_h = 0;
    ctx->fb_flip = FALSE;
    ctx->fb_depth_bits = ctx->fb_stencil_bits = 0;
    ctx->fb_depth_format = OGLES2_DEPTH_16;
    for (a = 0; a < NATT; a++) { /* ES 3.0: the attachments' common area (GLES2: one size) */
        UWORD w, h;
        if (fb->att[a].type != GL_NONE && att_size(&fb->att[a], &w, &h) && w && h) {
            if (!ctx->fb_w || w < ctx->fb_w)
                ctx->fb_w = w;
            if (!ctx->fb_h || h < ctx->fb_h)
                ctx->fb_h = h;
        }
    }
    if (fb->att[ATT_DEPTH].type == GL_RENDERBUFFER) {
        GLenum f = fb->att[ATT_DEPTH].rb->format;
        if (f == GL_DEPTH_COMPONENT16) {
            ctx->fb_depth_bits = 16;
        } else if (f == GL_DEPTH24_STENCIL8_OES || f == GL_DEPTH_COMPONENT24) {
            ctx->fb_depth_bits = 24;
            ctx->fb_depth_format = OGLES2_DEPTH_24;
        } else if (f == GL_DEPTH_COMPONENT32F || f == GL_DEPTH32F_STENCIL8) {
            ctx->fb_depth_bits = 32;
            ctx->fb_depth_format = OGLES2_DEPTH_32F;
        }
    }
    if (fb->att[ATT_STENCIL].type == GL_RENDERBUFFER && is_stencil(fb->att[ATT_STENCIL].rb->format)) {
        ctx->fb_stencil_bits = 8;
        if (fb->att[ATT_STENCIL].rb->format == GL_DEPTH24_STENCIL8_OES)
            ctx->fb_depth_format = OGLES2_DEPTH_24;
        else if (fb->att[ATT_STENCIL].rb->format == GL_DEPTH32F_STENCIL8)
            ctx->fb_depth_format = OGLES2_DEPTH_32F;
    }
    /* OES_depth_texture: a 16-bit depth texture is D16, a 24-bit one (and
     * a depth-stencil one) D24S8 in the tile buffer. */
    {
        UBYTE bits;
        BOOL st;
        const struct OGLES2Attachment *d = &fb->att[ATT_DEPTH], *s = &fb->att[ATT_STENCIL];
        if (d->type == GL_TEXTURE && ogles2_texture_depth(d->tex, d->level, &bits, &st)) {
            ctx->fb_depth_bits = bits;
            if (bits == 24)
                ctx->fb_depth_format = OGLES2_DEPTH_24;
            else if (bits == 32)
                ctx->fb_depth_format = OGLES2_DEPTH_32F;
        }
        if (s->type == GL_TEXTURE && ogles2_texture_depth(s->tex, s->level, &bits, &st) && st) {
            ctx->fb_stencil_bits = 8;
            ctx->fb_depth_format = bits == 32 ? OGLES2_DEPTH_32F : OGLES2_DEPTH_24;
        }
    }
}

BOOL ogles2_fb_ready(struct OGLES2Context *ctx)
{
    GLenum st;
    ogles2_fb_update(ctx);
    if (!ctx->draw_fb || (st = status_of(ctx, ctx->draw_fb)) == GL_FRAMEBUFFER_COMPLETE)
        return TRUE;
    ogles2_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
    if (ctx->fbo && !ctx->fbo->warned_incomplete) {
        ctx->fbo->warned_incomplete = TRUE;
        ogles2_log("framebuffer %ld is not complete (0x%04lx): nothing drawn or read (logged once)",
                   (ULONG)ctx->draw_fb->name, (ULONG)st);
    }
    return FALSE;
}

/* ---- the job's target (job.c) ---- */

static void rb_surface(const struct OGLES2Renderbuffer *rb, BOOL stencil_only, struct OGLES2Surface *s)
{
    BOOL sep = stencil_only && rb->format == GL_DEPTH32F_STENCIL8; /* its own S8 image */
    s->addr = sep ? rb->sbase : rb->base;
    s->ub = sep ? rb->sub : rb->ub;
    s->bytes = sep ? rb->sbytes : rb->bytes;
    s->cpp = sep ? 1 : rb->cpp;
    s->mem_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
    s->format = stencil_only                         ? V3D_OUTPUT_IMAGE_FORMAT_S8
              : rb->format == GL_DEPTH_COMPONENT16 ? V3D_OUTPUT_IMAGE_FORMAT_D16
              : rb->format == GL_DEPTH_COMPONENT32F || rb->format == GL_DEPTH32F_STENCIL8
                                                   ? V3D_OUTPUT_IMAGE_FORMAT_D32F
                                                   : V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    s->force_alpha1 = FALSE;
}

/* Tile pools for w x h: the context's, if it has as many tiles; else fb's. */
static BOOL pools(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, struct OGLES2Target *t)
{
    V3DContext *v = ctx->v3d;
    v3d_u32 zs, ts, ta, tx, ty;
    /* The job's tiles (smaller with several render targets), and as many
     * as the context's pools were made for (32x32 with 4x MSAA). */
    ULONG tiles = (ULONG)((t->w + t->tile_w - 1) / t->tile_w) * ((t->h + t->tile_h - 1) / t->tile_h);
    ULONG ct = ctx->msaa ? 32 : 64, mine = (ULONG)((v->width + ct - 1) / ct) * ((v->height + ct - 1) / ct);
    t->ta_addr = t->ta_size = t->ts_addr = 0;
    if (tiles <= mine)
        return TRUE;
    if (fb->pool_tiles < tiles) {
        ogles2_job_finish(ctx); /* a running render may use the old pools */
        if (fb->pool_alloc.hostptr)
            v3d_mem_free(ogles2_device(), &fb->pool_alloc);
        if (fb->pool_state.hostptr)
            v3d_mem_free(ogles2_device(), &fb->pool_state);
        fb->pool_tiles = 0;
        /* sized for 32x32 tiles when the job's are smaller than 64x64; for
         * smaller ones (32x16 to 8x8: several 64- or 128-bit targets, 4x
         * MSAA) as 32x32 over a frame that much larger, as many tiles */
        v3d_frame_compute_pool_sizes(t->tile_w < 32 ? (UWORD)(t->w * (32 / t->tile_w)) : t->w,
                                     t->tile_h < 32 ? (UWORD)(t->h * (32 / t->tile_h)) : t->h, 16,
                                     t->tile_w < 64 || t->tile_h < 64, &zs, &ts, &ta, &tx, &ty);
        if (v3d_mem_alloc(ogles2_device(), &fb->pool_alloc, ta + 4096) < 0)
            return FALSE;
        if (v3d_mem_alloc(ogles2_device(), &fb->pool_state, ts + 4096) < 0) {
            v3d_mem_free(ogles2_device(), &fb->pool_alloc);
            return FALSE;
        }
        {
            ULONG len = (ULONG)fb->pool_alloc.size;
            CachePreDMA(fb->pool_alloc.hostptr, &len, 0);
            len = (ULONG)fb->pool_state.size;
            CachePreDMA(fb->pool_state.hostptr, &len, 0);
        }
        fb->pool_tiles = tiles;
        ogles2_log("framebuffer %ld: tile pools of its own for %ldx%ld", (ULONG)fb->name, (ULONG)t->w,
                   (ULONG)t->h);
    }
    t->ta_addr = ((ULONG)fb->pool_alloc.hostptr + 4095) & ~4095UL;
    t->ta_size = (ULONG)fb->pool_alloc.size - (t->ta_addr - (ULONG)fb->pool_alloc.hostptr);
    t->ts_addr = ((ULONG)fb->pool_state.hostptr + 4095) & ~4095UL;
    return TRUE;
}

BOOL ogles2_fbo_target(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, BOOL surfaces,
                       struct OGLES2Target *t)
{
    const struct OGLES2Attachment *d = &fb->att[ATT_DEPTH], *s = &fb->att[ATT_STENCIL];
    struct OGLES2Framebuffer *saved = ctx->draw_fb;
    UBYTE drawn;
    int i;
    memset(t, 0, sizeof(*t));
    ctx->draw_fb = fb; /* the sizes and formats of fb, whatever is bound now */
    ogles2_fb_update(ctx);
    t->w = ctx->fb_w;
    t->h = ctx->fb_h;
    t->depth_type = ctx->fb_depth_format == OGLES2_DEPTH_24  ? V3D_INTERNAL_TYPE_DEPTH24
                  : ctx->fb_depth_format == OGLES2_DEPTH_32F ? V3D_INTERNAL_TYPE_DEPTH32F
                                                             : V3D_INTERNAL_TYPE_DEPTH16;
    drawn = ctx->fb_draw_mask;
    t->nrt = ctx->fb_nrt;
    memcpy(t->rt, ctx->fb_rt, sizeof(t->rt));
    ctx->draw_fb = saved;
    ogles2_fb_update(ctx);
    /* The widest drawn target's bpp (64 for a half-float or sRGB one, 128
     * for RGBA32F and the 32-bit RGBA integers). */
    t->bpp = V3D_INTERNAL_BPP_32;
    for (i = 0; i < OGLES2_MAX_DRAW_BUFFERS; i++)
        if ((drawn & (1 << i)) && t->rt[i].kind != OGLES2_RT_NONE && t->rt[i].bpp > t->bpp)
            t->bpp = t->rt[i].bpp;
    t->msaa = ogles2_fbo_samples(fb) != 0;
    /* Mesa's v3d_choose_tile_size for V3D 4.x: 64x64 for one 32-bit
     * target, a step smaller for two, two steps for three or four, two
     * steps more with 4x MSAA, and a step more for each doubling of the
     * widest target (64x32, 32x32, 32x16, 16x16, 16x8, 8x8); the hardware
     * derives the same from the configuration packets. */
    {
        static const UBYTE sizes[] = {64, 64, 64, 32, 32, 32, 32, 16, 16, 16, 16, 8, 8, 8};
        int k = (t->nrt > 2 ? 2 : t->nrt > 1 ? 1 : 0) + (t->msaa ? 2 : 0) + t->bpp;
        t->tile_w = sizes[2 * k];
        t->tile_h = sizes[2 * k + 1];
    }
    if (!t->w || !t->h || !pools(ctx, fb, t))
        return FALSE;
    if (!surfaces)
        return TRUE;
    for (i = 0; i < OGLES2_MAX_DRAW_BUFFERS; i++) {
        const struct OGLES2Attachment *c = &fb->att[ATT_COLOR + i];
        if (!(drawn & (1 << i)))
            continue;
        if (c->type == GL_TEXTURE) {
            if (!ogles2_texture_surface(ctx, c->tex, c->face, c->level, &t->color[i]))
                return FALSE;
        } else if (c->type == GL_RENDERBUFFER) {
            if (!c->rb->color || !ogles2_texture_surface(ctx, c->rb->color, 0, 0, &t->color[i]))
                return FALSE;
        }
        t->colors |= (UBYTE)(1 << i);
    }
    if (d->type == GL_RENDERBUFFER && d->rb->base) {
        t->has_z = TRUE;
        rb_surface(d->rb, FALSE, &t->z);
        t->packed_zs = d->rb->format == GL_DEPTH24_STENCIL8_OES;
    }
    if (s->type == GL_RENDERBUFFER && s->rb->base && !(t->packed_zs && s->rb == d->rb)) {
        if (s->rb->format == GL_DEPTH32F_STENCIL8) { /* its separate S8 image */
            t->has_s = TRUE;
            rb_surface(s->rb, TRUE, &t->s);
            if (!(d->type == GL_RENDERBUFFER && d->rb == s->rb)) { /* as stencil only: its depth in the tile buffer */
                t->has_z = TRUE;
                rb_surface(s->rb, FALSE, &t->z);
            }
        } else if (s->rb->format == GL_DEPTH24_STENCIL8_OES) { /* packed, as stencil only */
            t->has_z = TRUE;
            t->packed_zs = TRUE;
            rb_surface(s->rb, FALSE, &t->z);
        } else {
            t->has_s = TRUE;
            rb_surface(s->rb, TRUE, &t->s);
        }
    }
    /* OES_depth_texture: the depth texture's level itself; a depth-stencil
     * texture for both is one packed image, as a D24S8 renderbuffer; a
     * DEPTH32F_STENCIL8 one has its stencil apart, as the renderbuffer. */
    if (d->type == GL_TEXTURE) {
        if (!ogles2_texture_zs_surface(ctx, d->tex, d->level, &t->z))
            return FALSE;
        t->has_z = TRUE;
        t->packed_zs = same_image(d, s) && t->z.format == V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    }
    if (s->type == GL_TEXTURE && !t->packed_zs) {
        if (ogles2_texture_s8_surface(ctx, s->tex, s->level, &t->s)) {
            t->has_s = TRUE;
            if (same_image(d, s))
                return TRUE;
        }
        /* a depth-stencil texture as the stencil only: its depth in the tile buffer */
        if (!ogles2_texture_zs_surface(ctx, s->tex, s->level, &t->z))
            return FALSE;
        t->has_z = TRUE;
        t->packed_zs = !t->has_s;
    }
    return TRUE;
}

void ogles2_fbo_rendered(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb)
{
    int a;
    (void)ctx;
    for (a = ATT_COLOR; a < ATT_DEPTH; a++) {
        const struct OGLES2Attachment *c = &fb->att[a];
        if (c->type == GL_TEXTURE)
            ogles2_texture_rendered(c->tex, c->face, c->level);
        else if (c->type == GL_RENDERBUFFER && c->rb->color)
            ogles2_texture_rendered(c->rb->color, 0, 0);
    }
    for (a = ATT_DEPTH; a <= ATT_STENCIL; a++) /* depth textures (OES_depth_texture) */
        if (fb->att[a].type == GL_TEXTURE)
            ogles2_texture_rendered(fb->att[a].tex, 0, fb->att[a].level);
}

BOOL ogles2_fbo_read(struct OGLES2Context *ctx, int x, int y, int w, int h, UBYTE *rgba, ULONG stride)
{
    const struct OGLES2Attachment *c = read_attachment(ctx->draw_fb);
    if (!c)
        return FALSE;
    if (c->type == GL_TEXTURE)
        return ogles2_texture_read(c->tex, c->face, c->level, x, y, w, h, rgba, stride);
    if (c->type == GL_RENDERBUFFER && c->rb->color) {
        GLenum f = c->rb->format;
        int i, j;
        if (!ogles2_texture_read(c->rb->color, 0, 0, x, y, w, h, rgba, stride))
            return FALSE;
        /* A 16F format without some components (kept as RGBA16F): those
         * read as GLES reads absent ones, green and blue 0, alpha 1. */
        if (f == GL_RGB16F_EXT || f == GL_RG16F_EXT || f == GL_R16F_EXT)
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++) {
                    UBYTE *p = rgba + (ULONG)j * stride + (ULONG)i * 4;
                    if (f == GL_R16F_EXT)
                        p[1] = 0;
                    if (f != GL_RGB16F_EXT)
                        p[2] = 0;
                    p[3] = 255;
                }
        return TRUE;
    }
    return FALSE;
}

BOOL ogles2_fbo_fill(struct OGLES2Context *ctx, int rt, int x, int y, int w, int h, const ULONG *v, BOOL is_int,
                     const GLboolean *mask)
{
    const struct OGLES2Attachment *c;
    if (!ctx->draw_fb || rt < 0 || rt >= OGLES2_MAX_DRAW_BUFFERS)
        return FALSE;
    c = &ctx->draw_fb->att[ATT_COLOR + rt];
    if (c->type == GL_TEXTURE)
        return ogles2_texture_fill(c->tex, c->face, c->level, x, y, w, h, v, is_int, mask);
    return c->type == GL_RENDERBUFFER && c->rb->color &&
           ogles2_texture_fill(c->rb->color, 0, 0, x, y, w, h, v, is_int, mask);
}

struct OGLES2Framebuffer *ogles2_fbo_read_begin(struct OGLES2Context *ctx)
{
    struct OGLES2Framebuffer *saved = ctx->draw_fb;
    if (ctx->read_fb != saved) {
        ctx->draw_fb = ctx->read_fb;
        ogles2_fb_update(ctx);
    }
    return saved;
}

void ogles2_fbo_read_end(struct OGLES2Context *ctx, struct OGLES2Framebuffer *saved)
{
    if (ctx->draw_fb != saved) {
        ctx->draw_fb = saved;
        ogles2_fb_update(ctx);
    }
}

/* glBlitFramebuffer's write (blit.c): a box of draw buffer rt from
 * 16-byte texels (floats or integers), those `mask` marks; the default
 * framebuffer's bitmap as RGBA bytes (the box read first where the mask
 * leaves pixels). The jobs drawing into it have rendered. */
BOOL ogles2_fbo_write(struct OGLES2Context *ctx, int rt, int x, int y, int w, int h, const UBYTE *src, ULONG stride,
                      const UBYTE *mask)
{
    const struct OGLES2Attachment *c;
    if (!ctx->draw_fb) {
        ULONG row = (ULONG)w * 4;
        UBYTE *rgba;
        int i, j, k;
        if (rt || !(rgba = AllocVec(row * (ULONG)h, MEMF_ANY)))
            return FALSE;
        ogles2_display_read(&ctx->display, (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, rgba, row);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                if (!mask || mask[(ULONG)j * w + i])
                    for (k = 0; k < 4; k++) {
                        GLfloat v = ((const GLfloat *)(src + (ULONG)j * stride + (ULONG)i * 16))[k];
                        rgba[(ULONG)j * row + (ULONG)i * 4 + k] =
                            v <= 0.0f ? 0 : v >= 1.0f ? 255 : (UBYTE)(v * 255.0f + 0.5f);
                    }
        ogles2_display_write(&ctx->display, (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, rgba, row);
        FreeVec(rgba);
        return TRUE;
    }
    if (rt < 0 || rt >= OGLES2_MAX_DRAW_BUFFERS)
        return FALSE;
    c = &ctx->draw_fb->att[ATT_COLOR + rt];
    if (c->type == GL_TEXTURE)
        return ogles2_texture_write(c->tex, c->face, c->level, x, y, w, h, src, stride, mask);
    return c->type == GL_RENDERBUFFER && c->rb->color &&
           ogles2_texture_write(c->rb->color, 0, 0, x, y, w, h, src, stride, mask);
}

/* ---- glBlitFramebuffer's depth and stencil (blit.c) ----
 * Renderbuffers only: their images are UIF without XOR, padded as
 * rb_zs_storage pads them, which v3dcore's CPU tiling reads and writes
 * through a one-level texture view; a D24S8 texel's stencil is its low
 * byte (little-endian), its depth the other three; DEPTH32F_STENCIL8's
 * stencil an S8 image of its own. */

/* The depth (stencil FALSE) or stencil renderbuffer format of fb's
 * attachment, as blit.c compares them (the default framebuffer's as the
 * renderbuffer it would be); 0: none blittable (no attachment, a texture). */
GLenum ogles2_fbo_zs_format(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, BOOL stencil)
{
    const struct OGLES2Attachment *at;
    if (!fb) {
        if (stencil ? !ctx->stencil_bits : !ctx->depth_bits)
            return 0;
        return ctx->depth_format == OGLES2_DEPTH_24    ? GL_DEPTH24_STENCIL8_OES
             : ctx->depth_format == OGLES2_DEPTH_32F ? GL_DEPTH_COMPONENT32F
                                                     : GL_DEPTH_COMPONENT16;
    }
    at = &fb->att[stencil ? ATT_STENCIL : ATT_DEPTH];
    return at->type == GL_RENDERBUFFER && at->rb->base ? at->rb->format : 0;
}

/* A texture view of renderbuffer rb's depth image (sep: DEPTH32F_STENCIL8's
 * separate stencil). */
static void zs_view(const struct OGLES2Renderbuffer *rb, BOOL sep, V3DTexture *v)
{
    ULONG cpp = sep ? 1 : rb->cpp;
    ULONG ub_w = 2 * v3d_utile_width((int)cpp), ub_h = 2 * v3d_utile_height((int)cpp);
    ULONG pw = (RB_SW(rb) + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w), ph = (RB_SH(rb) + ub_h - 1) / ub_h * ub_h;
    memset(v, 0, sizeof(*v));
    v->texture_mem.hostptr = (void *)(sep ? rb->sbase : rb->base);
    v->width = RB_SW(rb);
    v->height = RB_SH(rb);
    v->cpp = (v3d_u8)cpp;
    v->num_levels = 1;
    v->levels[0].offset = 0;
    v->levels[0].stride = pw * cpp;
    v->levels[0].padded_w = (v3d_u16)pw;
    v->levels[0].padded_h = (v3d_u16)ph;
    v->levels[0].tiling_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
    v->tiling_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
}

/* One side of a depth or stencil blit: its image as a texture view, the
 * bytes of a texel the part is (a mask: bit i byte i), the rows the
 * framebuffer has (the default one's are stored top down: flip), and a
 * multisampled image's 2 (a pixel's sample 0 at twice its coordinates:
 * the blit's resolve takes that one, as ES 3.0 4.3.3 allows). */
struct zs_side
{
    V3DTexture v;
    ULONG w, h;
    UBYTE bytes;
    BOOL flip;
    UBYTE scale;
};

static BOOL zs_side(struct OGLES2Context *ctx, struct OGLES2Framebuffer *fb, BOOL stencil, struct zs_side *z)
{
    z->scale = 1;
    if (!fb) {
        if (!ogles2_job_zs_view(ctx, &z->v))
            return FALSE;
        z->w = ctx->width < z->v.width ? ctx->width : z->v.width;
        z->h = ctx->height < z->v.height ? ctx->height : z->v.height;
        z->flip = TRUE;
        z->bytes = stencil ? 1 : ctx->depth_format == OGLES2_DEPTH_24 ? 0x0e : z->v.cpp == 2 ? 0x03 : 0x0f;
    } else {
        const struct OGLES2Renderbuffer *rb = fb->att[stencil ? ATT_STENCIL : ATT_DEPTH].rb;
        zs_view(rb, stencil && rb->format == GL_DEPTH32F_STENCIL8, &z->v);
        z->w = rb->w;
        z->h = rb->h;
        z->flip = FALSE;
        z->scale = rb->samples ? 2 : 1;
        if (stencil)
            z->bytes = 1; /* S8, the separate S8, or D24S8's low byte */
        else
            z->bytes = rb->format == GL_DEPTH24_STENCIL8_OES || rb->format == GL_DEPTH_COMPONENT24 ? 0x0e
                     : rb->cpp == 2 ? 0x03 : 0x0f;
    }
    return TRUE;
}

/* The depth or stencil blit: the destination box ox0..ox1, oy0..oy1 (in
 * the draw framebuffer, clipped) takes, byte by byte, the nearest texel
 * of the source rectangle mapped as glBlitFramebuffer maps it; texels
 * whose sample is outside the read framebuffer's image are left. The
 * default framebuffer's are those it keeps across a mid-frame flush (job.c):
 * none before anything was drawn or cleared, and none with 4x MSAA. */
void ogles2_fbo_blit_zs(struct OGLES2Context *ctx, BOOL stencil, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                        GLint dx0, GLint dy0, GLint dx1, GLint dy1, LONG ox0, LONG oy0, LONG ox1, LONG oy1)
{
    static BOOL logged;
    struct zs_side sz, dz;
    ULONG cpp, sw, sh, dw, dh;
    UBYTE *src, *dst;
    GLfloat scx = (GLfloat)(sx1 - sx0) / (GLfloat)(dx1 - dx0), scy = (GLfloat)(sy1 - sy0) / (GLfloat)(dy1 - dy0);
    LONG x, y;
    int b;
    if (!zs_side(ctx, ctx->read_fb, stencil, &sz) || !zs_side(ctx, ctx->draw_fb, stencil, &dz)) {
        if (!logged) {
            logged = TRUE;
            ogles2_log("glBlitFramebuffer: the default framebuffer's depth and stencil are blitted only once "
                       "something was drawn or cleared, and not with 4x MSAA (logged once)");
        }
        return;
    }
    cpp = dz.v.cpp;
    sw = sz.v.width;
    sh = sz.v.height;
    dw = dz.v.width;
    dh = dz.v.height;
    if (sz.v.cpp != cpp || sz.bytes != dz.bytes || ox0 >= ox1 || oy0 >= oy1)
        return;
    if (!(src = AllocVec(sw * sh * cpp, MEMF_ANY)) || !(dst = AllocVec(dw * dh * cpp, MEMF_ANY))) {
        if (src)
            FreeVec(src);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    v3d_texture_load(ogles2_device(), &sz.v, 0, src, sw * cpp, 0, 0, (int)sw, (int)sh);
    v3d_texture_load(ogles2_device(), &dz.v, 0, dst, dw * cpp, 0, 0, (int)dw, (int)dh);
    for (y = oy0; y < oy1 && y < (LONG)dz.h; y++)
        for (x = ox0; x < ox1 && x < (LONG)dz.w; x++) {
            GLfloat u = (GLfloat)sx0 + ((GLfloat)x + 0.5f - (GLfloat)dx0) * scx;
            GLfloat v = (GLfloat)sy0 + ((GLfloat)y + 0.5f - (GLfloat)dy0) * scy;
            LONG iu = (LONG)(u < 0.0f ? u - 1.0f : u), iv = (LONG)(v < 0.0f ? v - 1.0f : v);
            ULONG sr, dr;
            const UBYTE *s;
            UBYTE *d;
            if (iu < 0 || iv < 0 || iu >= (LONG)sz.w || iv >= (LONG)sz.h)
                continue;
            sr = sz.flip ? sz.h - 1 - (ULONG)iv : (ULONG)iv;
            dr = dz.flip ? dz.h - 1 - (ULONG)y : (ULONG)y;
            s = src + (sr * sz.scale * sw + (ULONG)iu * sz.scale) * cpp;
            d = dst + (dr * dw + (ULONG)x) * cpp;
            for (b = 0; b < (int)cpp; b++)
                if (dz.bytes & (1 << b))
                    d[b] = s[b];
        }
    v3d_texture_store(ogles2_device(), &dz.v, 0, dst, dw * cpp, 0, 0, (int)dw, (int)dh);
    FreeVec(dst);
    FreeVec(src);
}

/* glBlitFramebuffer (ES 3.0 4.3.3): the samples of fb's renderbuffers (an
 * object's; the default framebuffer resolves its own: 0). */
UBYTE ogles2_fbo_samples(const struct OGLES2Framebuffer *fb)
{
    UBYTE n = 0;
    int a;
    for (a = 0; fb && a < NATT; a++)
        if (fb->att[a].type == GL_RENDERBUFFER && fb->att[a].rb->samples > n)
            n = fb->att[a].rb->samples;
    return n;
}

/* glReadPixels and the copies from the read framebuffer (the draw one
 * between ogles2_fbo_read_begin and _end): complete, and not multisampled
 * (ES 3.0 4.3.1 and 3.8.5: GL_INVALID_OPERATION). */
BOOL ogles2_fbo_read_ready(struct OGLES2Context *ctx)
{
    if (!ogles2_fb_ready(ctx))
        return FALSE;
    if (ogles2_fbo_samples(ctx->draw_fb)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    return TRUE;
}

/* glBlitFramebuffer from a multisampled read buffer: its format is each
 * draw buffer's (`draw`: their mask; the default framebuffer's RGBA8). */
BOOL ogles2_fbo_blit_same_format(struct OGLES2Context *ctx, UBYTE draw)
{
    const struct OGLES2Attachment *c;
    struct OGLES2RtFormat r, d;
    int k;
    if (!ctx->read_fb || !(c = read_attachment(ctx->read_fb)) || !att_rt(c, &r))
        return TRUE; /* no read buffer: nothing blitted */
    for (k = 0; k < OGLES2_MAX_DRAW_BUFFERS; k++) {
        if (!(draw & (1 << k)))
            continue;
        if (!ctx->draw_fb) {
            if (r.pipe != 84) /* PIPE_FORMAT_R8G8B8A8_UNORM */
                return FALSE;
            continue;
        }
        c = &ctx->draw_fb->att[ATT_COLOR + k];
        if (c->type != GL_NONE && (!att_rt(c, &d) || d.pipe != r.pipe))
            return FALSE;
    }
    return TRUE;
}

/* ES 3.0 4.3.1: the read buffer's format as a render target (its kind
 * says glReadPixels' format and type); FALSE: no colour image. */
BOOL ogles2_fbo_read_format(struct OGLES2Context *ctx, struct OGLES2RtFormat *rf)
{
    const struct OGLES2Attachment *c = read_attachment(ctx->draw_fb);
    return c && att_rt(c, rf);
}

/* A box of a float or integer read buffer as 16-byte texels (RGBA floats
 * or four 32-bit integers, absent components 0, 0 and 1). */
BOOL ogles2_fbo_read_wide(struct OGLES2Context *ctx, int x, int y, int w, int h, UBYTE *out, ULONG stride)
{
    const struct OGLES2Attachment *c = read_attachment(ctx->draw_fb);
    if (!c)
        return FALSE;
    if (c->type == GL_TEXTURE)
        return ogles2_texture_read_wide(c->tex, c->face, c->level, x, y, w, h, out, stride);
    if (c->type != GL_RENDERBUFFER || !c->rb->color ||
        !ogles2_texture_read_wide(c->rb->color, 0, 0, x, y, w, h, out, stride))
        return FALSE;
    {
        /* A 16F format without some components (kept as RGBA16F): those
         * read as absent ones, green and blue 0, alpha 1. */
        GLenum f = c->rb->format;
        int i, j;
        if (f == GL_RGB16F_EXT || f == GL_RG16F_EXT || f == GL_R16F_EXT)
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++) {
                    GLfloat *p = (GLfloat *)(out + (ULONG)j * stride) + i * 4;
                    if (f == GL_R16F_EXT)
                        p[1] = 0.0f;
                    if (f != GL_RGB16F_EXT)
                        p[2] = 0.0f;
                    p[3] = 1.0f;
                }
    }
    return TRUE;
}

/* ---- queries ---- */

static void rb_bits(GLenum f, GLint *r, GLint *g, GLint *b, GLint *a, GLint *d, GLint *s)
{
    *r = *g = *b = *a = *d = *s = 0;
    switch (f) {
    case GL_RGBA4:   *r = *g = *b = *a = 4; break;
    case GL_RGB5_A1: *r = *g = *b = 5; *a = 1; break;
    case GL_RGB565:  *r = *b = 5; *g = 6; break;
    case GL_RGB8_OES: *r = *g = *b = 8; break;
    case GL_RGBA8_OES:
    case GL_BGRA_EXT:
    case GL_BGRA8_EXT: *r = *g = *b = *a = 8; break;
    case GL_RGBA16F_EXT: *r = *g = *b = *a = 16; break;
    case GL_RGB16F_EXT: *r = *g = *b = 16; break;
    case GL_RG16F_EXT: *r = *g = 16; break;
    case GL_R16F_EXT: *r = 16; break;
    case GL_DEPTH_COMPONENT16: *d = 16; break;
    case GL_STENCIL_INDEX8: *s = 8; break;
    case GL_DEPTH24_STENCIL8_OES: *d = 24; *s = 8; break;
    case GL_DEPTH_COMPONENT24: *d = 24; break;
    case GL_DEPTH_COMPONENT32F: *d = 32; break;
    case GL_DEPTH32F_STENCIL8: *d = 32; *s = 8; break;
    }
}

/* A render target's colour bits (the ES 3.0 formats). */
static void rt_bits(const struct OGLES2RtFormat *rf, GLint *r, GLint *g, GLint *b, GLint *a)
{
    *r = rf->bits;
    *g = rf->comps > 1 ? rf->bits : 0;
    *b = rf->comps > 2 ? rf->bits : 0;
    *a = rf->comps > 3 ? rf->bits : 0;
    switch (rf->out) {
    case V3D_OUTPUT_IMAGE_FORMAT_BGR565:       *g = 6; break;
    case V3D_OUTPUT_IMAGE_FORMAT_ABGR1555:     *a = 1; break;
    case V3D_OUTPUT_IMAGE_FORMAT_RGB10A2:
    case V3D_OUTPUT_IMAGE_FORMAT_RGB10A2UI:    *a = 2; break;
    case V3D_OUTPUT_IMAGE_FORMAT_R11FG11FB10F: *b = 10; break;
    }
}

/* A texture image's colour bits. */
static void tex_bits(const struct OGLES2Attachment *at, GLint *r, GLint *g, GLint *b, GLint *a)
{
    UWORD w, h;
    GLenum f = 0, ty = 0;
    struct OGLES2RtFormat rf;
    if (att_rt(at, &rf)) { /* a colour-renderable one: its format's */
        rt_bits(&rf, r, g, b, a);
        return;
    }
    ogles2_texture_image(at->tex, at->face, at->level, &w, &h, &f, &ty);
    *r = *g = *b = 8;
    *a = f == GL_RGBA || f == GL_BGRA_EXT ? 8 : 0;
    if (ty == GL_UNSIGNED_SHORT_5_6_5) { *r = *b = 5; *g = 6; }
    if (ty == GL_UNSIGNED_SHORT_4_4_4_4) { *r = *g = *b = *a = 4; }
    if (ty == GL_UNSIGNED_SHORT_5_5_5_1) { *r = *g = *b = 5; *a = 1; }
    if (ty == GL_HALF_FLOAT_OES) { *r = *g = *b = 16; *a = f == GL_RGBA ? 16 : 0; }
}

/* Whether an attachment's image has storage: a renderbuffer's
 * glRenderbufferStorage, a texture level's glTexImage. ES 3.0 6.1.13: the
 * sizes of one without are 0, its component type GL_NONE. */
static BOOL att_specified(const struct OGLES2Attachment *at)
{
    UWORD w, h;
    GLenum f, ty;
    if (at->type == GL_RENDERBUFFER)
        return at->rb->w != 0;
    return ogles2_texture_image(at->tex, at->face, at->level, &w, &h, &f, &ty);
}

/* EXT_color_buffer_half_float, ES 3.0: an attachment's component type. */
static GLint component_type(const struct OGLES2Attachment *at, int a)
{
    struct OGLES2RtFormat rf;
    UBYTE bits;
    BOOL st;
    if (a == ATT_STENCIL)
        return GL_UNSIGNED_INT; /* stencil indices (GLES 3.0's answer) */
    if (a == ATT_DEPTH) {
        if (at->type == GL_RENDERBUFFER)
            return at->rb->format == GL_DEPTH_COMPONENT32F || at->rb->format == GL_DEPTH32F_STENCIL8
                       ? GL_FLOAT : GL_UNSIGNED_NORMALIZED_EXT;
        return ogles2_texture_depth(at->tex, at->level, &bits, &st) && bits == 32 ? GL_FLOAT
                                                                                  : GL_UNSIGNED_NORMALIZED_EXT;
    }
    if (!att_rt(at, &rf))
        return GL_UNSIGNED_NORMALIZED_EXT;
    return rf.kind == OGLES2_RT_F16 || rf.kind == OGLES2_RT_F32 ? GL_FLOAT
         : rf.kind == OGLES2_RT_INT                            ? GL_INT
         : rf.kind == OGLES2_RT_UINT                           ? GL_UNSIGNED_INT
                                                               : GL_UNSIGNED_NORMALIZED_EXT;
}

void ogles2_glGetRenderbufferParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                         GLint *params)
{
    struct OGLES2Renderbuffer *rb;
    GLint bits[6];
    if (!ogles2_context_valid(ctx) || !params || !fs_get(ctx))
        return;
    if (target != GL_RENDERBUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(rb = ctx->fbo->rb_bound)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    rb_bits(rb->w ? rb->format : 0, &bits[0], &bits[1], &bits[2], &bits[3], &bits[4], &bits[5]);
    if (rb->color && es3_color(rb->format)) { /* ES 3.0's sized colour formats: the storage's */
        struct OGLES2Attachment at;
        struct OGLES2RtFormat rf;
        memset(&at, 0, sizeof(at));
        at.type = GL_RENDERBUFFER;
        at.rb = rb;
        if (att_rt(&at, &rf))
            rt_bits(&rf, &bits[0], &bits[1], &bits[2], &bits[3]);
    }
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH:           *params = rb->w; break;
    case GL_RENDERBUFFER_HEIGHT:          *params = rb->h; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *params = (GLint)rb->format; break;
    case GL_RENDERBUFFER_RED_SIZE:        *params = bits[0]; break;
    case GL_RENDERBUFFER_GREEN_SIZE:      *params = bits[1]; break;
    case GL_RENDERBUFFER_BLUE_SIZE:       *params = bits[2]; break;
    case GL_RENDERBUFFER_ALPHA_SIZE:      *params = bits[3]; break;
    case GL_RENDERBUFFER_DEPTH_SIZE:      *params = bits[4]; break;
    case GL_RENDERBUFFER_STENCIL_SIZE:    *params = bits[5]; break;
    case GL_RENDERBUFFER_SAMPLES:
        if (!ogles2_es3(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        *params = rb->samples;
        break;
    default:                              ogles2_error(ctx, GL_INVALID_ENUM); break;
    }
}

/* GLES 2.0 6.1.13; ES 3.0 6.1.13 in ES3 contexts: either binding, the
 * default framebuffer's GL_BACK, GL_DEPTH and GL_STENCIL, the
 * GL_DEPTH_STENCIL_ATTACHMENT point (one image for both), an empty
 * attachment's type GL_NONE and name 0 (the rest GL_INVALID_OPERATION),
 * and the sizes, component type, colour encoding and layer. */
void ogles2_glGetFramebufferAttachmentParameteriv(struct OGLES2Context *ctx, GLenum target,
                                                  GLenum attachment, GLenum pname, GLint *params)
{
    const struct OGLES2Attachment *at;
    struct OGLES2Framebuffer *fb;
    BOOL es3;
    GLint bits[6];
    int a, which;
    BOOL both = FALSE;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    es3 = ogles2_es3(ctx);
    if (!(which = fb_target(ctx, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    fb = target_fb(ctx, which);
    if (!fb) {
        /* GLES 2.0: the default framebuffer has no attachments to ask */
        GLint *dflt;
        if (!es3) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (attachment != GL_BACK && attachment != 0x1801 /* GL_DEPTH */ && attachment != 0x1802 /* GL_STENCIL */) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            return;
        }
        bits[0] = bits[1] = bits[2] = bits[3] = attachment == GL_BACK ? 8 : 0;
        bits[4] = attachment == 0x1801 ? ctx->depth_bits : 0;
        bits[5] = attachment == 0x1802 ? ctx->stencil_bits : 0;
        dflt = attachment == GL_BACK ? &bits[0] : attachment == 0x1801 ? &bits[4] : &bits[5];
        switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
            *params = *dflt ? GL_FRAMEBUFFER_DEFAULT : GL_NONE;
            return;
        case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:     *params = bits[0]; return;
        case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:   *params = bits[1]; return;
        case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:    *params = bits[2]; return;
        case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:   *params = bits[3]; return;
        case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:   *params = bits[4]; return;
        case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: *params = bits[5]; return;
        case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
            *params = attachment == 0x1802 ? GL_UNSIGNED_INT : GL_UNSIGNED_NORMALIZED;
            return;
        case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
            *params = GL_LINEAR;
            return;
        default:
            ogles2_error(ctx, *dflt ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
            return;
        }
    }
    a = attachment_index(ctx, attachment);
    if (a < 0) {
        ogles2_error(ctx, bad_attachment(ctx, attachment));
        return;
    }
    if (a == ATT_BOTH) { /* one image for both, or GL_INVALID_OPERATION */
        if (!same_image(&fb->att[ATT_DEPTH], &fb->att[ATT_STENCIL])) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (pname == GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        a = ATT_DEPTH;
        both = TRUE; /* its depth and its stencil sizes */
    }
    at = &fb->att[a];
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) {
        *params = (GLint)at->type;
        return;
    }
    if (at->type == GL_NONE) {
        if (es3 && pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
            *params = 0;
            return;
        }
        ogles2_error(ctx, es3 ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
        *params = at->type == GL_TEXTURE ? (GLint)ogles2_texture_name(at->tex) : (GLint)at->rb->name;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
        if (at->type == GL_TEXTURE) {
            *params = at->level;
            return;
        }
        break;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
        if (at->type == GL_TEXTURE) {
            *params = ogles2_texture_target(at->tex) == GL_TEXTURE_CUBE_MAP
                          ? (GLint)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + at->face) : 0;
            return;
        }
        break;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
        if (es3 && at->type == GL_TEXTURE) {
            GLenum tt = ogles2_texture_target(at->tex);
            *params = tt == GL_TEXTURE_3D || tt == GL_TEXTURE_2D_ARRAY ? at->face : 0;
            return;
        }
        break;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT:
        *params = !es3 || att_specified(at) ? component_type(at, a) : GL_NONE;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
        if (es3) {
            struct OGLES2RtFormat rf;
            *params = a < ATT_DEPTH && att_rt(at, &rf) && rf.out == 0 /* SRGB8ALPHA8 */ ? GL_SRGB : GL_LINEAR;
            return;
        }
        break;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
        if (!es3)
            break;
        if (!att_specified(at)) {
            *params = 0;
            return;
        }
        bits[0] = bits[1] = bits[2] = bits[3] = bits[4] = bits[5] = 0;
        if (a < ATT_DEPTH) {
            if (at->type == GL_TEXTURE)
                tex_bits(at, &bits[0], &bits[1], &bits[2], &bits[3]);
            else {
                struct OGLES2RtFormat rf;
                rb_bits(at->rb->format, &bits[0], &bits[1], &bits[2], &bits[3], &bits[4], &bits[5]);
                if (att_rt(at, &rf))
                    rt_bits(&rf, &bits[0], &bits[1], &bits[2], &bits[3]);
            }
        } else if (at->type == GL_RENDERBUFFER) {
            GLint r, g, b, al;
            rb_bits(at->rb->format, &r, &g, &b, &al, &bits[4], &bits[5]);
            if (a == ATT_DEPTH && !both)
                bits[5] = 0;
            else if (a == ATT_STENCIL)
                bits[4] = 0;
        } else {
            UBYTE db;
            BOOL st;
            if (ogles2_texture_depth(at->tex, at->level, &db, &st)) {
                bits[4] = a == ATT_DEPTH ? db : 0;
                bits[5] = (a == ATT_STENCIL || both) && st ? 8 : 0;
            }
        }
        *params = bits[pname - GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE];
        return;
    }
    ogles2_error(ctx, es3 ? GL_INVALID_ENUM : GL_INVALID_ENUM);
}

/* glGet* (state.c asks): the bindings, the limit, the draw framebuffer's bits. */
BOOL ogles2_fbo_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2Framebuffer *fb = ctx->draw_fb;
    GLint r = 8, g = 8, b = 8, a = 8, d, s;
    switch (pname) {
    case GL_FRAMEBUFFER_BINDING:  *v = fb ? (GLint)fb->name : 0; return TRUE; /* = GL_DRAW_FRAMEBUFFER_BINDING */
    case GL_READ_FRAMEBUFFER_BINDING: /* ES 3.0 */
        if (!ogles2_es3(ctx))
            return FALSE;
        *v = ctx->read_fb ? (GLint)ctx->read_fb->name : 0;
        return TRUE;
    case GL_RENDERBUFFER_BINDING: *v = ctx->fbo && ctx->fbo->rb_bound ? (GLint)ctx->fbo->rb_bound->name : 0; return TRUE;
    case GL_MAX_RENDERBUFFER_SIZE: *v = OGLES2_MAX_VIEWPORT; return TRUE;
    case GL_MAX_DRAW_BUFFERS_EXT:
    case GL_MAX_COLOR_ATTACHMENTS_EXT: *v = OGLES2_MAX_DRAW_BUFFERS; return TRUE;
    case GL_READ_BUFFER: /* ES 3.0 */
        if (!ogles2_es3(ctx))
            return FALSE;
        *v = ctx->read_fb ? (GLint)ctx->read_fb->read_buf
                          : ctx->fbo && ctx->fbo->default_read_none ? GL_NONE : GL_BACK;
        return TRUE;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
        if (fb) {
            const struct OGLES2Attachment *c = &fb->att[ATT_COLOR];
            struct OGLES2RtFormat rf;
            r = g = b = a = 0;
            if (c->type == GL_RENDERBUFFER && es3_color(c->rb->format) && att_rt(c, &rf))
                rt_bits(&rf, &r, &g, &b, &a);
            else if (c->type == GL_RENDERBUFFER)
                rb_bits(c->rb->format, &r, &g, &b, &a, &d, &s);
            else if (c->type == GL_TEXTURE)
                tex_bits(c, &r, &g, &b, &a);
        }
        *v = pname == GL_RED_BITS ? r : pname == GL_GREEN_BITS ? g : pname == GL_BLUE_BITS ? b : a;
        return TRUE;
    case GL_DEPTH_BITS:   ogles2_fb_update(ctx); *v = ctx->fb_depth_bits; return TRUE;
    case GL_STENCIL_BITS: ogles2_fb_update(ctx); *v = ctx->fb_stencil_bits; return TRUE;
    default:
        return FALSE;
    }
}

/* aglDestroyContext, after the last job: the framebuffers; the
 * renderbuffers with the share group's last member. */
void ogles2_fbo_free(struct OGLES2Context *ctx)
{
    struct OGLES2FboState *fs = ctx->fbo;
    struct OGLES2Share *sh = ctx->share;
    GLuint i;
    int a;
    /* The attachments let go first: an orphan they keep goes with them. */
    if (fs) {
        for (i = 1; i < fs->nfbs; i++)
            if (fs->fbs[i])
                for (a = 0; a < NATT; a++)
                    att_clear(ctx, &fs->fbs[i]->att[a], TRUE);
        if (fs->copy_fb) {
            for (a = 0; a < NATT; a++)
                att_clear(ctx, &fs->copy_fb->att[a], TRUE);
            fb_free(fs->copy_fb);
            fs->copy_fb = 0;
        }
    }
    if (sh && sh->members == 1) {
        for (i = 1; i < sh->nrbs; i++)
            if (sh->rbs[i]) {
                rb_free_storage(ctx, sh->rbs[i]);
                FreeVec(sh->rbs[i]);
            }
        if (sh->rbs)
            FreeVec(sh->rbs);
        sh->rbs = 0;
        sh->nrbs = 0;
        ogles2_names_free(&sh->far_rb);
    }
    if (!fs)
        return;
    for (i = 1; i < fs->nfbs; i++)
        if (fs->fbs[i])
            fb_free(fs->fbs[i]);
    if (fs->fbs)
        FreeVec(fs->fbs);
    ogles2_names_free(&fs->far_fb);
    FreeVec(fs);
    ctx->fbo = 0;
    ctx->draw_fb = ctx->read_fb = ctx->job_fb = 0;
}
