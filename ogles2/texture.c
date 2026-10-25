/*
 * Textures v1 (plan section 7.6): 2D and cube map texture objects, glTexImage2D and
 * glTexSubImage2D, parameters, glGenerateMipmap, glPixelStorei, and the
 * texture and sampler state the draw path hands to the TMU_CONFIG recipes.
 *
 * Each level keeps an RGBA8 copy in RAM, converted with GL's rules
 * (GL_LUMINANCE -> (L,L,L,1), GL_ALPHA -> (0,0,0,A), the 16-bit formats
 * expanded), for glGenerateMipmap and for re-uploads when the mip chain
 * grows. On the GPU the texture is stored in the hardware format level 0's
 * format and type map to (plan 7.6, Mesa's v3dx_format_table.c: RGB565,
 * RGBA4, RGB5_A1, R8 and RG8 with swizzles, else RGBA8), packed from those
 * copies at upload, with PiStorm3D's layout code (v3dcore's v3d_texture.c:
 * Mesa's level layout, UIF/UBLINEAR/LINEARTILE tiling).
 *
 * A texture changed after a draw of the open job used it is renamed (plan
 * 7.7 hazard 3): the job keeps the old GPU copy, the texture gets a new one.
 * Cube maps: six faces, each with its own levels; on the GPU six copies of
 * the 2D layout one after another (Mesa's cube_map_stride), sampled with
 * an image depth of 6 and that stride.
 *
 * Render to texture (plan 7.7, fbo.c): a level a framebuffer object's job
 * drew into is newer on the GPU than its RAM copy (OGLES2Face.rendered);
 * the copy is read back (sync) before anything works from it.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_texture.h"
#include "v3d_tfu.h"
#include "v3d_gpu_lock.h"

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a control list restarts */

#define MAX_LEVELS 13 /* 4096 = 2^12 */
#define MAX_SIZE   4096

/* The images of one face (a 2D texture has one). */
struct OGLES2Face
{
    UWORD w[MAX_LEVELS], h[MAX_LEVELS];
    ULONG defined;                  /* levels given (bit per level) */
    GLenum format[MAX_LEVELS];      /* each level's internal format (glTexSubImage2D must match it) */
    GLenum type[MAX_LEVELS];        /* and the type it was given in (picks the stored format) */
    UBYTE *shadow[MAX_LEVELS];      /* RGBA8 copy of each level */
    ULONG rendered;                 /* levels the GPU drew into since: the copy is stale */
    ULONG blank;                    /* levels never given texels (glTexImage2D without pixels):
                                     * the copy is zeros, as a new GPU copy is (MEMF_CLEAR) */
    GLenum sized[MAX_LEVELS];       /* ES 3.0: the sized format where format and type do not say the
                                     * storage (sRGB; R11F_G11F_B10F and RGB9_E5 given as floats); else 0 */
};

struct OGLES2Texture
{
    GLuint name;
    GLenum target;                  /* 0 until first bound */
    struct OGLES2SamplerParams s;   /* what a sampler object bound to its unit replaces */
    GLfloat lod_bias;               /* EXT_texture_lod_bias (a texture parameter, as AmigaOS 4's) */
    GLint base_level, max_level;
    ULONG pver;                     /* bumped by every parameter change: the records' check */
    struct OGLES2Face *f;           /* 2D: f[0]; a cube map: +X, -X, +Y, -Y, +Z, -Z (in f6); a 2D
                                     * array's layers, a 3D texture's slices (f6, or allocated) */
    V3DTexture *gpu;                /* NULL until level 0 exists (a cube map: of all faces) */
    UBYTE hw_type, cpp, swizzle[4]; /* the stored format (storage_of), set with gpu */
    ULONG face_stride;              /* a cube map: bytes from one face's levels to the next */
    ULONG used_serial;              /* the job that last drew with it */
    BOOL warned_incomplete;         /* sampled while incomplete: logged once */
    BOOL warned_filter;             /* sampled nearest though linear was asked for: logged once */
    /* The state records last made for a draw (ogles2_texture_state), reused
     * while what they hold is the same: by every later job when they are in
     * the texture's own block (rec), else by the job's later draws (in its
     * state buffer: a job still used the block when they changed). */
    ULONG st_gen;                   /* the control list they are in (g_v3d_cl_generation) */
    APTR st_mem;                    /* the GPU copy they point at */
    ULONG st_pver;                  /* the parameters they were made from */
    ULONG st_tex, st_smp;
    struct OGLES2Context *st_ctx;   /* whose job's state buffer they are in (not st_own) */
    BOOL st_own;                    /* they are in rec */
    v3d_mem rec;                    /* 64 bytes: texture state at 0, sampler state at 32; goes
                                     * with the GPU copy (freed, or deferred when renamed) */
    /* GLES 2.0 4.4.3: deleting a texture detaches it from the deleting
     * context's bound framebuffer only; attached to another, it lives on
     * without its name (orphan) until the last attachment goes. */
    UWORD fbo_refs;                 /* framebuffer attachment points that refer to it */
    BOOL orphan;                    /* deleted while attached: freed with the last of them */
    /* ES 3.0's texture swizzle (GL_TEXTURE_SWIZZLE_R..A: GL_RED..GL_ALPHA,
     * GL_ZERO, GL_ONE), composed with the format's own (swizzle above). */
    GLenum gl_swizzle[4];
    ULONG st_sver;                  /* the records' sampler object's version (0: none) */
    struct OGLES2Face f6[6];
    UWORD nf;                       /* faces f has room for */
    UWORD d[MAX_LEVELS];            /* a 2D array's layers, a 3D texture's depth, by level (ES 3.0;
                                     * a 3D level's slices are faces 0 to d - 1) */
    UBYTE immutable_levels;         /* ES 3.0's glTexStorage*: its levels (0: mutable) */
    BOOL es3_rules;                 /* its images were given in an ES3 context: ES 3.0's depth sampling */
};

/* Both records fit their halves of rec. */
typedef char ogles2_rec_fits[sizeof(v3d_texture_shader_state) <= 32 && sizeof(v3d_sampler_state) <= 32 ? 1 : -1];

/* ---- the context's texture state ---- */

struct OGLES2TexState
{
    struct OGLES2Share *sh;         /* the named textures: sh->tex, sh->ntex (share.c) */
    struct OGLES2Texture default2d, defaultcube, defaultrect; /* name 0: the context's own */
    struct OGLES2Texture default3d, defaultarray; /* ES 3.0 */
    struct OGLES2Texture *unit2d[OGLES2_MAX_TEXTURE_UNITS], *unitcube[OGLES2_MAX_TEXTURE_UNITS];
    struct OGLES2Texture *unitrect[OGLES2_MAX_TEXTURE_UNITS]; /* ARB_texture_rectangle */
    struct OGLES2Texture *unit3d[OGLES2_MAX_TEXTURE_UNITS], *unitarray[OGLES2_MAX_TEXTURE_UNITS]; /* ES 3.0 */
    struct OGLES2Texture fallback;  /* 1x1 (0,0,0,1): what an incomplete texture samples */
    struct OGLES2Texture fallback_int; /* the same in RGBA8UI, for an integer sampler (ES 3.0) */
    UBYTE active;
    BOOL warned_flush;
    BOOL logged_tfu;                /* glGenerateMipmap's first TFU job logged */
    struct OGLES2Texture *scratch;  /* glCopyTex* from the default framebuffer (copy.c) */
    struct OGLES2Sampler *unitsmp[OGLES2_MAX_TEXTURE_UNITS]; /* ES 3.0: glBindSampler's (NULL: none) */
    struct OGLES2Sampler *copy_smp; /* unit 0's, set aside while a copy draws (ogles2_texture_unit0) */
    BOOL copying;
};

/* ES 3.0's sampler objects (stage B item 7): bound to a unit, sampled with
 * instead of the unit's texture's parameters. */
struct OGLES2Sampler
{
    GLuint name;
    struct OGLES2SamplerParams s;
    ULONG ver;                      /* its parameters' version, unique across samplers (smp_serial):
                                     * the texture records' check (st_sver) */
};

static ULONG smp_serial; /* the sampler versions given out */

static void sync(struct OGLES2Texture *t);
static void rect_defaults(struct OGLES2Texture *t);
static void put_level(struct OGLES2Face *f, int level, UBYTE *img, GLsizei width, GLsizei height, GLenum format,
                      GLenum type, BOOL given, GLenum sized);
static void drop_level(struct OGLES2Face *f, int level);
static int int_components(GLenum format);

/* A texture's and a new sampler object's sampling parameters. */
static void smp_defaults(struct OGLES2SamplerParams *sp)
{
    sp->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    sp->mag_filter = GL_LINEAR;
    sp->wrap_s = sp->wrap_t = sp->wrap_r = GL_REPEAT;
    sp->max_aniso = 1.0f;
    sp->min_lod = -1000.0f;
    sp->max_lod = 1000.0f;
    sp->compare_mode = GL_NONE;
    sp->compare_func = GL_LEQUAL;
}

static void init_object(struct OGLES2Texture *t, GLuint name, GLenum target)
{
    memset(t, 0, sizeof(*t));
    t->f = t->f6;
    t->nf = 6;
    t->name = name;
    t->target = target;
    smp_defaults(&t->s);
    t->max_level = 1000;
    t->gl_swizzle[0] = GL_RED;
    t->gl_swizzle[1] = GL_GREEN;
    t->gl_swizzle[2] = GL_BLUE;
    t->gl_swizzle[3] = GL_ALPHA;
    rect_defaults(t);
}

/* ARB_texture_rectangle's defaults: a linear minification filter (no
 * mipmaps) and clamped wraps. Also when a new name is first bound there. */
static void rect_defaults(struct OGLES2Texture *t)
{
    if (t->target != GL_TEXTURE_RECTANGLE)
        return;
    t->s.min_filter = GL_LINEAR;
    t->s.wrap_s = t->s.wrap_t = GL_CLAMP_TO_EDGE;
}

static BOOL cpu_uploads; /* probe U of the context last seen here (a test switch): store_tfu */

static struct OGLES2TexState *ts_get(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    int i;
    cpu_uploads = (ctx->probe & OGLES2_PROBE_CPU_UPLOAD) != 0;
    if (ts)
        return ts;
    if (!(ts = AllocVec(sizeof(*ts), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    ts->sh = ctx->share;
    init_object(&ts->default2d, 0, GL_TEXTURE_2D);
    init_object(&ts->defaultcube, 0, GL_TEXTURE_CUBE_MAP);
    init_object(&ts->defaultrect, 0, GL_TEXTURE_RECTANGLE);
    init_object(&ts->default3d, 0, GL_TEXTURE_3D);
    init_object(&ts->defaultarray, 0, GL_TEXTURE_2D_ARRAY);
    for (i = 0; i < OGLES2_MAX_TEXTURE_UNITS; i++) {
        ts->unit2d[i] = &ts->default2d;
        ts->unitcube[i] = &ts->defaultcube;
        ts->unitrect[i] = &ts->defaultrect;
        ts->unit3d[i] = &ts->default3d;
        ts->unitarray[i] = &ts->defaultarray;
    }
    ctx->tex = ts;
    return ts;
}

/* No job uses the GPU copy any more (before_change renamed it or rendered
 * the job): the copy and the records' block. */
static void free_gpu(struct OGLES2Texture *t)
{
    if (t->gpu) {
        if (t->gpu->texture_mem.hostptr)
            v3d_texture_free(ogles2_device(), t->gpu);
        FreeVec(t->gpu);
        t->gpu = 0;
    }
    if (t->rec.hostptr)
        v3d_mem_free(ogles2_device(), &t->rec);
    t->st_own = FALSE;
}

static void free_levels(struct OGLES2Texture *t)
{
    int k, l;
    for (k = 0; k < t->nf; k++) {
        for (l = 0; l < MAX_LEVELS; l++) {
            if (t->f[k].shadow[l])
                FreeVec(t->f[k].shadow[l]);
            t->f[k].shadow[l] = 0;
        }
        t->f[k].defined = 0;
        t->f[k].blank = 0;
    }
    if (t->f != t->f6)
        FreeVec(t->f);
    t->f = t->f6;
    t->nf = 6;
    memset(t->d, 0, sizeof(t->d));
    free_gpu(t);
}

/* aglDestroyContext (its jobs finished): the context's own textures; the
 * named ones with the share group's last member. */
void ogles2_textures_free(struct OGLES2Context *ctx)
{
    struct OGLES2TexState *ts = ctx->tex;
    struct OGLES2Share *sh = ctx->share;
    GLuint i;
    if (sh && sh->members == 1) {
        for (i = 1; i < sh->ntex; i++)
            if (sh->tex[i]) {
                free_levels(sh->tex[i]);
                FreeVec(sh->tex[i]);
            }
        if (sh->tex)
            FreeVec(sh->tex);
        sh->tex = 0;
        sh->ntex = 0;
        ogles2_names_free(&sh->far_tex);
        for (i = 1; i < sh->nsmp; i++)
            if (sh->smp[i])
                FreeVec(sh->smp[i]);
        if (sh->smp)
            FreeVec(sh->smp);
        sh->smp = 0;
        sh->nsmp = 0;
    }
    if (!ts)
        return;
    free_levels(&ts->default2d);
    free_levels(&ts->defaultcube);
    free_levels(&ts->defaultrect);
    free_levels(&ts->default3d);
    free_levels(&ts->defaultarray);
    free_levels(&ts->fallback);
    free_levels(&ts->fallback_int);
    ogles2_texture_hidden_free(ctx, ts->scratch);
    FreeVec(ts);
    ctx->tex = 0;
}

/* A texture is about to change while a draw of the open job uses it (plan
 * 7.7 hazard 3): that draw must still read the old contents. Renaming, as
 * Mesa does: the GPU copy goes to the job (freed when the frame has
 * rendered, v3d_frame_defer_free), and the texture gets a new one, rebuilt
 * from the RAM copies at its next upload or draw. Only when the job's
 * retire list is nearly full is the job rendered first instead, which
 * costs a wait and the frame's depth and stencil (plan 7.2). */
static void before_change(struct OGLES2Context *ctx, struct OGLES2TexState *ts,
                          struct OGLES2Texture *t)
{
    struct OGLES2Context *m, *user;
    V3DContext *v;
    /* Its records are made again at the next draw: a new level can change
     * the level count without a new GPU copy. */
    t->st_own = FALSE;
    t->st_gen = 0;
    /* An open job draws into it (a framebuffer object's colour image), any
     * member's of the share group: rendered first. Then the RAM copies are
     * made current. */
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fbo_texture_changing(m, t);
    sync(t);
    /* A job not rendered yet uses it: an open one, or one submitted and
     * still running (M6 overlap), of any member: the old copy goes with
     * that job (share.c: serials are unique across contexts). */
    if (!t->used_serial || !(user = ogles2_job_user(ctx, t->used_serial)) || !(v = user->v3d))
        return;
    if (t->gpu && v->frame.retire_count < V3D_RETIRE_MAX - 8) {
        if (t->gpu->texture_mem.hostptr)
            v3d_frame_defer_free(&v->frame, &t->gpu->texture_mem);
        FreeVec(t->gpu);
        t->gpu = 0;
        if (t->rec.hostptr) { /* the job's draws read their records there too */
            v3d_frame_defer_free(&v->frame, &t->rec);
            memset(&t->rec, 0, sizeof(t->rec));
        }
        t->st_own = FALSE;
        t->used_serial = 0;
        ctx->n_renames++;
        if (!ts->warned_flush) {
            ts->warned_flush = TRUE;
            ogles2_log("texture %ld changed after a draw used it in the same frame: renamed "
                       "(logged once)", (ULONG)t->name);
        }
        return;
    }
    ogles2_log("texture %ld changed after a draw used it: the frame is rendered first "
               "(too many renamed blocks in this frame)", (ULONG)t->name);
    ogles2_job_flush(user, FALSE);
    ogles2_job_finish(user);
}

/* ---- names and binding ---- */

/* Makes `name` a valid index of the name table. */
static BOOL table_room(struct OGLES2TexState *ts, GLuint name)
{
    GLuint nn, j;
    struct OGLES2Texture **tab;
    if (name < ts->sh->ntex)
        return TRUE;
    nn = ts->sh->ntex ? ts->sh->ntex : 32;
    while (nn <= name)
        nn *= 2;
    if (!(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < ts->sh->ntex; j++)
        tab[j] = ts->sh->tex[j];
    if (ts->sh->tex)
        FreeVec(ts->sh->tex);
    ts->sh->tex = tab;
    ts->sh->ntex = nn;
    return TRUE;
}

/* A new texture object named `name` (no object has it). */
static struct OGLES2Texture *create(struct OGLES2TexState *ts, GLuint name)
{
    struct OGLES2Texture *t;
    GLuint slot = ogles2_name_slot(&ts->sh->far_tex, name, TRUE);
    if (slot == OGLES2_NO_SLOT || !table_room(ts, slot) ||
        !(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_name_forget(&ts->sh->far_tex, name);
        return 0;
    }
    init_object(t, name, 0);
    ts->sh->tex[slot] = t;
    return t;
}

void ogles2_glGenTextures(struct OGLES2Context *ctx, GLsizei n, GLuint *textures)
{
    struct OGLES2TexState *ts;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(ts = ts_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        while (i < ts->sh->ntex && ts->sh->tex[i])
            i++;
        if (i >= OGLES2_NEAR_NAMES || !create(ts, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (textures)
            textures[k] = i;
    }
}

static struct OGLES2Texture *by_name(struct OGLES2TexState *ts, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&ts->sh->far_tex, name, FALSE) : OGLES2_NO_SLOT;
    return slot < ts->sh->ntex ? ts->sh->tex[slot] : 0;
}

/* A deleted texture's memory goes (its name already has). */
static void destroy(struct OGLES2Context *ctx, struct OGLES2TexState *ts, struct OGLES2Texture *t)
{
    int u;
    for (u = 0; u < t->nf; u++)
        t->f[u].rendered = 0; /* going: nothing to read back (SDL's render targets: 38 ms) */
    before_change(ctx, ts, t); /* a queued draw must not read it; a job drawing into it rendered */
    free_levels(t);
    FreeVec(t);
}

void ogles2_glDeleteTextures(struct OGLES2Context *ctx, GLsizei n, const GLuint *textures)
{
    struct OGLES2TexState *ts;
    struct OGLES2Context *m;
    GLsizei k;
    int u;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(ts = ts_get(ctx)) || !textures)
        return;
    ogles2_trace(ctx, "glDeleteTextures", n, n ? textures[0] : 0);
    for (k = 0; k < n; k++) {
        struct OGLES2Texture *t = by_name(ts, textures[k]);
        if (!t)
            continue;
        /* Detached from this context's bound framebuffer (its job drawing
         * into it rendered first); other framebuffers keep it (GLES 2.0
         * 4.4.3). Every member of the share group unbinds it. */
        ogles2_fbo_texture_deleted(ctx, t);
        for (m = ctx->share->first; m; m = m->share_next) {
            struct OGLES2TexState *mts = m->tex;
            if (!mts)
                continue;
            for (u = 0; u < OGLES2_MAX_TEXTURE_UNITS; u++) { /* deleting unbinds (GLES 2.0 3.7.13) */
                if (mts->unit2d[u] == t)
                    mts->unit2d[u] = &mts->default2d;
                if (mts->unitcube[u] == t)
                    mts->unitcube[u] = &mts->defaultcube;
                if (mts->unitrect[u] == t)
                    mts->unitrect[u] = &mts->defaultrect;
                if (mts->unit3d[u] == t)
                    mts->unit3d[u] = &mts->default3d;
                if (mts->unitarray[u] == t)
                    mts->unitarray[u] = &mts->defaultarray;
            }
        }
        ts->sh->tex[ogles2_name_slot(&ts->sh->far_tex, t->name, FALSE)] = 0;
        ogles2_name_forget(&ts->sh->far_tex, t->name);
        if (t->fbo_refs)
            t->orphan = TRUE; /* still attached: lives on, nameless */
        else
            destroy(ctx, ts, t);
    }
}

/* fbo.c: an attachment point refers to t, or no longer does. An orphan goes
 * with its last attachment; at aglDestroyContext (`now`: the jobs are
 * finished) without waiting for jobs. */
void ogles2_texture_attach(struct OGLES2Texture *t)
{
    t->fbo_refs++;
}

void ogles2_texture_detach(struct OGLES2Context *ctx, struct OGLES2Texture *t, BOOL now)
{
    struct OGLES2TexState *ts;
    if (t->fbo_refs)
        t->fbo_refs--;
    if (t->fbo_refs || !t->orphan)
        return;
    if (now || !(ts = ts_get(ctx))) {
        free_levels(t);
        FreeVec(t);
        return;
    }
    destroy(ctx, ts, t);
}

GLboolean ogles2_glIsTexture(struct OGLES2Context *ctx, GLuint texture)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return GL_FALSE;
    t = by_name(ts, texture);
    return t && t->target ? GL_TRUE : GL_FALSE;
}

void ogles2_glActiveTexture(struct OGLES2Context *ctx, GLenum texture)
{
    struct OGLES2TexState *ts;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + OGLES2_MAX_TEXTURE_UNITS) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ts->active = (UBYTE)(texture - GL_TEXTURE0);
}

void ogles2_glBindTexture(struct OGLES2Context *ctx, GLenum target, GLuint texture)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP && target != GL_TEXTURE_RECTANGLE &&
        !(ogles2_es3(ctx) && (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!texture) {
        t = target == GL_TEXTURE_2D ? &ts->default2d
          : target == GL_TEXTURE_CUBE_MAP ? &ts->defaultcube
          : target == GL_TEXTURE_3D ? &ts->default3d
          : target == GL_TEXTURE_2D_ARRAY ? &ts->defaultarray : &ts->defaultrect;
    } else {
        t = by_name(ts, texture);
        /* GLES2 lets a program bind a name glGenTextures did not return. */
        if (!t && !(t = create(ts, texture))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (t->target && t->target != target) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        if (!t->target) {
            t->target = target;
            rect_defaults(t);
        }
    }
    if (target == GL_TEXTURE_2D)
        ts->unit2d[ts->active] = t;
    else if (target == GL_TEXTURE_CUBE_MAP)
        ts->unitcube[ts->active] = t;
    else if (target == GL_TEXTURE_3D)
        ts->unit3d[ts->active] = t;
    else if (target == GL_TEXTURE_2D_ARRAY)
        ts->unitarray[ts->active] = t;
    else
        ts->unitrect[ts->active] = t;
}

/* ---- parameters ---- */

static struct OGLES2Texture *bound(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_CUBE_MAP)
        return ts->unitcube[ts->active];
    if (target == GL_TEXTURE_RECTANGLE)
        return ts->unitrect[ts->active];
    if (ogles2_es3(ctx) && target == GL_TEXTURE_3D)
        return ts->unit3d[ts->active];
    if (ogles2_es3(ctx) && target == GL_TEXTURE_2D_ARRAY)
        return ts->unitarray[ts->active];
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

/* Unit `unit`'s texture of a sampler kind (OGLES2_TEX_*, not NONE). */
static struct OGLES2Texture *unit_texture(struct OGLES2TexState *ts, int kind, int unit)
{
    kind &= ~OGLES2_TEX_INT;
    return kind == OGLES2_TEX_CUBE    ? ts->unitcube[unit]
         : kind == OGLES2_TEX_RECT    ? ts->unitrect[unit]
         : kind == OGLES2_TEX_3D      ? ts->unit3d[unit]
         : kind == OGLES2_TEX_ARRAY   ? ts->unitarray[unit] : ts->unit2d[unit];
}

/* The largest anisotropy (the sampler's 2, 4, 8 or 16) and LOD bias (its
 * s8.8 field holds more; Mesa's v3d screen offers 15). */
#define MAX_ANISOTROPY 16.0f
#define MAX_LOD_BIAS   15.0f

/* A sampler parameter (ES 3.0 3.8.2: a texture's and a sampler object's)
 * given as `i` (gl*Parameteri*) or `f` (gl*Parameterf*): enums read i, the
 * float parameters f (GL converts either way). GL_NO_ERROR, or the error
 * (GL_INVALID_ENUM for a pname that is none). */
static GLenum smp_param(struct OGLES2Context *ctx, struct OGLES2SamplerParams *sp, GLenum pname, GLint i,
                        GLfloat f)
{
    GLenum e = (GLenum)i;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST &&
            e != GL_LINEAR_MIPMAP_NEAREST && e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR)
            return GL_INVALID_ENUM;
        sp->min_filter = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_MAG_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR)
            return GL_INVALID_ENUM;
        sp->mag_filter = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_WRAP_R: /* ES 3.0 */
        if (!ogles2_es3(ctx))
            return GL_INVALID_ENUM;
        /* fall through */
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (e != GL_REPEAT && e != GL_CLAMP_TO_EDGE && e != GL_MIRRORED_REPEAT && e != GL_MIRROR_CLAMP_TO_EDGE)
            return GL_INVALID_ENUM;
        if (pname == GL_TEXTURE_WRAP_S)
            sp->wrap_s = e;
        else if (pname == GL_TEXTURE_WRAP_T)
            sp->wrap_t = e;
        else
            sp->wrap_r = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
        if (!(f >= 1.0f)) /* EXT_texture_filter_anisotropic: below 1 (or NaN) */
            return GL_INVALID_VALUE;
        sp->max_aniso = f > MAX_ANISOTROPY ? MAX_ANISOTROPY : f;
        return GL_NO_ERROR;
    case GL_TEXTURE_MIN_LOD:
        sp->min_lod = f;
        return GL_NO_ERROR;
    case GL_TEXTURE_MAX_LOD:
        sp->max_lod = f;
        return GL_NO_ERROR;
    /* ES 3.0's depth comparison (stage B item 8) */
    case GL_TEXTURE_COMPARE_MODE:
        if (!ogles2_es3(ctx) || (e != GL_NONE && e != GL_COMPARE_REF_TO_TEXTURE))
            return GL_INVALID_ENUM;
        sp->compare_mode = e;
        return GL_NO_ERROR;
    case GL_TEXTURE_COMPARE_FUNC:
        if (!ogles2_es3(ctx) || e < GL_NEVER || e > GL_ALWAYS)
            return GL_INVALID_ENUM;
        sp->compare_func = e;
        return GL_NO_ERROR;
    default:
        return GL_INVALID_ENUM;
    }
}

/* A sampler parameter's value, as a float; FALSE if pname is none. */
static BOOL smp_get(struct OGLES2Context *ctx, const struct OGLES2SamplerParams *sp, GLenum pname, GLfloat *v)
{
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:          *v = (GLfloat)sp->min_filter; return TRUE;
    case GL_TEXTURE_MAG_FILTER:          *v = (GLfloat)sp->mag_filter; return TRUE;
    case GL_TEXTURE_WRAP_S:              *v = (GLfloat)sp->wrap_s; return TRUE;
    case GL_TEXTURE_WRAP_T:              *v = (GLfloat)sp->wrap_t; return TRUE;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:  *v = sp->max_aniso; return TRUE;
    case GL_TEXTURE_MIN_LOD:             *v = sp->min_lod; return TRUE;
    case GL_TEXTURE_MAX_LOD:             *v = sp->max_lod; return TRUE;
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_COMPARE_FUNC:
        if (!ogles2_es3(ctx))
            return FALSE;
        *v = (GLfloat)(pname == GL_TEXTURE_WRAP_R ? sp->wrap_r
                       : pname == GL_TEXTURE_COMPARE_MODE ? sp->compare_mode : sp->compare_func);
        return TRUE;
    default:
        return FALSE;
    }
}

/* glGet*Parameteriv: enums and levels exact, the float parameters rounded. */
static BOOL exact_param(GLenum pname)
{
    return pname != GL_TEXTURE_MAX_ANISOTROPY_EXT && pname != GL_TEXTURE_LOD_BIAS &&
           pname != GL_TEXTURE_MIN_LOD && pname != GL_TEXTURE_MAX_LOD;
}

/* A texture parameter given as `i` or `f` (as smp_param's). */
static void tex_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint i, GLfloat f)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum e = (GLenum)i, err;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return;
    /* ARB_texture_rectangle: no mipmapping, clamped coordinates only (of
     * desktop GL's three, the one GLES2 has), base level 0. */
    if (target == GL_TEXTURE_RECTANGLE) {
        if ((pname == GL_TEXTURE_MIN_FILTER && e != GL_NEAREST && e != GL_LINEAR) ||
            ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) && e != GL_CLAMP_TO_EDGE)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            return;
        }
        if (pname == GL_TEXTURE_BASE_LEVEL && i != 0) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    switch (pname) {
    case GL_TEXTURE_LOD_BIAS:
        t->lod_bias = f; /* clamped where it is used (GL keeps the value) */
        t->pver++;
        return;
    case GL_TEXTURE_BASE_LEVEL:
    case GL_TEXTURE_MAX_LEVEL:
        if (i < 0) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        if (pname == GL_TEXTURE_BASE_LEVEL)
            t->base_level = i;
        else
            t->max_level = i;
        t->pver++;
        return;
    /* ES 3.0's swizzle (stage B item 8) */
    case GL_TEXTURE_SWIZZLE_R:
    case GL_TEXTURE_SWIZZLE_G:
    case GL_TEXTURE_SWIZZLE_B:
    case GL_TEXTURE_SWIZZLE_A:
        if (!ogles2_es3(ctx) ||
            (e != GL_RED && e != GL_GREEN && e != GL_BLUE && e != GL_ALPHA && e != GL_ZERO && e != GL_ONE))
            break;
        t->gl_swizzle[pname - GL_TEXTURE_SWIZZLE_R] = e;
        t->pver++;
        return;
    default:
        if ((err = smp_param(ctx, &t->s, pname, i, f)) != GL_NO_ERROR) {
            ogles2_error(ctx, err);
            return;
        }
        t->pver++;
        return;
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
}

/* GL's conversion of a float to an integer parameter: the nearest. */
static GLint round_param(GLfloat f)
{
    return f < 0.0f ? -(GLint)(0.5f - f) : (GLint)(f + 0.5f);
}

void ogles2_glTexParameteri(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint param)
{
    tex_param(ctx, target, pname, param, (GLfloat)param);
}

void ogles2_glTexParameterf(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat param)
{
    tex_param(ctx, target, pname, round_param(param), param);
}

void ogles2_glTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLint *params)
{
    if (params)
        tex_param(ctx, target, pname, params[0], (GLfloat)params[0]);
}

void ogles2_glTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                             const GLfloat *params)
{
    if (params)
        tex_param(ctx, target, pname, round_param(params[0]), params[0]);
}

/* A parameter's value, as a float; FALSE (GL_INVALID_ENUM set) if pname is none. */
static BOOL get_param(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLfloat *v)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)) || !(t = bound(ctx, ts, target)))
        return FALSE;
    switch (pname) {
    case GL_TEXTURE_LOD_BIAS:            *v = t->lod_bias; return TRUE;
    case GL_TEXTURE_IMMUTABLE_FORMAT:
    case GL_TEXTURE_IMMUTABLE_LEVELS:
        if (!ogles2_es3(ctx))
            break;
        *v = pname == GL_TEXTURE_IMMUTABLE_LEVELS ? (GLfloat)t->immutable_levels
                                                  : (t->immutable_levels ? 1.0f : 0.0f);
        return TRUE;
    case GL_TEXTURE_BASE_LEVEL:          *v = (GLfloat)t->base_level; return TRUE;
    case GL_TEXTURE_MAX_LEVEL:           *v = (GLfloat)t->max_level; return TRUE;
    case GL_TEXTURE_SWIZZLE_R:
    case GL_TEXTURE_SWIZZLE_G:
    case GL_TEXTURE_SWIZZLE_B:
    case GL_TEXTURE_SWIZZLE_A:
        if (!ogles2_es3(ctx))
            break;
        *v = (GLfloat)t->gl_swizzle[pname - GL_TEXTURE_SWIZZLE_R];
        return TRUE;
    default:
        if (smp_get(ctx, &t->s, pname, v))
            return TRUE;
        break;
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
    return FALSE;
}

void ogles2_glGetTexParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLint *params)
{
    GLfloat v;
    if (params && get_param(ctx, target, pname, &v))
        *params = exact_param(pname) ? (GLint)(ULONG)v : round_param(v);
}

void ogles2_glGetTexParameterfv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                GLfloat *params)
{
    GLfloat v;
    if (params && get_param(ctx, target, pname, &v))
        *params = v;
}

void ogles2_glPixelStorei(struct OGLES2Context *ctx, GLenum pname, GLint param)
{
    GLint *v;
    if (!ogles2_context_valid(ctx))
        return;
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:
    case GL_PACK_ALIGNMENT:
        if (param != 1 && param != 2 && param != 4 && param != 8) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        (pname == GL_UNPACK_ALIGNMENT ? &ctx->unpack : &ctx->pack)->alignment = param;
        return;
    /* ES 3.0's (3.7.1, 4.3.1) */
    case GL_UNPACK_ROW_LENGTH:   v = &ctx->unpack.row_length; break;
    case GL_UNPACK_IMAGE_HEIGHT: v = &ctx->unpack.image_height; break;
    case GL_UNPACK_SKIP_PIXELS:  v = &ctx->unpack.skip_pixels; break;
    case GL_UNPACK_SKIP_ROWS:    v = &ctx->unpack.skip_rows; break;
    case GL_UNPACK_SKIP_IMAGES:  v = &ctx->unpack.skip_images; break;
    case GL_PACK_ROW_LENGTH:     v = &ctx->pack.row_length; break;
    case GL_PACK_SKIP_PIXELS:    v = &ctx->pack.skip_pixels; break;
    case GL_PACK_SKIP_ROWS:      v = &ctx->pack.skip_rows; break;
    default:                     v = 0; break;
    }
    if (!v || !ogles2_es3(ctx)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (param < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    *v = param;
}

/* glGet* for the texture state (state.c asks). */
BOOL ogles2_texture_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    switch (pname) {
    case GL_ACTIVE_TEXTURE:       *v = (GLint)(GL_TEXTURE0 + (ts ? ts->active : 0)); return TRUE;
    case GL_TEXTURE_BINDING_2D:   *v = ts ? (GLint)ts->unit2d[ts->active]->name : 0; return TRUE;
    case GL_TEXTURE_BINDING_CUBE_MAP: *v = ts ? (GLint)ts->unitcube[ts->active]->name : 0; return TRUE;
    case GL_TEXTURE_BINDING_RECTANGLE: *v = ts ? (GLint)ts->unitrect[ts->active]->name : 0; return TRUE;
    case GL_TEXTURE_BINDING_3D:
    case GL_TEXTURE_BINDING_2D_ARRAY:
        if (!ogles2_es3(ctx))
            return FALSE;
        *v = !ts ? 0 : (GLint)(pname == GL_TEXTURE_BINDING_3D ? ts->unit3d : ts->unitarray)[ts->active]->name;
        return TRUE;
    case GL_SAMPLER_BINDING:
        if (!ogles2_es3(ctx))
            return FALSE;
        *v = ts && ts->unitsmp[ts->active] ? (GLint)ts->unitsmp[ts->active]->name : 0;
        return TRUE;
    case GL_MAX_RECTANGLE_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_UNPACK_ALIGNMENT:     *v = ctx->unpack.alignment; return TRUE;
    case GL_PACK_ALIGNMENT:       *v = ctx->pack.alignment; return TRUE;
    case GL_UNPACK_ROW_LENGTH:
    case GL_UNPACK_IMAGE_HEIGHT:
    case GL_UNPACK_SKIP_PIXELS:
    case GL_UNPACK_SKIP_ROWS:
    case GL_UNPACK_SKIP_IMAGES:
    case GL_PACK_ROW_LENGTH:
    case GL_PACK_SKIP_PIXELS:
    case GL_PACK_SKIP_ROWS:
        if (!ogles2_es3(ctx))
            return FALSE;
        *v = pname == GL_UNPACK_ROW_LENGTH     ? ctx->unpack.row_length
           : pname == GL_UNPACK_IMAGE_HEIGHT   ? ctx->unpack.image_height
           : pname == GL_UNPACK_SKIP_PIXELS    ? ctx->unpack.skip_pixels
           : pname == GL_UNPACK_SKIP_ROWS      ? ctx->unpack.skip_rows
           : pname == GL_UNPACK_SKIP_IMAGES    ? ctx->unpack.skip_images
           : pname == GL_PACK_ROW_LENGTH       ? ctx->pack.row_length
           : pname == GL_PACK_SKIP_PIXELS      ? ctx->pack.skip_pixels : ctx->pack.skip_rows;
        return TRUE;
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = MAX_SIZE; return TRUE;
    case GL_MAX_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_STAGE_SAMPLERS; return TRUE;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_TEXTURE_UNITS; return TRUE;
    /* Vertex texture fetch: the draws bind the units of both stages' samplers
     * (draw.c texture_states); dEQP-GLES2's texture.vertex tests sample in
     * the vertex shader (M7). Units are shared between the stages. */
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *v = OGLES2_MAX_STAGE_SAMPLERS; return TRUE;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: *v = (GLint)MAX_ANISOTROPY; return TRUE;
    case GL_MAX_TEXTURE_LOD_BIAS: *v = (GLint)MAX_LOD_BIAS; return TRUE;
    default:                      return FALSE;
    }
}

/* ---- images ---- */

/* What a level's RAM copy holds, by its format and type:
 *  - RGBA8: RGBA bytes, GL's conversions (the 16-bit packings expanded);
 *  - FLOAT: RGBA floats, the 68k's own (OES_texture_float, _half_float);
 *  - DEPTH16, DEPTH24: the stored depth word as a ULONG (OES_depth_texture:
 *    16 bits; or 24 bits << 8 with the stencil in the low byte, which is
 *    also OES_packed_depth_stencil's UNSIGNED_INT_24_8 layout);
 *  - BLOCKS: S3TC's 4x4 blocks as given, rows of ceil(w/4) blocks (M7,
 *    docs/M7_EXTENSIONS_PLAN.md step 3), stored as they are: the TMU
 *    decodes them.
 * The levels of a texture must be of level 0's class to be sampled. */
#define CLASS_RGBA8   0
#define CLASS_FLOAT   1
#define CLASS_DEPTH16 2
#define CLASS_DEPTH24 3
#define CLASS_BLOCKS  4

/* EXT_texture_compression_dxt1, EXT_texture_compression_s3tc and ANGLE's
 * dxt3/dxt5 (the same enums): a block's bytes, 0 for another format. */
/* The bytes of a 4x4 block of a format the TMU samples compressed: S3TC,
 * ETC1 and ES 3.0's ETC2/EAC (docs/GLES3_PLAN.md §5 item 9); 0 for another. */
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
static ULONG dxt_block_bytes(GLenum format)
{
    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case GL_ETC1_RGB8_OES:
    case GL_COMPRESSED_RGB8_ETC2:
    case GL_COMPRESSED_SRGB8_ETC2:
    case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
    case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
    case GL_COMPRESSED_R11_EAC:
    case GL_COMPRESSED_SIGNED_R11_EAC:
        return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    case GL_COMPRESSED_RGBA8_ETC2_EAC:
    case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
    case GL_COMPRESSED_RG11_EAC:
    case GL_COMPRESSED_SIGNED_RG11_EAC:
        return 16;
    default:
        return 0;
    }
}

/* ES 3.0's ETC2/EAC formats (GLES2 contexts: GL_INVALID_ENUM). */
static BOOL etc2_format(GLenum format)
{
    return format >= GL_COMPRESSED_R11_EAC && format <= GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC;
}

static BOOL srgb_etc2(GLenum format)
{
    return format == GL_COMPRESSED_SRGB8_ETC2 || format == GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 ||
           format == GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC;
}

/* Texels in blocks of 4, rounded up (a level smaller than 4x4 is one block). */
static ULONG blocks4(ULONG texels)
{
    return (texels + 3) / 4;
}

static int class_of(GLenum format, GLenum type)
{
    if (dxt_block_bytes(format))
        return CLASS_BLOCKS;
    if (int_components(format))
        return CLASS_FLOAT; /* four 32-bit integers a texel (convert_int) */
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES)
        return type == GL_UNSIGNED_SHORT ? CLASS_DEPTH16 : CLASS_DEPTH24; /* 32F depth: its float's bits */
    /* ES 3.0's signed bytes and packed words: floats, encoded when stored */
    if (type == GL_FLOAT || type == GL_HALF_FLOAT_OES || type == GL_BYTE || type == GL_UNSIGNED_INT_2_10_10_10_REV ||
        type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == GL_UNSIGNED_INT_5_9_9_9_REV)
        return CLASS_FLOAT;
    if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT)
        return CLASS_DEPTH16;
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES)
        return CLASS_DEPTH24;
    return CLASS_RGBA8;
}

/* Bytes per texel of a RAM copy of that class (BLOCKS is never asked: its
 * copies are only stored and replaced, never converted, filtered or read). */
static ULONG copy_bytes(int cls)
{
    return cls == CLASS_FLOAT ? 16 : 4;
}

static int level_class(const struct OGLES2Face *f, int l)
{
    return class_of(f->format[l], f->type[l]);
}

/* Set once an image whose texture the TMU returns 32 bits for (32-bit
 * floats, depth) was given: from then on draws look at their samplers'
 * textures to pick the program's 32-bit variants (draw.c). */
ULONG ogles2_wide_seen;

static ULONG wide_class(GLenum format, GLenum type)
{
    if (int_components(format)) /* 32-bit integers return 32 bits, the others 16 (Mesa's table) */
        return type == GL_INT || type == GL_UNSIGNED_INT;
    return type == GL_FLOAT || format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES;
}

/* A level's texels return 32 bits (wide_class), but for the packed floats
 * given as floats (16-bit returns, Mesa's table). */
static BOOL level_wide(const struct OGLES2Face *f, int l)
{
    return wide_class(f->format[l], f->type[l]) && f->sized[l] != GL_R11F_G11F_B10F && f->sized[l] != GL_RGB9_E5;
}

/* Bytes per pixel of a valid format/type pair; 0 and the error to set if not. */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1 /* EXT_texture_format_BGRA8888 */
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

static int components(GLenum format)
{
    switch (format) {
    case GL_RGBA:            return 4;
    case GL_RGB:             return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_LUMINANCE:
    case GL_ALPHA:           return 1;
    case GL_RG:              return 2; /* ES 3.0's (stage B item 8), in ES3 contexts only (rg_refused) */
    case GL_RED:             return 1;
    default:                 return 0;
    }
}

/* ES 3.0's formats and types in a GLES2 context: GL_RED and GL_RG
 * (EXT_texture_rg is not offered there), signed bytes, the packed 32-bit
 * types, float depth. */
static BOOL rg_refused(struct OGLES2Context *ctx, GLenum format, GLenum type)
{
    if (!ogles2_es3(ctx) &&
        (format == GL_RED || format == GL_RG || int_components(format) || type == GL_BYTE ||
         type == GL_UNSIGNED_INT_2_10_10_10_REV ||
         type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == GL_UNSIGNED_INT_5_9_9_9_REV ||
         type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV || (format == GL_DEPTH_COMPONENT && type == GL_FLOAT))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return TRUE;
    }
    return FALSE;
}

/* ES 3.0's integer formats (*_INTEGER): their components; 0 for another. */
static int int_components(GLenum format)
{
    switch (format) {
    case GL_RED_INTEGER:  return 1;
    case GL_RG_INTEGER:   return 2;
    case GL_RGB_INTEGER:  return 3;
    case GL_RGBA_INTEGER: return 4;
    default:              return 0;
    }
}

/* An integer sized format's format and type (ES 3.0 table 3.2). */
static BOOL int_sized(GLenum ifmt, GLenum *format, GLenum *type)
{
    static const GLenum tab[][3] = {
        { GL_R8I, GL_RED_INTEGER, GL_BYTE }, { GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE },
        { GL_R16I, GL_RED_INTEGER, GL_SHORT }, { GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT },
        { GL_R32I, GL_RED_INTEGER, GL_INT }, { GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT },
        { GL_RG8I, GL_RG_INTEGER, GL_BYTE }, { GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE },
        { GL_RG16I, GL_RG_INTEGER, GL_SHORT }, { GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT },
        { GL_RG32I, GL_RG_INTEGER, GL_INT }, { GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT },
        { GL_RGB8I, GL_RGB_INTEGER, GL_BYTE }, { GL_RGB8UI, GL_RGB_INTEGER, GL_UNSIGNED_BYTE },
        { GL_RGB16I, GL_RGB_INTEGER, GL_SHORT }, { GL_RGB16UI, GL_RGB_INTEGER, GL_UNSIGNED_SHORT },
        { GL_RGB32I, GL_RGB_INTEGER, GL_INT }, { GL_RGB32UI, GL_RGB_INTEGER, GL_UNSIGNED_INT },
        { GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE }, { GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE },
        { GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT }, { GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT },
        { GL_RGBA32I, GL_RGBA_INTEGER, GL_INT }, { GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT },
        { GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV },
    };
    ULONG k;
    for (k = 0; k < sizeof(tab) / sizeof(tab[0]); k++)
        if (tab[k][0] == ifmt) {
            *format = tab[k][1];
            *type = tab[k][2];
            return TRUE;
        }
    return FALSE;
}

/* The sized formats whose storage format and type do not say: sRGB, and
 * the packed floats given as floats. */
static GLenum keep_sized(GLenum ifmt)
{
    return ifmt == GL_SRGB8 || ifmt == GL_SRGB8_ALPHA8 || ifmt == GL_R11F_G11F_B10F || ifmt == GL_RGB9_E5 ? ifmt
                                                                                                          : 0;
}

static int pixel_size(GLenum format, GLenum type, GLenum *err)
{
    *err = GL_NO_ERROR;
    if (int_components(format)) { /* ES 3.0's integer formats */
        int n = int_components(format);
        switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE:   return n;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return n * 2;
        case GL_INT: case GL_UNSIGNED_INT:     return n * 4;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            if (format == GL_RGBA_INTEGER)
                return 4;
            *err = GL_INVALID_OPERATION;
            return 0;
        case GL_FLOAT: case GL_HALF_FLOAT_OES: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
            *err = GL_INVALID_OPERATION;
            return 0;
        default:
            *err = GL_INVALID_ENUM;
            return 0;
        }
    }
    /* OES_depth_texture and OES_packed_depth_stencil's texture formats take
     * their types only, and their types no other format. */
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES ||
        type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_24_8_OES) {
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT) return 2;
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_INT) return 4;
        if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT) return 4; /* ES 3.0's */
        if (format == GL_DEPTH_STENCIL_OES && type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) return 8;
        if (format == GL_DEPTH_STENCIL_OES && type == GL_UNSIGNED_INT_24_8_OES) return 4;
        *err = (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES || components(format) ||
                format == GL_BGRA_EXT) &&
               (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_24_8_OES ||
                type == GL_UNSIGNED_BYTE || type == GL_FLOAT || type == GL_HALF_FLOAT_OES ||
                type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                type == GL_UNSIGNED_SHORT_5_5_5_1)
                   ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    }
    switch (type) {
    case GL_FLOAT:          /* OES_texture_float */
    case GL_HALF_FLOAT_OES: /* OES_texture_half_float */
        if (components(format))
            return components(format) * (type == GL_FLOAT ? 4 : 2);
        *err = format == GL_BGRA_EXT ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    /* ES 3.0's: signed bytes of the RGBA formats, the packed words */
    case GL_BYTE:
        if (format == GL_RED || format == GL_RG || format == GL_RGB || format == GL_RGBA)
            return components(format);
        *err = components(format) ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        if (format == GL_RGBA)
            return 4;
        *err = components(format) ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        if (format == GL_RGB)
            return 4;
        *err = components(format) ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE:
        switch (format) {
        case GL_RGBA:
        case GL_BGRA_EXT:        return 4;
        case GL_RGB:             return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE:
        case GL_ALPHA:
        case GL_RED:             return 1;
        case GL_RG:              return 2;
        default:                 *err = GL_INVALID_ENUM; return 0;
        }
    case GL_UNSIGNED_SHORT_5_6_5:
        if (format == GL_RGB) return 2;
        *err = format == GL_RGBA || format == GL_LUMINANCE || format == GL_ALPHA ||
               format == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        if (format == GL_RGBA) return 2;
        *err = format == GL_RGB || format == GL_LUMINANCE || format == GL_ALPHA ||
               format == GL_LUMINANCE_ALPHA ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
        return 0;
    default:
        *err = GL_INVALID_ENUM;
        return 0;
    }
}

static UBYTE x5(UWORD v) { return (UBYTE)((v << 3) | (v >> 2)); }
static UBYTE x6(UWORD v) { return (UBYTE)((v << 2) | (v >> 4)); }
static UBYTE x4(UWORD v) { return (UBYTE)((v << 4) | v); }

/* A float's bits and back. */
static ULONG f2u(GLfloat f)
{
    union { GLfloat f; ULONG u; } x;
    x.f = f;
    return x.u;
}

static GLfloat u2f(ULONG u)
{
    union { GLfloat f; ULONG u; } x;
    x.u = u;
    return x.f;
}

/* IEEE half precision (OES_texture_half_float) to single and back, by
 * their bits (nearest, ties away from zero). */
static GLfloat half_to_float(UWORD h)
{
    ULONG s = (ULONG)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023;
    if (e == 31)
        return u2f(s | 0x7f800000UL | (m << 13)); /* infinity, NaN */
    if (!e) { /* zero, or a subnormal: m * 2^-24 */
        GLfloat v = (GLfloat)m * (1.0f / 16777216.0f);
        return s ? -v : v;
    }
    return u2f(s | ((e + 112) << 23) | (m << 13));
}

UWORD ogles2_float_to_half(GLfloat f)
{
    ULONG u = f2u(f), s = (u >> 16) & 0x8000, m = u & 0x7fffff;
    LONG e = (LONG)((u >> 23) & 255) - 127 + 15;
    if (((u >> 23) & 255) == 255)
        return (UWORD)(s | 0x7c00 | (m ? 0x200 : 0));
    if (e >= 31)
        return (UWORD)(s | 0x7c00); /* too large: infinity */
    if (e <= 0) {                   /* a subnormal half, or zero */
        ULONG shift, h;
        if (e < -10)
            return (UWORD)s;
        m |= 0x800000;
        shift = (ULONG)(14 - e);
        h = (m >> shift) + ((m >> (shift - 1)) & 1);
        return (UWORD)(s | h);
    }
    return (UWORD)((s | ((ULONG)e << 10) | (m >> 13)) + ((m >> 12) & 1)); /* a carry rounds the exponent up */
}

/* Floats or half floats of the application's format into RGBA floats (the
 * conventions of the RGBA8 copy: LUMINANCE (L,L,L,1), ALPHA (0,0,0,A)). */
static GLfloat pow2f(int e);

/* ES 3.0's packed words (the 68k's order, as GL's types are) as floats. */
static void unpack_word(GLenum type, ULONG w, GLfloat *v)
{
    if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
        v[0] = (GLfloat)(w & 1023) / 1023.0f;
        v[1] = (GLfloat)((w >> 10) & 1023) / 1023.0f;
        v[2] = (GLfloat)((w >> 20) & 1023) / 1023.0f;
        v[3] = (GLfloat)(w >> 30) / 3.0f;
    } else if (type == GL_UNSIGNED_INT_10F_11F_11F_REV) {
        /* 11- and 10-bit unsigned floats are a half float's top bits */
        v[0] = half_to_float((UWORD)((w & 0x7ff) << 4));
        v[1] = half_to_float((UWORD)(((w >> 11) & 0x7ff) << 4));
        v[2] = half_to_float((UWORD)(((w >> 22) & 0x3ff) << 5));
        v[3] = 1.0f;
    } else { /* 5_9_9_9_REV: three 9-bit mantissas and a shared exponent (ES 3.0 3.7.3) */
        GLfloat scale = pow2f((int)(w >> 27) - 15 - 9);
        v[0] = (GLfloat)(w & 511) * scale;
        v[1] = (GLfloat)((w >> 9) & 511) * scale;
        v[2] = (GLfloat)((w >> 18) & 511) * scale;
        v[3] = 1.0f;
    }
}

static void convert_float(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                          int w, int h, ULONG src_stride)
{
    int n = components(format), bpc = type == GL_FLOAT ? 4 : type == GL_BYTE ? 1 : 2, x, y, c;
    BOOL packed = type == GL_UNSIGNED_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_10F_11F_11F_REV ||
                  type == GL_UNSIGNED_INT_5_9_9_9_REV;
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        GLfloat *d = (GLfloat *)dst;
        for (x = 0; x < w; x++, d += 4) {
            GLfloat v[4];
            if (packed) {
                ULONG wd;
                memcpy(&wd, s, 4);
                s += 4;
                unpack_word(type, wd, v);
                d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3];
                continue;
            }
            for (c = 0; c < n; c++, s += bpc) {
                if (bpc == 1) { /* signed normalized (ES 3.0 2.1.6) */
                    GLfloat f = (GLfloat)(BYTE)s[0] / 127.0f;
                    v[c] = f < -1.0f ? -1.0f : f;
                } else if (bpc == 4) {
                    memcpy(&v[c], s, 4); /* the 68k's own byte order, as GL's types are */
                } else {
                    UWORD hv;
                    memcpy(&hv, s, 2);
                    v[c] = half_to_float(hv);
                }
            }
            switch (format) {
            case GL_RGBA:            d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3]; break;
            case GL_RGB:             d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = 1.0f; break;
            case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = v[0]; d[3] = v[1]; break;
            case GL_LUMINANCE:       d[0] = d[1] = d[2] = v[0]; d[3] = 1.0f; break;
            case GL_RED:             d[0] = v[0]; d[1] = d[2] = 0.0f; d[3] = 1.0f; break;
            case GL_RG:              d[0] = v[0]; d[1] = v[1]; d[2] = 0.0f; d[3] = 1.0f; break;
            default: /* GL_ALPHA */  d[0] = d[1] = d[2] = 0.0f; d[3] = v[0]; break;
            }
        }
    }
}

/* Depth values into the stored depth word (CLASS_DEPTH16/24). */
static void convert_depth(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum type, int w, int h,
                          ULONG src_stride)
{
    int bpp = type == GL_UNSIGNED_SHORT ? 2 : type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? 8 : 4, x, y;
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        ULONG *d = (ULONG *)dst;
        for (x = 0; x < w; x++, s += bpp) {
            if (bpp == 2) {
                UWORD v;
                memcpy(&v, s, 2);
                d[x] = v;
            } else {
                ULONG v;
                memcpy(&v, s, 4);
                /* 24 bits kept; UNSIGNED_INT_24_8's low byte is the stencil;
                 * ES 3.0's floats kept as their bits, clamped to [0, 1] as
                 * every depth component is when given (ES 3.0 3.7.2; the
                 * stencil word after FLOAT_32_UNSIGNED_INT_24_8_REV's depth
                 * dropped) */
                if (type == GL_FLOAT || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
                    GLfloat f;
                    memcpy(&f, &v, 4);
                    f = f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f; /* NaN to 0 too */
                    memcpy(&v, &f, 4);
                }
                d[x] = type == GL_UNSIGNED_INT ? v & 0xffffff00UL : v;
            }
        }
    }
}

/* The 16-bit packed types into an RGBA8 texel (R in the top byte: the copy's
 * byte order on the 68k) as two table lookups, one per byte, ORed: each
 * channel's expansion (x5, x6, x4) splits into bits from the high byte and
 * bits from the low byte that never overlap. [0] by the high byte, [1] by
 * the low. Made once, 6 KB. */
static ULONG t565[2][256], t4444[2][256], t5551[2][256];
static BOOL packed_tables;

static void make_packed_tables(void)
{
    int b;
    for (b = 0; b < 256; b++) {
        ULONG h = (ULONG)b, l = (ULONG)b;
        /* 5-6-5: R = h>>3; G = (h&7)<<3 | l>>5; B = l&31 */
        t565[0][b] = ((ULONG)x5((UWORD)(h >> 3)) << 24) | ((((h & 7) << 5) | ((h & 7) >> 1)) << 16) | 0xFF;
        t565[1][b] = (((l >> 5) << 2) << 16) | ((ULONG)x5((UWORD)(l & 31)) << 8);
        /* 4-4-4-4: R = h>>4; G = h&15; B = l>>4; A = l&15 */
        t4444[0][b] = ((ULONG)x4((UWORD)(h >> 4)) << 24) | ((ULONG)x4((UWORD)(h & 15)) << 16);
        t4444[1][b] = ((ULONG)x4((UWORD)(l >> 4)) << 8) | x4((UWORD)(l & 15));
        /* 5-5-5-1: R = h>>3; G = (h&7)<<2 | l>>6; B = (l>>1)&31; A = l&1 */
        t5551[0][b] = ((ULONG)x5((UWORD)(h >> 3)) << 24) | ((((h & 7) << 5) | (h & 7)) << 16);
        t5551[1][b] = (((l >> 6) << 3) << 16) | ((ULONG)x5((UWORD)((l >> 1) & 31)) << 8) | ((l & 1) ? 0xFF : 0);
    }
    packed_tables = TRUE;
}

/* One row of RGBA8-class texels from the application's (format, type), the
 * common cases each in a loop of its own (a switch per texel cost 50 ms a
 * megapixel on the A1200). FALSE for one convert's general loop does. */
static BOOL convert_row(ULONG *d, const UBYTE *s, GLenum format, GLenum type, int w)
{
    const ULONG (*tab)[256];
    int x;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:   tab = (const ULONG (*)[256])t565; break;
    case GL_UNSIGNED_SHORT_4_4_4_4: tab = (const ULONG (*)[256])t4444; break;
    case GL_UNSIGNED_SHORT_5_5_5_1: tab = (const ULONG (*)[256])t5551; break;
    case GL_UNSIGNED_BYTE:          tab = 0; break;
    default:                        return FALSE;
    }
    if (tab) {
        if (!packed_tables)
            make_packed_tables();
        for (x = 0; x < w; x++, s += 2) /* big-endian: the 68k's own order */
            d[x] = tab[0][s[0]] | tab[1][s[1]];
        return TRUE;
    }
    switch (format) {
    case GL_RGBA:
        memcpy(d, s, (ULONG)w * 4);
        return TRUE;
    case GL_BGRA_EXT:
        for (x = 0; x < w; x++, s += 4)
            d[x] = ((ULONG)s[2] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[0] << 8) | s[3];
        return TRUE;
    case GL_RGB:
        for (x = 0; x < w; x++, s += 3)
            d[x] = ((ULONG)s[0] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[2] << 8) | 0xFF;
        return TRUE;
    case GL_LUMINANCE_ALPHA:
        for (x = 0; x < w; x++, s += 2)
            d[x] = ((ULONG)s[0] * 0x01010100UL) | s[1];
        return TRUE;
    case GL_LUMINANCE:
        for (x = 0; x < w; x++)
            d[x] = ((ULONG)s[x] * 0x01010100UL) | 0xFF;
        return TRUE;
    case GL_ALPHA:
        for (x = 0; x < w; x++)
            d[x] = s[x];
        return TRUE;
    case GL_RED:
        for (x = 0; x < w; x++)
            d[x] = ((ULONG)s[x] << 24) | 0xFF;
        return TRUE;
    case GL_RG:
        for (x = 0; x < w; x++, s += 2)
            d[x] = ((ULONG)s[0] << 24) | ((ULONG)s[1] << 16) | 0xFF;
        return TRUE;
    }
    return FALSE;
}

/* ES 3.0's integer formats into four 32-bit words a texel, signed types
 * sign-extended; missing components 0, alpha 1. */
static void convert_int(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type, int w, int h,
                        ULONG src_stride)
{
    int n = int_components(format), x, y, c;
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        ULONG *d = (ULONG *)dst;
        for (x = 0; x < w; x++, d += 4) {
            ULONG v[4];
            v[0] = v[1] = v[2] = 0;
            v[3] = 1;
            if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
                ULONG wd;
                memcpy(&wd, s, 4);
                s += 4;
                v[0] = wd & 1023; v[1] = (wd >> 10) & 1023; v[2] = (wd >> 20) & 1023; v[3] = wd >> 30;
            } else
                for (c = 0; c < n; c++)
                    switch (type) {
                    case GL_BYTE:           v[c] = (ULONG)(LONG)(BYTE)*s; s += 1; break;
                    case GL_UNSIGNED_BYTE:  v[c] = *s; s += 1; break;
                    case GL_SHORT:          { WORD w16; memcpy(&w16, s, 2); v[c] = (ULONG)(LONG)w16; s += 2; break; }
                    case GL_UNSIGNED_SHORT: { UWORD u16; memcpy(&u16, s, 2); v[c] = u16; s += 2; break; }
                    default:                memcpy(&v[c], s, 4); s += 4; break;
                    }
            d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3];
        }
    }
}

/* w x h pixels of the application's (format, type) at src, src_stride
 * bytes a row (unpack_image), into the RAM copy at dst (dst_stride bytes
 * per row) in the class of (format, type). */
static void convert(UBYTE *dst, ULONG dst_stride, const UBYTE *src, GLenum format, GLenum type,
                    int w, int h, ULONG src_stride)
{
    int x, y;
    if (int_components(format)) {
        convert_int(dst, dst_stride, src, format, type, w, h, src_stride);
        return;
    }
    switch (class_of(format, type)) {
    case CLASS_FLOAT:
        convert_float(dst, dst_stride, src, format, type, w, h, src_stride);
        return;
    case CLASS_DEPTH16:
    case CLASS_DEPTH24:
        convert_depth(dst, dst_stride, src, type, w, h, src_stride);
        return;
    }
    if (!((ULONG)dst & 3) && !(dst_stride & 3) && convert_row((ULONG *)dst, src, format, type, 0)) {
        for (y = 0; y < h; y++, src += src_stride, dst += dst_stride)
            convert_row((ULONG *)dst, src, format, type, w);
        return;
    }
    for (y = 0; y < h; y++, src += src_stride, dst += dst_stride) {
        const UBYTE *s = src;
        UBYTE *d = dst;
        for (x = 0; x < w; x++, d += 4) {
            UWORD v;
            switch (type) {
            case GL_UNSIGNED_SHORT_5_6_5:
                v = (UWORD)((s[0] << 8) | s[1]); /* big-endian: the 68k's own order */
                s += 2;
                d[0] = x5((UWORD)(v >> 11)); d[1] = x6((UWORD)((v >> 5) & 63)); d[2] = x5((UWORD)(v & 31)); d[3] = 255;
                continue;
            case GL_UNSIGNED_SHORT_4_4_4_4:
                v = (UWORD)((s[0] << 8) | s[1]);
                s += 2;
                d[0] = x4((UWORD)(v >> 12)); d[1] = x4((UWORD)((v >> 8) & 15));
                d[2] = x4((UWORD)((v >> 4) & 15)); d[3] = x4((UWORD)(v & 15));
                continue;
            case GL_UNSIGNED_SHORT_5_5_5_1:
                v = (UWORD)((s[0] << 8) | s[1]);
                s += 2;
                d[0] = x5((UWORD)(v >> 11)); d[1] = x5((UWORD)((v >> 6) & 31));
                d[2] = x5((UWORD)((v >> 1) & 31)); d[3] = (v & 1) ? 255 : 0;
                continue;
            }
            switch (format) {
            case GL_RGBA:            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; s += 4; break;
            case GL_BGRA_EXT:        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; s += 4; break;
            case GL_RGB:             d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; s += 3; break;
            case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; s += 2; break;
            case GL_LUMINANCE:       d[0] = d[1] = d[2] = s[0]; d[3] = 255; s += 1; break;
            case GL_RED:             d[0] = s[0]; d[1] = d[2] = 0; d[3] = 255; s += 1; break;
            case GL_RG:              d[0] = s[0]; d[1] = s[1]; d[2] = 0; d[3] = 255; s += 2; break;
            default: /* GL_ALPHA */  d[0] = d[1] = d[2] = 0; d[3] = s[0]; s += 1; break;
            }
        }
    }
}

/* The application's image as glPixelStorei's unpack state reads it (ES
 * 3.0 3.7.1): its first texel, and its row and image strides. */
static const UBYTE *unpack_image(struct OGLES2Context *ctx, const void *pixels, GLenum format, GLenum type, int w,
                                 int h, ULONG *row, ULONG *image)
{
    GLenum err;
    const struct OGLES2PixelStore *u = &ctx->unpack;
    ULONG bpp = (ULONG)pixel_size(format, type, &err), a = (ULONG)u->alignment;
    *row = ((ULONG)(u->row_length ? u->row_length : w) * bpp + a - 1) / a * a;
    *image = *row * (ULONG)(u->image_height ? u->image_height : h);
    return (const UBYTE *)pixels + (ULONG)u->skip_images * *image + (ULONG)u->skip_rows * *row +
           (ULONG)u->skip_pixels * bpp;
}

/* Tight images (the library's own: copies, decoded ETC1) through
 * glTexImage2D: the application's unpack state (and pixel buffer) set
 * aside, saved in *s. */
static void unpack_tight(struct OGLES2Context *ctx, struct OGLES2PixelStore *s)
{
    *s = ctx->unpack;
    memset(&ctx->unpack, 0, sizeof(ctx->unpack));
    ctx->unpack.alignment = 1;
    ctx->unpack.own = TRUE;
}

/* ES 3.0's pixel unpack buffer: with one bound, *pixels is an offset into
 * it, the image `bytes` long from there. FALSE: GL_INVALID_OPERATION set
 * (past its end, or mapped). */
static BOOL unpack_source(struct OGLES2Context *ctx, const void **pixels, ULONG bytes)
{
    BOOL bad;
    const UBYTE *p = ogles2_buffer_pixels(ctx, FALSE, (ULONG)*pixels, bytes, &bad);
    if (bad) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    if (p)
        *pixels = p;
    return TRUE;
}

/* The bytes an application's w x h x d image takes from its start, as the
 * unpack state reads it (unpack_image). */
static ULONG unpack_bytes(struct OGLES2Context *ctx, GLenum format, GLenum type, int w, int h, int d)
{
    ULONG row, image, start;
    GLenum err;
    if (w <= 0 || h <= 0 || d <= 0)
        return 0;
    start = (ULONG)(unpack_image(ctx, (const void *)0x1000, format, type, w, h, &row, &image) -
                    (const UBYTE *)0x1000);
    return start + (ULONG)(d - 1) * image + (ULONG)(h - 1) * row + (ULONG)w * (ULONG)pixel_size(format, type, &err);
}

/* ES 3.0's GL_HALF_FLOAT (0x140B) as OES_texture_half_float's
 * GL_HALF_FLOAT_OES (0x8D61), which the library keeps the levels in. */
static GLenum half_type(struct OGLES2Context *ctx, GLenum type)
{
    return type == GL_HALF_FLOAT && ogles2_es3(ctx) ? GL_HALF_FLOAT_OES : type;
}

/* 2^e as a float (no libm: built from its bits; e from -126 to 127). */
static GLfloat pow2f(int e)
{
    union { ULONG u; GLfloat f; } v;
    if (e < -126)
        return 0.0f;
    v.u = (ULONG)(e + 127) << 23;
    return v.f;
}

/* floor(log2(x)) of a positive normal float, from its exponent bits. */
static int ilog2f(GLfloat x)
{
    union { ULONG u; GLfloat f; } v;
    v.f = x;
    return (int)((v.u >> 23) & 0xff) - 127;
}

/* RGB9_E5 of three floats (ES 3.0 3.7.3's equations 3.4 to 3.7). */
static ULONG rgb9e5(const GLfloat *s)
{
    const GLfloat maxv = 65408.0f; /* (2^9 - 1) / 2^9 x 2^(31 - 15) */
    GLfloat c[3], m = 0.0f, scale;
    ULONG w = 0;
    int k, e, ms;
    for (k = 0; k < 3; k++) {
        c[k] = s[k] > 0.0f ? (s[k] < maxv ? s[k] : maxv) : 0.0f;
        if (c[k] > m)
            m = c[k];
    }
    e = (m > 0.0f ? (ilog2f(m) > -16 ? ilog2f(m) : -16) : -16) + 1 + 15;
    ms = (int)(m / pow2f(e - 15 - 9) + 0.5f);
    if (ms == 512)
        e++;
    scale = pow2f(e - 15 - 9);
    for (k = 0; k < 3; k++)
        w |= (ULONG)(int)(c[k] / scale + 0.5f) << (9 * k);
    return w | ((ULONG)e << 27);
}

/* An unsigned 11- (or `bits` 10-) bit float: a half float's top bits, rounded. */
static ULONG small_float(GLfloat f, int bits)
{
    ULONG h;
    if (!(f > 0.0f))
        return 0;
    h = ogles2_float_to_half(f) & 0x7fff;
    if (h >= 0x7c00)
        return bits == 11 ? 0x7c0 : 0x3e0; /* infinity */
    h = (h + (bits == 11 ? 8 : 16)) >> (bits == 11 ? 4 : 5);
    return h;
}

static int level_count(int w, int h)
{
    int n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

/* Level `l` of a face has the size the mip chain of its level 0 gives it,
 * and its RAM copy is of level 0's class (the stored format is level 0's). */
static BOOL consistent(const struct OGLES2Face *f, int l)
{
    int w = f->w[0] >> l, h = f->h[0] >> l;
    /* The same format and type as level 0 (GLES 2.0 3.7.10), not just the
     * same storage: RGB and RGBA levels are both kept as RGBA8. */
    if (!(f->defined & (1UL << l)) || !(f->defined & 1) || level_class(f, l) != level_class(f, 0) ||
        f->format[l] != f->format[0] || f->type[l] != f->type[0] || f->sized[l] != f->sized[0])
        return FALSE;
    return f->w[l] == (w ? w : 1) && f->h[l] == (h ? h : 1);
}

/* A 2D array or a 3D texture (ES 3.0): its faces are layers, or slices. */
static BOOL layered(const struct OGLES2Texture *t)
{
    return t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_3D;
}

/* The faces of level 0: a cube map's six, an array's layers, a 3D
 * texture's slices. */
static int faces(const struct OGLES2Texture *t)
{
    return t->target == GL_TEXTURE_CUBE_MAP ? 6 : layered(t) ? t->d[0] : 1;
}

/* The faces level l has: a 3D texture's slices halve with its levels. */
static int faces_at(const struct OGLES2Texture *t, int l)
{
    return t->target == GL_TEXTURE_3D ? t->d[l] : faces(t);
}

/* Level l has the depth (an array: the layers) the chain from level 0 gives it. */
static BOOL depth_ok(const struct OGLES2Texture *t, int l)
{
    int d;
    if (!layered(t))
        return TRUE;
    d = t->target == GL_TEXTURE_3D ? t->d[0] >> l : t->d[0];
    return t->d[l] == (d ? d : 1);
}

/* The levels of a full chain from level 0 (a 3D texture's depth halves too). */
static int chain_levels(const struct OGLES2Texture *t)
{
    int n = level_count(t->f[0].w[0], t->f[0].h[0]), d = t->d[0], m = 1;
    if (t->target != GL_TEXTURE_3D)
        return n;
    while (d > 1) {
        d /= 2;
        m++;
    }
    return m > n ? m : n;
}

/* Level 0 exists; a cube map's on all six faces, square and of one size
 * (GLES 2.0 3.7.10, "cube complete"). */
static BOOL base_ok(const struct OGLES2Texture *t)
{
    const struct OGLES2Face *f0 = &t->f[0];
    int k;
    if (!(f0->defined & 1) || !f0->w[0] || !f0->h[0])
        return FALSE;
    for (k = 1; k < faces(t); k++)
        if (!(t->f[k].defined & 1) || t->f[k].w[0] != f0->w[0] || t->f[k].h[0] != f0->h[0] ||
            level_class(&t->f[k], 0) != level_class(f0, 0) || t->f[k].format[0] != f0->format[0] ||
            t->f[k].type[0] != f0->type[0])
            return FALSE;
    return t->target != GL_TEXTURE_CUBE_MAP || f0->w[0] == f0->h[0];
}

static BOOL mipmapped(GLenum min_filter)
{
    return min_filter != GL_NEAREST && min_filter != GL_LINEAR;
}

/* GLES 2.0 3.7.10 with SGIS_texture_lod's levels: level 0 (a cube map's
 * cube complete; the GPU copy is laid out from it, so it must exist even
 * when a later base level is sampled), and the levels sampled, base_level
 * up to max_level or the 1x1 level (a mipmapping filter) or base_level
 * alone, consistent on every face and in the GPU copy: *first and *last. */
static BOOL complete(const struct OGLES2Texture *t, GLenum min_filter, GLenum mag_filter, int *first, int *last)
{
    int n, l, k, b = t->base_level, e;
    if (!t->gpu || !base_ok(t))
        return FALSE;
    /* ES 3.0 3.8.13: an integer texture filtered linearly is incomplete */
    if (int_components(t->f[0].format[0]) &&
        (mag_filter != GL_NEAREST || (min_filter != GL_NEAREST && min_filter != GL_NEAREST_MIPMAP_NEAREST)))
        return FALSE;
    n = chain_levels(t);
    /* ES 3.0 3.8.10: an immutable texture's levels, base and max clamped to them */
    if (t->immutable_levels) {
        n = t->immutable_levels;
        if (b > n - 1)
            b = n - 1;
    }
    if (b >= n)
        return FALSE;
    e = !mipmapped(min_filter) ? b : t->max_level < n - 1 ? t->max_level : n - 1;
    if (t->immutable_levels && e < b)
        e = b; /* ES 3.0: max level clamped to [base, levels - 1] */
    if (e < b || e >= t->gpu->num_levels)
        return FALSE;
    for (l = b; l <= e; l++) {
        if (!depth_ok(t, l))
            return FALSE;
        for (k = 0; k < faces_at(t, l); k++)
            if (l && !consistent(&t->f[k], l))
                return FALSE;
    }
    *first = b;
    *last = e;
    return TRUE;
}

/* A face's view of the GPU texture: face 0 is the texture itself; face k
 * the same layout `k` face strides further on (the upload functions only
 * write through texture_mem). */
static V3DTexture *face_gpu(struct OGLES2Texture *t, int k, int l, V3DTexture *tmp)
{
    if (!k)
        return t->gpu;
    *tmp = *t->gpu;
    if (t->target == GL_TEXTURE_3D) { /* slice k: k of the level's slice sizes on (volume_memory) */
        tmp->levels[l].offset += (ULONG)k * tmp->levels[l].padded_h * tmp->levels[l].stride;
        return tmp;
    }
    tmp->texture_mem.hostptr =
        (void *)(V3D_ALIGN_UP((v3d_uintptr)t->gpu->texture_mem.hostptr, 256) + (ULONG)k * t->face_stride);
    tmp->texture_mem.size = t->face_stride;
    return tmp;
}

/* The hardware format for level 0's format and type (Mesa's
 * v3dx_format_table.c, the formats GLES2's state tracker picks). */
static void storage_of(struct OGLES2Texture *t)
{
    GLenum f = t->f[0].format[0], ty = t->f[0].type[0], sz = t->f[0].sized[0];
    UBYTE R = v3d_SWIZZLE_RED, G = v3d_SWIZZLE_GREEN, B = v3d_SWIZZLE_BLUE, A = v3d_SWIZZLE_ALPHA;
    UBYTE O = v3d_SWIZZLE_ONE, Z = v3d_SWIZZLE_ZERO;
    UBYTE type = V3D_TEXTURE_DATA_FORMAT_RGBA8, cpp = 4, sw[4];
    sw[0] = R; sw[1] = G; sw[2] = B; sw[3] = A;
    if (f == GL_RGB)
        sw[3] = O; /* rendered into, the stored alpha is the shader's */
    if (int_components(f)) { /* ES 3.0's integers: the TMU's integer types, RGB as RGBA (alpha 1) */
        int n = int_components(f), bits = ty == GL_BYTE || ty == GL_UNSIGNED_BYTE ? 8
                                         : ty == GL_SHORT || ty == GL_UNSIGNED_SHORT ? 16 : 32;
        BOOL s = ty == GL_BYTE || ty == GL_SHORT || ty == GL_INT;
        int k = n == 1 ? 0 : n == 2 ? 1 : 2; /* R, RG, RGBA */
        static const UBYTE types[3][3][2] = { /* [k][bits][unsigned, signed] */
            { { V3D_TEXTURE_DATA_FORMAT_R8UI, V3D_TEXTURE_DATA_FORMAT_R8I },
              { V3D_TEXTURE_DATA_FORMAT_R16UI, V3D_TEXTURE_DATA_FORMAT_R16I },
              { V3D_TEXTURE_DATA_FORMAT_R32UI, V3D_TEXTURE_DATA_FORMAT_R32I } },
            { { V3D_TEXTURE_DATA_FORMAT_RG8UI, V3D_TEXTURE_DATA_FORMAT_RG8I },
              { V3D_TEXTURE_DATA_FORMAT_RG16UI, V3D_TEXTURE_DATA_FORMAT_RG16I },
              { V3D_TEXTURE_DATA_FORMAT_RG32UI, V3D_TEXTURE_DATA_FORMAT_RG32I } },
            { { V3D_TEXTURE_DATA_FORMAT_RGBA8UI, V3D_TEXTURE_DATA_FORMAT_RGBA8I },
              { V3D_TEXTURE_DATA_FORMAT_RGBA16UI, V3D_TEXTURE_DATA_FORMAT_RGBA16I },
              { V3D_TEXTURE_DATA_FORMAT_RGBA32UI, V3D_TEXTURE_DATA_FORMAT_RGBA32I } } };
        if (ty == GL_UNSIGNED_INT_2_10_10_10_REV) {
            type = V3D_TEXTURE_DATA_FORMAT_RGB10A2UI;
            cpp = 4;
        } else {
            type = types[k][bits == 8 ? 0 : bits == 16 ? 1 : 2][s ? 1 : 0];
            cpp = (UBYTE)((k == 2 ? 4 : k + 1) * (bits / 8));
        }
        if (n < 4)
            sw[3] = O;
        if (n < 3)
            sw[2] = Z;
        if (n < 2)
            sw[1] = Z;
    } else if (sz == GL_R11F_G11F_B10F || ty == GL_UNSIGNED_INT_10F_11F_11F_REV) { /* ES 3.0's packed floats */
        type = V3D_TEXTURE_DATA_FORMAT_R11FG11FB10F; cpp = 4; sw[3] = O;
    } else if (sz == GL_RGB9_E5 || ty == GL_UNSIGNED_INT_5_9_9_9_REV) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB9E5; cpp = 4; sw[3] = O;
    } else if (ty == GL_UNSIGNED_INT_2_10_10_10_REV) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB10A2; cpp = 4;
    } else if (ty == GL_BYTE) { /* signed normalized */
        type = f == GL_RED ? V3D_TEXTURE_DATA_FORMAT_R8_SNORM
             : f == GL_RG ? V3D_TEXTURE_DATA_FORMAT_RG8_SNORM : V3D_TEXTURE_DATA_FORMAT_RGBA8_SNORM;
        cpp = f == GL_RED ? 1 : f == GL_RG ? 2 : 4;
        if (f != GL_RGBA)
            sw[3] = O;
        if (f == GL_RED || f == GL_RG)
            sw[2] = Z;
        if (f == GL_RED)
            sw[1] = Z;
    } else if ((f == GL_DEPTH_COMPONENT && ty == GL_FLOAT) || ty == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
        type = V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP32F; cpp = 4; /* the depth only, as sampled */
        sw[1] = sw[2] = t->es3_rules ? Z : R;
        sw[3] = O;
    } else if (dxt_block_bytes(f)) {
        /* S3TC: BC1-BC3, a block a "texel" (Mesa's table: DXT1 RGB is XYZ1);
         * ETC1 and ETC2/EAC the same way, ETC1 as RGB8_ETC2 (a subset), the
         * sRGB ones by the texture state's bit */
        switch (f) {
        case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: type = V3D_TEXTURE_DATA_FORMAT_BC2; break;
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: type = V3D_TEXTURE_DATA_FORMAT_BC3; break;
        case GL_ETC1_RGB8_OES:
        case GL_COMPRESSED_RGB8_ETC2:
        case GL_COMPRESSED_SRGB8_ETC2:         type = V3D_TEXTURE_DATA_FORMAT_RGB8ETC2; sw[3] = O; break;
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
        case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
                                               type = V3D_TEXTURE_DATA_FORMAT_RGB8PUNCHTHROUGHALPHA1; break;
        case GL_COMPRESSED_RGBA8_ETC2_EAC:
        case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC: type = V3D_TEXTURE_DATA_FORMAT_RGBA8ETC2EAC; break;
        case GL_COMPRESSED_R11_EAC:            type = V3D_TEXTURE_DATA_FORMAT_R11EAC; break;
        case GL_COMPRESSED_SIGNED_R11_EAC:     type = V3D_TEXTURE_DATA_FORMAT_SIGNEDR11EAC; break;
        case GL_COMPRESSED_RG11_EAC:           type = V3D_TEXTURE_DATA_FORMAT_RG11EAC; break;
        case GL_COMPRESSED_SIGNED_RG11_EAC:    type = V3D_TEXTURE_DATA_FORMAT_SIGNEDRG11EAC; break;
        default:                               type = V3D_TEXTURE_DATA_FORMAT_BC1; break;
        }
        if (f == GL_COMPRESSED_R11_EAC || f == GL_COMPRESSED_SIGNED_R11_EAC) {
            sw[1] = sw[2] = Z; sw[3] = O;
        } else if (f == GL_COMPRESSED_RG11_EAC || f == GL_COMPRESSED_SIGNED_RG11_EAC) {
            sw[2] = Z; sw[3] = O;
        }
        cpp = (UBYTE)dxt_block_bytes(f);
        if (f == GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
            sw[3] = O;
    } else if (ty == GL_FLOAT || ty == GL_HALF_FLOAT_OES) {
        /* OES_texture_float (32-bit returns: the .v3dw variants) and
         * OES_texture_half_float (16-bit, filterable); RGB as RGBA. */
        BOOL h = ty == GL_HALF_FLOAT_OES;
        if (f == GL_LUMINANCE || f == GL_ALPHA || f == GL_RED) {
            type = h ? V3D_TEXTURE_DATA_FORMAT_R16F : V3D_TEXTURE_DATA_FORMAT_R32F;
            cpp = h ? 2 : 4;
            if (f == GL_LUMINANCE) {
                sw[0] = sw[1] = sw[2] = R; sw[3] = O;
            } else if (f == GL_RED) {
                sw[1] = sw[2] = Z; sw[3] = O;
            } else {
                sw[0] = sw[1] = sw[2] = Z; sw[3] = R;
            }
        } else if (f == GL_LUMINANCE_ALPHA || f == GL_RG) {
            type = h ? V3D_TEXTURE_DATA_FORMAT_RG16F : V3D_TEXTURE_DATA_FORMAT_RG32F;
            cpp = h ? 4 : 8;
            if (f == GL_RG) {
                sw[2] = Z; sw[3] = O;
            } else {
                sw[0] = sw[1] = sw[2] = R; sw[3] = G;
            }
        } else {
            type = h ? V3D_TEXTURE_DATA_FORMAT_RGBA16F : V3D_TEXTURE_DATA_FORMAT_RGBA32F;
            cpp = h ? 8 : 16;
        }
    } else if (f == GL_DEPTH_COMPONENT || f == GL_DEPTH_STENCIL_OES) {
        /* OES_depth_texture: sampled as luminance, (d, d, d, 1); ES 3.0 as
         * (d, 0, 0, 1) (its table 3.24); 32-bit returns */
        type = ty == GL_UNSIGNED_SHORT ? V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16 : V3D_TEXTURE_DATA_FORMAT_DEPTH24X8;
        cpp = ty == GL_UNSIGNED_SHORT ? 2 : 4;
        sw[0] = R;
        sw[1] = sw[2] = t->es3_rules ? Z : R;
        sw[3] = O;
    } else if (f == GL_RGB && ty == GL_UNSIGNED_SHORT_5_6_5) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB565; cpp = 2; sw[3] = O;
    } else if (f == GL_RGBA && ty == GL_UNSIGNED_SHORT_4_4_4_4) {
        type = V3D_TEXTURE_DATA_FORMAT_RGBA4; cpp = 2;
    } else if (f == GL_RGBA && ty == GL_UNSIGNED_SHORT_5_5_5_1) {
        type = V3D_TEXTURE_DATA_FORMAT_RGB5A1; cpp = 2;
    } else if (f == GL_LUMINANCE) {
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[0] = sw[1] = sw[2] = R; sw[3] = O;
    } else if (f == GL_ALPHA) {
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[0] = sw[1] = sw[2] = Z; sw[3] = R;
    } else if (f == GL_LUMINANCE_ALPHA) {
        type = V3D_TEXTURE_DATA_FORMAT_RG8; cpp = 2; sw[0] = sw[1] = sw[2] = R; sw[3] = G;
    } else if (f == GL_RED) { /* ES 3.0's: (r, 0, 0, 1) */
        type = V3D_TEXTURE_DATA_FORMAT_R8; cpp = 1; sw[1] = sw[2] = Z; sw[3] = O;
    } else if (f == GL_RG) { /* (r, g, 0, 1) */
        type = V3D_TEXTURE_DATA_FORMAT_RG8; cpp = 2; sw[2] = Z; sw[3] = O;
    }
    t->hw_type = type;
    t->cpp = cpp;
    memcpy(t->swizzle, sw, 4);
}

/* A 16-bit texel stored little-endian (the GPU's order) from the 68k. */
#define LE16_STORE(p, v) (*(UWORD *)(p) = (UWORD)(((v) << 8) | ((v) >> 8)))

/* n RGBA8 texels into the stored format, in the GPU's byte order (16-bit
 * texels little-endian, the GL packing: red in the top bits). Each format
 * in a loop of its own, a texel read as one long (R in the top byte): a
 * switch per texel cost 50 ms a megapixel on the A1200. The copy's rows and
 * `d` are word aligned (AllocVec, widths in whole texels). */
static void pack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    const ULONG *p = (const ULONG *)s;
    ULONG i, c;
    switch (t->hw_type) {
    case V3D_TEXTURE_DATA_FORMAT_RGB565:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF800) | ((c >> 13) & 0x07E0) | ((c >> 11) & 0x001F));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_RGBA4:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
        for (i = 0; i < n; i++, d += 2) {
            UWORD v;
            c = p[i];
            v = (UWORD)(((c >> 16) & 0xF800) | ((c >> 13) & 0x07C0) | ((c >> 10) & 0x003E) | ((c >> 7) & 1));
            LE16_STORE(d, v);
        }
        return;
    case V3D_TEXTURE_DATA_FORMAT_R8: /* luminance (red of the copy) or alpha */
        if (t->swizzle[3] == v3d_SWIZZLE_RED)
            for (i = 0; i < n; i++)
                d[i] = (UBYTE)p[i];
        else
            for (i = 0; i < n; i++)
                d[i] = (UBYTE)(p[i] >> 24);
        return;
    case V3D_TEXTURE_DATA_FORMAT_RG8: /* luminance, alpha; red, green (GL_RG: green swizzled from green) */
        for (i = 0; i < n; i++, d += 2) {
            c = p[i];
            d[0] = (UBYTE)(c >> 24);
            d[1] = (UBYTE)(t->swizzle[1] == v3d_SWIZZLE_GREEN ? c >> 16 : c);
        }
        return;
    default:
        memcpy(d, s, n * 4);
        return;
    }
}

static BOOL float_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R32F || hw == V3D_TEXTURE_DATA_FORMAT_RG32F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGBA32F || hw == V3D_TEXTURE_DATA_FORMAT_R16F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RG16F || hw == V3D_TEXTURE_DATA_FORMAT_RGBA16F;
}

static BOOL depth_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16 || hw == V3D_TEXTURE_DATA_FORMAT_DEPTH24X8 ||
           hw == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP32F;
}

/* ES 3.0's integer types (their copies 32-bit words, pack_wide narrows them). */
static BOOL int_type(UBYTE hw)
{
    return (hw >= V3D_TEXTURE_DATA_FORMAT_R8I && hw <= V3D_TEXTURE_DATA_FORMAT_RGB10A2UI);
}

/* ES 3.0's formats the CPU encodes from a float copy (pack_wide). */
static BOOL encoded_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R11FG11FB10F || hw == V3D_TEXTURE_DATA_FORMAT_RGB9E5 ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGB10A2 || hw == V3D_TEXTURE_DATA_FORMAT_R8_SNORM ||
           hw == V3D_TEXTURE_DATA_FORMAT_RG8_SNORM || hw == V3D_TEXTURE_DATA_FORMAT_RGBA8_SNORM;
}

/* Formats the TMU returns 32 bits a channel for (Mesa's v3dx_format_table.c):
 * a draw sampling one needs the program's .v3dp's 32-bit variants. */
static BOOL wide_type(UBYTE hw)
{
    return hw == V3D_TEXTURE_DATA_FORMAT_R32F || hw == V3D_TEXTURE_DATA_FORMAT_RG32F ||
           hw == V3D_TEXTURE_DATA_FORMAT_RGBA32F || depth_type(hw);
}

/* Formats the TMU cannot filter (Mesa's table): sampled nearest whatever the
 * filters say (OES_texture_float_linear is not offered; OES_depth_texture
 * leaves filtering to the implementation). */
static BOOL unfilterable_type(UBYTE hw)
{
    return wide_type(hw) || int_type(hw);
}

static void put_le32(UBYTE *d, ULONG v)
{
    d[0] = (UBYTE)v; d[1] = (UBYTE)(v >> 8); d[2] = (UBYTE)(v >> 16); d[3] = (UBYTE)(v >> 24);
}

/* n texels of a FLOAT or DEPTH RAM copy into the stored format, little-endian.
 * Luminance and alpha formats store red or alpha, as pack does. */
static void pack_wide(const struct OGLES2Texture *t, const UBYTE *src, UBYTE *d, ULONG n)
{
    ULONG i;
    BOOL alpha = t->swizzle[3] == v3d_SWIZZLE_RED; /* an ALPHA format: its one channel is alpha */
    if (depth_type(t->hw_type)) {
        const ULONG *s = (const ULONG *)src;
        for (i = 0; i < n; i++)
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16) {
                *d++ = (UBYTE)s[i];
                *d++ = (UBYTE)(s[i] >> 8);
            } else {
                put_le32(d, s[i]);
                d += 4;
            }
        return;
    }
    for (i = 0; i < n; i++, src += 16) {
        const GLfloat *s = (const GLfloat *)src;
        GLfloat v[4];
        int c, nc;
        if (int_type(t->hw_type)) { /* ES 3.0's integers: the words narrowed */
            const ULONG *w4 = (const ULONG *)src;
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB10A2UI) {
                put_le32(d, (w4[0] & 1023) | ((w4[1] & 1023) << 10) | ((w4[2] & 1023) << 20) | (w4[3] << 30));
                d += 4;
                continue;
            }
            {
                int ncomp = t->hw_type <= V3D_TEXTURE_DATA_FORMAT_R8UI || t->hw_type == V3D_TEXTURE_DATA_FORMAT_R16I ||
                                    t->hw_type == V3D_TEXTURE_DATA_FORMAT_R16UI || t->hw_type == V3D_TEXTURE_DATA_FORMAT_R32I ||
                                    t->hw_type == V3D_TEXTURE_DATA_FORMAT_R32UI ? 1
                          : t->hw_type <= V3D_TEXTURE_DATA_FORMAT_RG8UI || t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG16I ||
                                    t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG16UI || t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG32I ||
                                    t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG32UI ? 2 : 4;
                int bytes = t->cpp / ncomp;
                for (c = 0; c < ncomp; c++) {
                    if (bytes == 1)
                        *d++ = (UBYTE)w4[c];
                    else if (bytes == 2) {
                        *d++ = (UBYTE)w4[c];
                        *d++ = (UBYTE)(w4[c] >> 8);
                    } else {
                        put_le32(d, w4[c]);
                        d += 4;
                    }
                }
            }
            continue;
        }
        if (encoded_type(t->hw_type)) { /* ES 3.0's: one word or 1 to 4 signed bytes */
            ULONG wd = 0;
            switch (t->hw_type) {
            case V3D_TEXTURE_DATA_FORMAT_R11FG11FB10F:
                wd = small_float(s[0], 11) | (small_float(s[1], 11) << 11) | (small_float(s[2], 10) << 22);
                break;
            case V3D_TEXTURE_DATA_FORMAT_RGB9E5:
                wd = rgb9e5(s);
                break;
            case V3D_TEXTURE_DATA_FORMAT_RGB10A2:
                for (c = 0; c < 4; c++) {
                    GLfloat f = s[c] < 0.0f ? 0.0f : s[c] > 1.0f ? 1.0f : s[c];
                    wd |= (ULONG)(f * (c == 3 ? 3.0f : 1023.0f) + 0.5f) << (10 * c);
                }
                break;
            default: /* signed normalized bytes */
                nc = t->cpp;
                for (c = 0; c < nc; c++) {
                    GLfloat f = s[c] < -1.0f ? -1.0f : s[c] > 1.0f ? 1.0f : s[c];
                    *d++ = (UBYTE)(BYTE)(f < 0.0f ? (int)(f * 127.0f - 0.5f) : (int)(f * 127.0f + 0.5f));
                }
                continue;
            }
            put_le32(d, wd);
            d += 4;
            continue;
        }
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_R32F:
        case V3D_TEXTURE_DATA_FORMAT_R16F:
            v[0] = alpha ? s[3] : s[0];
            nc = 1;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG32F:
        case V3D_TEXTURE_DATA_FORMAT_RG16F:
            v[0] = s[0];
            v[1] = t->swizzle[1] == v3d_SWIZZLE_GREEN ? s[1] : s[3]; /* GL_RG's green, else alpha */
            nc = 2;
            break;
        default:
            v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; v[3] = s[3];
            nc = 4;
            break;
        }
        for (c = 0; c < nc; c++) {
            if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_R16F || t->hw_type == V3D_TEXTURE_DATA_FORMAT_RG16F ||
                t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F) {
                UWORD hv = ogles2_float_to_half(v[c]);
                *d++ = (UBYTE)hv;
                *d++ = (UBYTE)(hv >> 8);
            } else {
                put_le32(d, f2u(v[c]));
                d += 4;
            }
        }
    }
}

/* The inverse of pack_wide for depth (what a framebuffer object's job drew). */
static void unpack_depth(const struct OGLES2Texture *t, const UBYTE *s, ULONG *d, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++)
        if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_DEPTH_COMP16) {
            d[i] = (ULONG)s[0] | ((ULONG)s[1] << 8);
            s += 2;
        } else {
            d[i] = (ULONG)s[0] | ((ULONG)s[1] << 8) | ((ULONG)s[2] << 16) | ((ULONG)s[3] << 24);
            s += 4;
        }
}

/* The inverse of pack: n texels of the stored format into RGBA8. */
static void unpack(const struct OGLES2Texture *t, const UBYTE *s, UBYTE *d, ULONG n)
{
    ULONG i;
    UWORD v;
    for (i = 0; i < n; i++, d += 4) {
        switch (t->hw_type) {
        case V3D_TEXTURE_DATA_FORMAT_RGB565:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 11) & 31) * 255 / 31);
            d[1] = (UBYTE)(((v >> 5) & 63) * 255 / 63);
            d[2] = (UBYTE)((v & 31) * 255 / 31);
            d[3] = 255;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGBA4:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 12) & 15) * 17);
            d[1] = (UBYTE)(((v >> 8) & 15) * 17);
            d[2] = (UBYTE)(((v >> 4) & 15) * 17);
            d[3] = (UBYTE)((v & 15) * 17);
            break;
        case V3D_TEXTURE_DATA_FORMAT_RGB5A1:
            v = (UWORD)(s[0] | (s[1] << 8)); s += 2;
            d[0] = (UBYTE)(((v >> 11) & 31) * 255 / 31);
            d[1] = (UBYTE)(((v >> 6) & 31) * 255 / 31);
            d[2] = (UBYTE)(((v >> 1) & 31) * 255 / 31);
            d[3] = (v & 1) ? 255 : 0;
            break;
        case V3D_TEXTURE_DATA_FORMAT_R8:
            d[0] = d[1] = d[2] = t->swizzle[3] == v3d_SWIZZLE_RED ? 0 : s[0];
            d[3] = t->swizzle[3] == v3d_SWIZZLE_RED ? s[0] : 255;
            if (t->swizzle[1] == v3d_SWIZZLE_ZERO && t->swizzle[3] == v3d_SWIZZLE_ONE)
                d[1] = d[2] = 0; /* GL_RED */
            s++;
            break;
        case V3D_TEXTURE_DATA_FORMAT_RG8:
            if (t->swizzle[1] == v3d_SWIZZLE_GREEN) { /* GL_RG */
                d[0] = s[0]; d[1] = s[1]; d[2] = 0; d[3] = 255;
            } else {
                d[0] = d[1] = d[2] = s[0]; d[3] = s[1];
            }
            s += 2;
            break;
        default:
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            d[3] = t->swizzle[3] == v3d_SWIZZLE_ONE ? 255 : s[3];
            s += 4;
            break;
        }
    }
}

/* The inverse of pack_wide for RGBA16F (what a framebuffer object's job
 * drew into a half-float texture): n texels into RGBA floats. */
static void unpack_half(const UBYTE *s, GLfloat *d, ULONG n)
{
    ULONG i;
    for (i = 0; i < n * 4; i++, s += 2)
        d[i] = half_to_float((UWORD)(s[0] | (s[1] << 8)));
}

/* A box of face k, level l from the GPU's copy, as the RAM copy holds it:
 * RGBA8 rows, depth words (a depth texture a job drew into), or RGBA
 * floats (a half-float one). Other float textures are never drawn into. */
static BOOL load_box(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba,
                     ULONG stride)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp;
    UBYTE *packed;
    int j;
    if (!t->gpu || l >= t->gpu->num_levels || w <= 0 || h <= 0 ||
        (float_type(t->hw_type) && t->hw_type != V3D_TEXTURE_DATA_FORMAT_RGBA16F))
        return FALSE;
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return FALSE;
    v3d_texture_load(ogles2_device(), face_gpu(t, k, l, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    for (j = 0; j < h; j++)
        if (depth_type(t->hw_type))
            unpack_depth(t, packed + (ULONG)j * row, (ULONG *)(rgba + (ULONG)j * stride), (ULONG)w);
        else if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F)
            unpack_half(packed + (ULONG)j * row, (GLfloat *)(rgba + (ULONG)j * stride), (ULONG)w);
        else
            unpack(t, packed + (ULONG)j * row, rgba + (ULONG)j * stride, (ULONG)w);
    FreeVec(packed);
    return TRUE;
}

/* The RAM copies of every level the GPU drew into, read back. */
static void sync(struct OGLES2Texture *t)
{
    int k, l;
    for (k = 0; k < t->nf; k++) {
        struct OGLES2Face *f = &t->f[k];
        if (!f->rendered)
            continue;
        for (l = 0; l < MAX_LEVELS; l++)
            if ((f->rendered & (1UL << l)) && f->shadow[l]) {
                load_box(t, k, l, 0, 0, f->w[l], f->h[l], f->shadow[l],
                         (ULONG)f->w[l] * copy_bytes(level_class(f, l)));
                f->blank &= ~(1UL << l);
            }
        f->rendered = 0;
    }
}

/* A whole level stored by the TFU from a raster copy in the stored format
 * (`row` bytes a row), where it beats the CPU's tiling (v3d_texture_store),
 * the copy in memory the GPU reaches, a format the TFU takes. The job's
 * fixed cost is a cache clean, the lock and a few bridge writes, 0.3-0.5 ms
 * on the A1200, then about 1.9 ms a megapixel; the CPU tiles RGBA8 into UIF
 * directly at 5.6 ms a megapixel, other formats at about 40: so 512x512 and
 * up for RGBA8, 128x128 for the rest. The caller has dealt with the jobs
 * that use the texture (before_change), as for the CPU's store. Probe U
 * keeps the CPU's. FALSE: the CPU stores it. */
#define TFU_UPLOAD_MIN_TEXELS       (128UL * 128UL)
#define TFU_UPLOAD_MIN_TEXELS_RGBA8 (512UL * 512UL)
#define TFU_LOCK_TIMEOUT_MS 5000 /* job.c's GPU_LOCK_TIMEOUT_MS */
static BOOL store_tfu(struct OGLES2Texture *t, int k, int l, const UBYTE *raster, ULONG row, int w, int h)
{
    V3DTexture tmp, *g;
    ULONG len;
    int r;
    if (cpu_uploads ||
        (ULONG)w * (ULONG)h < (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA8 ? TFU_UPLOAD_MIN_TEXELS_RGBA8
                                                                          : TFU_UPLOAD_MIN_TEXELS) ||
        !v3d_tfu_supports_mipmaps(t->hw_type) || !ogles2_device_gpu_reachable((ULONG)raster, row * (ULONG)h) ||
        t->target == GL_TEXTURE_3D)
        return FALSE;
    g = face_gpu(t, k, l, &tmp);
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the copy in memory, and no dirty line over the level */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return FALSE;
    r = v3d_tfu_store_raster(ogles2_device(), g, (v3d_u8)l, t->hw_type, (v3d_u16)w, (v3d_u16)h, raster, row);
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r == -2)
        ogles2_log("texture %ld: the TFU did not finish an upload (%ldx%ld): the CPU stores it",
                   (ULONG)t->name, (ULONG)w, (ULONG)h);
    if (r)
        return FALSE;
    /* The level's memory as the TFU left it: no CPU cache line from before
     * (a later glTexSubImage2D's partial store reads them). */
    len = g->texture_mem.size;
    CachePostDMA(g->texture_mem.hostptr, &len, 0);
    return TRUE;
}

/* A box of level l of face k, from the RAM copy `src` (`w` texels a row, of
 * level 0's class: complete() saw to it). S3TC: the box on block bounds,
 * `src` its blocks, stored as they are (the TFU takes no BC format). */
static void store_box(struct OGLES2Texture *t, int k, int l, const UBYTE *src, int x, int y, int w,
                      int h)
{
    V3DTexture tmp;
    ULONG row = (ULONG)w * t->cpp, in;
    UBYTE *packed;
    BOOL whole = x == 0 && y == 0 && w == t->f[k].w[l] && h == t->f[k].h[l];
    int j;
    if (class_of(t->f[0].format[0], t->f[0].type[0]) == CLASS_BLOCKS) {
        v3d_texture_store(ogles2_device(), face_gpu(t, k, l, &tmp), (v3d_u8)l, src, blocks4((ULONG)w) * t->cpp,
                          x / 4, y / 4, (int)blocks4((ULONG)w), (int)blocks4((ULONG)h));
        return;
    }
    in = (ULONG)w * copy_bytes(class_of(t->f[0].format[0], t->f[0].type[0]));
    /* RGBA8 is stored as the RAM copy holds it: straight from there. */
    if (t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA8) {
        if (whole && store_tfu(t, k, l, src, (ULONG)w * 4, w, h))
            return;
        v3d_texture_store(ogles2_device(), face_gpu(t, k, l, &tmp), (v3d_u8)l, src, (ULONG)w * 4, x, y, w, h);
        return;
    }
    if (!(packed = AllocVec(row * h + 4, MEMF_ANY)))
        return;
    for (j = 0; j < h; j++)
        if (float_type(t->hw_type) || depth_type(t->hw_type) || encoded_type(t->hw_type) || int_type(t->hw_type))
            pack_wide(t, src + (ULONG)j * in, packed + (ULONG)j * row, (ULONG)w);
        else
            pack(t, src + (ULONG)j * in, packed + (ULONG)j * row, (ULONG)w);
    if (!whole || !store_tfu(t, k, l, packed, row, w, h))
        v3d_texture_store(ogles2_device(), face_gpu(t, k, l, &tmp), (v3d_u8)l, packed, row, x, y, w, h);
    FreeVec(packed);
}

static void upload_level(struct OGLES2Texture *t, int k, int l)
{
    store_box(t, k, l, t->f[k].shadow[l], 0, 0, t->f[k].w[l], t->f[k].h[l]);
}

/* A cube map's or 2D array's memory: its faces (layers) each of the 2D
 * layout v3d_texture_alloc(_mipchain) made, `face_stride` apart (Mesa: the
 * tree's end aligned to 64 bytes; a page here, so each face's level 0 keeps
 * the alignment UIF XOR needs). */
static BOOL cube_memory(struct OGLES2Texture *t)
{
    V3DTexture *g = t->gpu;
    v3d_mem m;
    ULONG tree = g->levels[0].offset + (ULONG)g->levels[0].padded_h * g->levels[0].stride;
    t->face_stride = (tree + 4095) & ~4095UL;
    if (v3d_mem_alloc(ogles2_device(), &m, t->face_stride * (ULONG)faces(t) + 256) < 0)
        return FALSE;
    v3d_mem_free(ogles2_device(), &g->texture_mem);
    g->texture_mem = m;
    return TRUE;
}

/* A 3D texture's memory, Mesa's v3d_setup_slices: each level's slices one
 * after another, smallest level first, a level's slice size times its
 * depth (from level 1 on the depth of the power-of-two padded chain), with
 * level 1's page alignment and level 0 on a page (v3dcore's 2D rules, its
 * v3d_texture.c); the widths, heights and tilings are the 2D layout's.
 * face_stride is level 0's slice size: the texture state's array stride. */
#define UIF_PAGE 4096 /* v3d_texture.c's V3D_UIFCFG_PAGE_SIZE */
#define PAGE_CACHE_MINUS_1_5_UB_ROWS 26 /* and V3D_PAGE_CACHE_MINUS_1_5_UB_ROWS */
static BOOL volume_memory(struct OGLES2Texture *t)
{
    V3DTexture *g = t->gpu;
    v3d_mem m;
    ULONG offset = 0, pad, sz, total, utile_w = v3d_utile_width((int)t->cpp), mb_h = 2 * v3d_utile_height((int)t->cpp);
    ULONG d1 = t->d[0] > 1 ? (ULONG)t->d[0] / 2 : 1, pot_d = 1, dl;
    int i;
    while (pot_d < d1)
        pot_d *= 2;
    pot_d *= 2;
    for (i = (int)g->num_levels - 1; i >= 0; i--) {
        sz = (ULONG)g->levels[i].padded_h * g->levels[i].stride;
        dl = i < 1 ? t->d[0] : pot_d >> i;
        total = sz * (dl ? dl : 1);
        if (i == 1 && g->levels[i].padded_w > 4 * (utile_w * 2) &&
            g->levels[i].padded_h > PAGE_CACHE_MINUS_1_5_UB_ROWS * mb_h)
            total = (total + UIF_PAGE - 1) & ~(ULONG)(UIF_PAGE - 1);
        g->levels[i].offset = offset;
        offset += total;
    }
    pad = ((g->levels[0].offset + UIF_PAGE - 1) & ~(ULONG)(UIF_PAGE - 1)) - g->levels[0].offset;
    for (i = 0; i < (int)g->num_levels; i++)
        g->levels[i].offset += pad;
    t->face_stride = (ULONG)g->levels[0].padded_h * g->levels[0].stride;
    if (v3d_mem_alloc(ogles2_device(), &m, offset + pad + 256) < 0)
        return FALSE;
    v3d_mem_free(ogles2_device(), &g->texture_mem);
    g->texture_mem = m;
    return TRUE;
}

/* (Re)creates the GPU texture for the levels given and uploads them. */
static BOOL upload_all(struct OGLES2Texture *t)
{
    V3DDevice *dev = ogles2_device();
    const struct OGLES2Face *f0 = &t->f[0];
    int l, k, n = 1;
    sync(t);
    free_gpu(t);
    if (!base_ok(t))
        return TRUE; /* nothing to sample yet (a cube map: until all faces agree) */
    for (l = 1; l < MAX_LEVELS; l++)
        if (consistent(f0, l) && depth_ok(t, l))
            n = l + 1;
    if (!(t->gpu = AllocVec(sizeof(*t->gpu), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    storage_of(t);
    if (v3d_texture_alloc_blocks(dev, t->gpu, f0->w[0], f0->h[0], t->cpp,
                                 level_class(f0, 0) == CLASS_BLOCKS ? 4 : 1) < 0 ||
        (n > 1 && v3d_texture_alloc_mipchain(dev, t->gpu, (v3d_u8)n) < 0)) {
        free_gpu(t);
        return FALSE;
    }
    /* Growing the chain parks the level-0 block it replaces on v3dcore's
     * park list for PiStorm3D's mid-frame renaming. Here nothing can use it
     * (the job that last drew with this texture was rendered before the
     * change, before_change), and ogles2 is the list's only user: free it. */
    if (n > 1)
        v3d_texture_drain_parked_all(dev);
    if (t->target == GL_TEXTURE_3D ? !volume_memory(t) : faces(t) > 1 && !cube_memory(t)) {
        free_gpu(t);
        return FALSE;
    }
    {
        extern int g_v3d_uif_fast; /* v3dcore's direct UIF store, checked at its first use */
        static BOOL told;
        if (g_v3d_uif_fast == 2 && !told) {
            told = TRUE;
            ogles2_log("texture: v3dcore's direct UIF store differed from its general one: not used");
        }
    }
    /* A blank level is in the new copy already: its memory starts cleared
     * (SDL's testrendercopyex makes a render target a frame: 1.2 MB of
     * zeros stored at 640x480). */
    for (k = 0; k < faces(t); k++)
        for (l = 0; l < n; l++)
            if ((l == 0 || consistent(&t->f[k], l)) && !(t->f[k].blank & (1UL << l)))
                upload_level(t, k, l);
    return TRUE;
}

static BOOL face_target(GLenum target)
{
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

/* The bound texture an image call's target names, and the face (*k);
 * 0 with GL_INVALID_ENUM set for another target. A rectangle texture
 * (ARB_texture_rectangle) has level 0 only: GL_INVALID_VALUE otherwise. */
static struct OGLES2Texture *image_texture(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target,
                                           GLint level, int *k)
{
    *k = 0;
    if (face_target(target)) {
        *k = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        return ts->unitcube[ts->active];
    }
    if (target == GL_TEXTURE_2D)
        return ts->unit2d[ts->active];
    if (target == GL_TEXTURE_RECTANGLE) {
        if (level != 0) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return 0;
        }
        return ts->unitrect[ts->active];
    }
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

/* ---- for framebuffer objects (fbo.c) ---- */

struct OGLES2Texture *ogles2_texture_get(struct OGLES2Context *ctx, GLuint name)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    return ts ? by_name(ts, name) : 0;
}

GLuint ogles2_texture_name(const struct OGLES2Texture *t)
{
    return t->name;
}

GLenum ogles2_texture_target(const struct OGLES2Texture *t)
{
    return t->target;
}

BOOL ogles2_texture_image(const struct OGLES2Texture *t, int k, int l, UWORD *w, UWORD *h,
                          GLenum *format, GLenum *type)
{
    const struct OGLES2Face *f = &t->f[k];
    if (k < 0 || k > 5 || l < 0 || l >= MAX_LEVELS || !(f->defined & (1UL << l)))
        return FALSE;
    *w = f->w[l];
    *h = f->h[l];
    *format = f->format[l];
    *type = f->type[l];
    return TRUE;
}

/* GLES2's colour-renderable texture images (plan 7.7): RGB and RGBA in the
 * formats the TLB stores; the GPU copy is in level 0's format, so the level
 * must have level 0's format and type. Half-float RGBA too
 * (EXT_color_buffer_half_float, which names RGBA only for textures, as
 * dEQP checks): stored as RGBA16F, which the TLB writes from a 16F render
 * target. */
BOOL ogles2_texture_renderable(const struct OGLES2Texture *t, int k, int l)
{
    const struct OGLES2Face *f = &t->f[k], *f0 = &t->f[0];
    GLenum fm, ty;
    if (!(f->defined & (1UL << l)) || !(f0->defined & 1))
        return FALSE;
    fm = f->format[l];
    ty = f->type[l];
    if (fm != f0->format[0] || ty != f0->type[0])
        return FALSE;
    /* A BGRA_EXT image is kept as RGBA8 (only its uploads swap), so it is
     * colour-renderable as RGBA is (EXT_texture_format_BGRA8888). */
    return (ty == GL_UNSIGNED_BYTE && (fm == GL_RGB || fm == GL_RGBA || fm == GL_BGRA_EXT)) ||
           (ty == GL_UNSIGNED_SHORT_5_6_5 && fm == GL_RGB) ||
           ((ty == GL_UNSIGNED_SHORT_4_4_4_4 || ty == GL_UNSIGNED_SHORT_5_5_5_1) && fm == GL_RGBA) ||
           (ty == GL_HALF_FLOAT_OES && fm == GL_RGBA);
}

BOOL ogles2_texture_half(const struct OGLES2Texture *t, int k, int l)
{
    return ogles2_texture_renderable(t, k, l) && t->f[k].type[l] == GL_HALF_FLOAT_OES;
}

BOOL ogles2_texture_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, int l,
                            struct OGLES2Surface *s)
{
    const V3DTexture *g;
    ULONG cpp;
    if (!ogles2_texture_renderable(t, k, l))
        return FALSE;
    if ((!t->gpu || l >= t->gpu->num_levels) && !upload_all(t))
        return FALSE;
    if (!(g = t->gpu) || l >= g->num_levels)
        return FALSE; /* not in the chain (a level of another size), or a cube map not complete */
    cpp = t->cpp;
    s->addr = V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256) + (ULONG)k * t->face_stride +
              g->levels[l].offset;
    s->bytes = g->levels[l].stride * g->levels[l].padded_h;
    s->mem_format = g->levels[l].tiling_format;
    s->format = t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB565 ? V3D_OUTPUT_IMAGE_FORMAT_BGR565
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA4  ? V3D_OUTPUT_IMAGE_FORMAT_ABGR4444
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGB5A1 ? V3D_OUTPUT_IMAGE_FORMAT_ABGR1555
              : t->hw_type == V3D_TEXTURE_DATA_FORMAT_RGBA16F ? V3D_OUTPUT_IMAGE_FORMAT_RGBA16F
                                                             : V3D_OUTPUT_IMAGE_FORMAT_RGBA8;
    s->force_alpha1 = t->f[k].format[l] == GL_RGB;
    s->ub = s->mem_format == V3D_MEMORY_FORMAT_UIF_NO_XOR || s->mem_format == V3D_MEMORY_FORMAT_UIF_XOR
                ? g->levels[l].padded_h / (2 * v3d_utile_height((int)cpp))
                : 0;
    (void)ctx;
    return TRUE;
}

/* OES_depth_texture: level l of a 2D texture is a depth image; its depth
 * bits (16 or 24) and whether it holds stencil too (DEPTH_STENCIL_OES). */
BOOL ogles2_texture_depth(const struct OGLES2Texture *t, int l, UBYTE *bits, BOOL *stencil)
{
    const struct OGLES2Face *f = &t->f[0];
    int c;
    if (t->target != GL_TEXTURE_2D || l < 0 || l >= MAX_LEVELS || !(f->defined & (1UL << l)))
        return FALSE;
    c = level_class(f, l);
    if (c != CLASS_DEPTH16 && c != CLASS_DEPTH24)
        return FALSE;
    *bits = c == CLASS_DEPTH16 ? 16 : 24;
    *stencil = f->format[l] == GL_DEPTH_STENCIL_OES;
    return TRUE;
}

/* A depth texture's level as a job's depth (and stencil) image, its GPU
 * storage made first: the tile buffer stores into the level's own layout
 * as D16 or D24S8, which the TMU then samples as DEPTH_COMP16 or
 * DEPTH24_X8 (Mesa's depth textures). FALSE: none. */
BOOL ogles2_texture_zs_surface(struct OGLES2Context *ctx, struct OGLES2Texture *t, int l,
                               struct OGLES2Surface *s)
{
    const V3DTexture *g;
    UBYTE bits;
    BOOL st;
    (void)ctx;
    if (!ogles2_texture_depth(t, l, &bits, &st) || !consistent(&t->f[0], l))
        return FALSE;
    if ((!t->gpu || l >= t->gpu->num_levels) && !upload_all(t))
        return FALSE;
    if (!(g = t->gpu) || l >= g->num_levels)
        return FALSE;
    s->addr = V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256) + g->levels[l].offset;
    s->bytes = g->levels[l].stride * g->levels[l].padded_h;
    s->mem_format = g->levels[l].tiling_format;
    s->format = bits == 16 ? V3D_OUTPUT_IMAGE_FORMAT_D16 : V3D_OUTPUT_IMAGE_FORMAT_D24S8;
    s->force_alpha1 = FALSE;
    s->ub = s->mem_format == V3D_MEMORY_FORMAT_UIF_NO_XOR || s->mem_format == V3D_MEMORY_FORMAT_UIF_XOR
                ? g->levels[l].padded_h / (2 * v3d_utile_height((int)t->cpp))
                : 0;
    return TRUE;
}

/* draw.c: unit `unit`'s texture of that kind returns 32 bits (by its level
 * 0's format; an incomplete one samples the RGBA8 fallback, which the
 * 32-bit variants read correctly too). */
BOOL ogles2_texture_wide(struct OGLES2Context *ctx, int unit, int kind)
{
    struct OGLES2TexState *ts = ctx->tex;
    const struct OGLES2Texture *t;
    if (!ts || kind == OGLES2_TEX_NONE)
        return FALSE;
    t = unit_texture(ts, kind, unit);
    /* A depth comparison returns 16 bits (Mesa's v3d_get_tex_return_size). */
    if ((ts->unitsmp[unit] ? &ts->unitsmp[unit]->s : &t->s)->compare_mode == GL_COMPARE_REF_TO_TEXTURE)
        return FALSE;
    return (t->f[0].defined & 1) && level_wide(&t->f[0], 0);
}

void ogles2_texture_rendered(struct OGLES2Texture *t, int k, int l)
{
    t->f[k].rendered |= 1UL << l;
    t->f[k].blank &= ~(1UL << l);
}

/* A half-float level read as RGBA bytes (glReadPixels' RGBA/UNSIGNED_BYTE):
 * each value clamped to [0,1] and rounded, as GLES 2.0 4.3.1 converts a
 * colour for an unsigned normalized type. */
static UBYTE float_byte(GLfloat v)
{
    return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (UBYTE)(v * 255.0f + 0.5f);
}

static BOOL read_half(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *rgba,
                      ULONG stride)
{
    const struct OGLES2Face *f = &t->f[k];
    ULONG row = (ULONG)w * 16, i;
    GLfloat *box = AllocVec(row * h + 4, MEMF_ANY);
    int j;
    if (!box)
        return FALSE;
    if (!load_box(t, k, l, x, y, w, h, (UBYTE *)box, row)) {
        if (!f->shadow[l]) { /* no GPU copy (a state-only context): the RAM copy */
            FreeVec(box);
            return FALSE;
        }
        for (j = 0; j < h; j++)
            memcpy((UBYTE *)box + (ULONG)j * row, f->shadow[l] + ((ULONG)(y + j) * f->w[l] + x) * 16, row);
    }
    for (j = 0; j < h; j++) {
        const GLfloat *s = (const GLfloat *)((UBYTE *)box + (ULONG)j * row);
        UBYTE *d = rgba + (ULONG)j * stride;
        for (i = 0; i < (ULONG)w * 4; i++)
            d[i] = float_byte(s[i]);
        if (f->format[l] == GL_RGB) /* the stored alpha is the shader's: an RGB image reads 1 */
            for (i = 3; i < (ULONG)w * 4; i += 4)
                d[i] = 255;
    }
    FreeVec(box);
    return TRUE;
}

BOOL ogles2_texture_read(struct OGLES2Texture *t, int k, int l, int x, int y, int w, int h,
                         UBYTE *rgba, ULONG stride)
{
    const struct OGLES2Face *f = &t->f[k];
    int j;
    if (ogles2_texture_half(t, k, l))
        return read_half(t, k, l, x, y, w, h, rgba, stride);
    if (level_class(f, l) != CLASS_RGBA8)
        return FALSE;
    if (load_box(t, k, l, x, y, w, h, rgba, stride))
        return TRUE;
    if (!f->shadow[l]) /* no GPU copy (a state-only context): the RAM copy */
        return FALSE;
    for (j = 0; j < h; j++)
        memcpy(rgba + (ULONG)j * stride, f->shadow[l] + ((ULONG)(y + j) * f->w[l] + x) * 4, (ULONG)w * 4);
    return TRUE;
}

struct OGLES2Texture *ogles2_texture_hidden(struct OGLES2Context *ctx, UWORD w, UWORD h, GLenum format,
                                            GLenum type)
{
    struct OGLES2Texture *t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR);
    if (!t)
        return 0;
    init_object(t, 0, GL_TEXTURE_2D);
    t->s.min_filter = GL_NEAREST;
    t->f[0].shadow[0] = AllocVec((ULONG)w * h * 4 + 4, MEMF_ANY | MEMF_CLEAR);
    t->f[0].w[0] = w;
    t->f[0].h[0] = h;
    t->f[0].format[0] = format;
    t->f[0].type[0] = type;
    t->f[0].defined = 1;
    t->f[0].blank = 1;
    if (!t->f[0].shadow[0] || (ctx->v3d && (!upload_all(t) || !t->gpu))) {
        ogles2_texture_hidden_free(ctx, t);
        return 0;
    }
    return t;
}

void ogles2_texture_hidden_free(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    (void)ctx;
    if (!t)
        return;
    t->f[0].rendered = 0;
    free_levels(t);
    FreeVec(t);
}

/* ---- for glCopyTex* on the GPU (copy.c) ---- */

/* The default framebuffer's region as a texture: w x h RGBA8, filled by the
 * TFU from the raster image at `src` (rows `stride` bytes apart, the top row
 * first, in the render bitmap the GPU drew), red and blue swapped in the
 * sampler for a BGRA bitmap. One per context, made again for another size.
 * The caller has rendered and finished the context's jobs: none still reads
 * the scratch texture, none still writes the bitmap. 0: the TFU could not
 * (the CPU copies). */
struct OGLES2Texture *ogles2_texture_scratch(struct OGLES2Context *ctx, UWORD w, UWORD h, ULONG src, ULONG stride,
                                             BOOL bgra)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *t;
    UBYTE sw0, sw2;
    int r;
    if (!ts || !ctx->v3d)
        return 0;
    if ((t = ts->scratch) && (t->f[0].w[0] != w || t->f[0].h[0] != h)) {
        ogles2_texture_hidden_free(ctx, t);
        ts->scratch = t = 0;
    }
    if (!t && !(t = ts->scratch = ogles2_texture_hidden(ctx, w, h, GL_RGBA, GL_UNSIGNED_BYTE)))
        return 0;
    CacheClearE(0, ~0UL, CACRF_ClearD); /* no dirty CPU line over the texture's memory */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return 0;
    r = v3d_tfu_store_raster(ogles2_device(), t->gpu, 0, t->hw_type, w, h, (const void *)src, stride);
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r) {
        ogles2_log("glCopyTexSubImage2D: the TFU did not copy %ldx%ld from the framebuffer (%ld): the CPU copies",
                   (ULONG)w, (ULONG)h, (LONG)r);
        return 0;
    }
    t->f[0].rendered = 1; /* its RAM copy is not the picture (never read: nothing changes it) */
    t->f[0].blank = 0;
    sw0 = bgra ? v3d_SWIZZLE_BLUE : v3d_SWIZZLE_RED;
    sw2 = bgra ? v3d_SWIZZLE_RED : v3d_SWIZZLE_BLUE;
    if (t->swizzle[0] != sw0 || t->swizzle[2] != sw2) {
        t->swizzle[0] = sw0;
        t->swizzle[2] = sw2;
        t->pver++; /* the state records hold the swizzle */
    }
    return t;
}

/* Texture t on unit 0's 2D target for the copy's draw (draw.c); the one
 * there before is returned, to be put back. */
struct OGLES2Texture *ogles2_texture_unit0(struct OGLES2Context *ctx, struct OGLES2Texture *t)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *old;
    if (!ts)
        return 0;
    old = ts->unit2d[0];
    ts->unit2d[0] = t;
    /* Unit 0's sampler object is set aside for the copy's draw (the first
     * call) and put back with the texture (the second). */
    if (!ts->copying) {
        ts->copy_smp = ts->unitsmp[0];
        ts->unitsmp[0] = 0;
    } else {
        ts->unitsmp[0] = ts->copy_smp;
    }
    ts->copying = !ts->copying;
    return old;
}

/* A framebuffer object's colour texture sampled by the copy: level 0,
 * nearest, clamped, whatever the application set (put back with
 * ogles2_texture_params_restore). */
void ogles2_texture_params_copy(struct OGLES2Texture *t, struct OGLES2TexParams *saved)
{
    saved->s = t->s;
    saved->base_level = t->base_level;
    saved->max_level = t->max_level;
    saved->lod_bias = t->lod_bias;
    memcpy(saved->gl_swizzle, t->gl_swizzle, sizeof(saved->gl_swizzle));
    t->gl_swizzle[0] = GL_RED; /* a copy reads the texels as they are */
    t->gl_swizzle[1] = GL_GREEN;
    t->gl_swizzle[2] = GL_BLUE;
    t->gl_swizzle[3] = GL_ALPHA;
    t->s.compare_mode = GL_NONE;
    t->s.min_filter = t->s.mag_filter = GL_NEAREST;
    t->s.wrap_s = t->s.wrap_t = GL_CLAMP_TO_EDGE;
    t->base_level = t->max_level = 0;
    t->s.max_aniso = 1.0f;
    t->lod_bias = 0.0f;
    t->pver++;
}

void ogles2_texture_params_restore(struct OGLES2Texture *t, const struct OGLES2TexParams *saved)
{
    t->s = saved->s;
    t->base_level = saved->base_level;
    t->max_level = saved->max_level;
    t->lod_bias = saved->lod_bias;
    memcpy(t->gl_swizzle, saved->gl_swizzle, sizeof(t->gl_swizzle));
    t->pver++;
}

static void set_level(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, GLint level, UBYTE *img,
                      GLsizei width, GLsizei height, GLenum format, GLenum type, BOOL given, GLenum sized);

/* OES_required_internalformat: a sized colour format's base format (0 for
 * any other value). */
static GLenum sized_color_base(GLenum ifmt)
{
    switch (ifmt) {
    case GL_RGBA8_OES: case GL_RGBA4: case GL_RGB5_A1: return GL_RGBA;
    case GL_RGB8_OES: case GL_RGB565:                  return GL_RGB;
    case GL_ALPHA8_OES:                                return GL_ALPHA;
    case GL_LUMINANCE8_OES:                            return GL_LUMINANCE;
    case GL_LUMINANCE8_ALPHA8_OES:                     return GL_LUMINANCE_ALPHA;
    default:                                           return 0;
    }
}

/* OES_required_internalformat in glTexImage2D: a sized internal format
 * stands for the base format, given a format and type that fill it with at
 * least its precision (the image keeps the type's, as an unsized one: GLES
 * lets an implementation store more). The value as given if it is not a
 * sized format; NOT_FILLED for one that format and type cannot fill
 * (GL_INVALID_OPERATION). */
#define NOT_FILLED 0xFFFFFFFFUL /* no GL value: 0 is an internal format to refuse too */
static GLenum sized_base(GLenum ifmt, GLenum format, GLenum type)
{
    GLenum base = sized_color_base(ifmt), f, ty;
    BOOL ok;
    if (int_sized(ifmt, &f, &ty)) /* ES 3.0's integer formats: their own format and type only */
        return f == format && ty == type ? format : NOT_FILLED;
    switch (ifmt) {
    case GL_RGBA4:   ok = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_4_4_4_4; break;
    case GL_RGB5_A1: ok = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_5_5_5_1; break;
    case GL_RGB565:  ok = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT_5_6_5; break;
    case GL_DEPTH_COMPONENT16:
        return format == GL_DEPTH_COMPONENT && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)
                   ? GL_DEPTH_COMPONENT : NOT_FILLED;
    case GL_DEPTH24_STENCIL8_OES:
        return format == GL_DEPTH_STENCIL_OES && type == GL_UNSIGNED_INT_24_8_OES ? GL_DEPTH_STENCIL_OES : NOT_FILLED;
    /* ES 3.0's (table 3.2) of what the library stores (stage B item 8) */
    case GL_DEPTH_COMPONENT24:
        return format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_INT ? GL_DEPTH_COMPONENT : NOT_FILLED;
    case GL_R8:
    case GL_RG8:
        return format == (ifmt == GL_R8 ? GL_RED : GL_RG) && type == GL_UNSIGNED_BYTE ? format : NOT_FILLED;
    case GL_R16F:
    case GL_RG16F:
        return format == (ifmt == GL_R16F ? GL_RED : GL_RG) && (type == GL_HALF_FLOAT_OES || type == GL_FLOAT)
                   ? format : NOT_FILLED;
    case GL_R32F:
    case GL_RG32F:
        return format == (ifmt == GL_R32F ? GL_RED : GL_RG) && type == GL_FLOAT ? format : NOT_FILLED;
    case GL_R8_SNORM: return format == GL_RED && type == GL_BYTE ? format : NOT_FILLED;
    case GL_RG8_SNORM: return format == GL_RG && type == GL_BYTE ? format : NOT_FILLED;
    case GL_RGB8_SNORM: return format == GL_RGB && type == GL_BYTE ? format : NOT_FILLED;
    case GL_RGBA8_SNORM: return format == GL_RGBA && type == GL_BYTE ? format : NOT_FILLED;
    case GL_SRGB8: return format == GL_RGB && type == GL_UNSIGNED_BYTE ? format : NOT_FILLED;
    case GL_SRGB8_ALPHA8: return format == GL_RGBA && type == GL_UNSIGNED_BYTE ? format : NOT_FILLED;
    case GL_RGB10_A2: return format == GL_RGBA && type == GL_UNSIGNED_INT_2_10_10_10_REV ? format : NOT_FILLED;
    case GL_R11F_G11F_B10F:
        return format == GL_RGB && (type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == GL_HALF_FLOAT_OES ||
                                    type == GL_FLOAT) ? format : NOT_FILLED;
    case GL_RGB9_E5:
        return format == GL_RGB && (type == GL_UNSIGNED_INT_5_9_9_9_REV || type == GL_HALF_FLOAT_OES ||
                                    type == GL_FLOAT) ? format : NOT_FILLED;
    case GL_DEPTH_COMPONENT32F:
        return format == GL_DEPTH_COMPONENT && type == GL_FLOAT ? format : NOT_FILLED;
    case GL_DEPTH32F_STENCIL8:
        return format == GL_DEPTH_STENCIL_OES && type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? format : NOT_FILLED;
    case GL_RGBA16F:
    case GL_RGB16F:
        return format == (ifmt == GL_RGBA16F ? GL_RGBA : GL_RGB) && (type == GL_HALF_FLOAT_OES || type == GL_FLOAT)
                   ? format : NOT_FILLED;
    case GL_RGBA32F:
    case GL_RGB32F:
        return format == (ifmt == GL_RGBA32F ? GL_RGBA : GL_RGB) && type == GL_FLOAT ? format : NOT_FILLED;
    default:
        if (!base)
            return ifmt;
        ok = type == GL_UNSIGNED_BYTE;
        break;
    }
    return ok && format == base ? base : NOT_FILLED;
}

static void tex_image(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum err, keep;
    UBYTE *img;
    ULONG cb;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    type = half_type(ctx, type);
    if (rg_refused(ctx, format, type))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    if (level < 0 || level >= MAX_LEVELS || width < 0 || height < 0 || border != 0 ||
        width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) ||
        (face_target(target) && width != height)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* EXT_texture_format_BGRA8888 (revision 1.4 on): its sized format names
     * the same image as BGRA_EXT. */
    if ((GLenum)internalformat == GL_BGRA8_EXT && format == GL_BGRA_EXT)
        internalformat = GL_BGRA_EXT;
    /* OES_required_internalformat (GLES 3 programs and dEQP's clip_control
     * cases pass GL_RGBA8). */
    if (sized_base((GLenum)internalformat, format, type) == NOT_FILLED) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (int_components((GLenum)internalformat)) { /* ES 3.0: integer textures take sized formats only */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    keep = keep_sized((GLenum)internalformat);
    internalformat = (GLint)sized_base((GLenum)internalformat, format, type);
    if ((GLenum)internalformat != format) {
        ogles2_error(ctx, internalformat == GL_RGBA || internalformat == GL_RGB ||
                          internalformat == GL_LUMINANCE || internalformat == GL_ALPHA ||
                          internalformat == GL_LUMINANCE_ALPHA || internalformat == GL_DEPTH_COMPONENT ||
                          internalformat == GL_DEPTH_STENCIL_OES || internalformat == GL_RED ||
                          internalformat == GL_RG ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    /* OES_depth_texture: 2D textures only (cube maps would need
     * OES_depth_texture_cube_map); ES 3.0 has cube maps too. */
    if ((format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES) && target != GL_TEXTURE_2D &&
        !(ogles2_es3(ctx) && face_target(target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    if (t->immutable_levels) { /* ES 3.0 3.8.4 */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!unpack_source(ctx, &pixels, unpack_bytes(ctx, format, type, width, height, 1)))
        return;
    before_change(ctx, ts, t);
    t->es3_rules = ogles2_es3(ctx);
    cb = copy_bytes(class_of(format, type));
    img = width && height ? AllocVec((ULONG)width * height * cb, MEMF_ANY | MEMF_CLEAR) : 0;
    if (width && height && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (class_of(format, type) != CLASS_RGBA8)
        ogles2_wide_seen |= wide_class(format, type);
    if (img && pixels) {
        ULONG row, image;
        const UBYTE *src = unpack_image(ctx, pixels, format, type, width, height, &row, &image);
        convert(img, (ULONG)width * cb, src, format, type, width, height, row);
    }
    set_level(ctx, t, k, level, img, width, height, format, type, pixels != 0, keep);
}

/* A face's level becomes `img`, its new RAM copy (0: empty); `given`: it
 * holds the image's texels, else zeros. */
static void put_level(struct OGLES2Face *f, int level, UBYTE *img, GLsizei width, GLsizei height, GLenum format,
                      GLenum type, BOOL given, GLenum sized)
{
    f->sized[level] = sized;
    if (f->shadow[level])
        FreeVec(f->shadow[level]);
    f->shadow[level] = img;
    f->w[level] = (UWORD)width;
    f->h[level] = (UWORD)height;
    f->format[level] = format;
    f->type[level] = type;
    f->defined |= 1UL << level;
    if (img && given)
        f->blank &= ~(1UL << level);
    else
        f->blank |= 1UL << level;
}

/* A face's level no longer exists (a 2D array or 3D level given fewer layers). */
static void drop_level(struct OGLES2Face *f, int level)
{
    if (f->shadow[level])
        FreeVec(f->shadow[level]);
    f->shadow[level] = 0;
    f->defined &= ~(1UL << level);
    f->blank &= ~(1UL << level);
    f->rendered &= ~(1UL << level);
}

/* Level `level` of face k becomes `img`, its new RAM copy (0: empty);
 * `given`: it holds the image's texels, else zeros. The caller has dealt
 * with the jobs that use the texture (before_change). */
static void set_level(struct OGLES2Context *ctx, struct OGLES2Texture *t, int k, GLint level, UBYTE *img,
                      GLsizei width, GLsizei height, GLenum format, GLenum type, BOOL given, GLenum sized)
{
    struct OGLES2Face *f = &t->f[k];
    put_level(f, level, img, width, height, format, type, given, sized);
    if (!ctx->v3d)
        return; /* a state-only context keeps the images only */
    /* No GPU texture yet, a new level 0, or a level the chain has no room
     * for: rebuilt at its first use (a draw, ogles2_texture_state; a
     * framebuffer, ogles2_texture_surface). A program sends level 0 and
     * then each mip level: rebuilding at each stored every level before it
     * again (Quake III's load, M6). No job uses the old copy (before_change). */
    if (!t->gpu || (level == 0 && (faces(t) == 1 || t->gpu->width != f->w[0])) ||
        level >= t->gpu->num_levels || t->gpu->width != t->f[0].w[0] || t->gpu->height != t->f[0].h[0]) {
        free_gpu(t);
    } else if (consistent(f, level) && img) {
        upload_level(t, k, level);
    }
}

/* Timed for probe P (frame.c). */
void ogles2_glTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    ULONG t0 = ogles2_clock_us();
    tex_image(ctx, target, level, internalformat, width, height, border, format, type, pixels);
    if (ogles2_context_valid(ctx))
        ctx->sum_teximage += (double)(ULONG)(ogles2_clock_us() - t0);
}

static void tex_sub_image(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    GLenum err;
    UBYTE *sub;
    ULONG cb;
    int y, k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    type = half_type(ctx, type);
    if (rg_refused(ctx, format, type))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    f = &t->f[k];
    if (level < 0 || level >= MAX_LEVELS || !(f->defined & (1UL << level))) {
        ogles2_error(ctx, level < 0 || level >= MAX_LEVELS ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return;
    }
    /* GLES 2.0 3.7.2: the level's own format; and the RAM copy's class
     * (float data into a float level, depth into a depth level). */
    if (format != f->format[level] || class_of(format, type) != level_class(f, level)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
        xoffset + width > f->w[level] || yoffset + height > f->h[level]) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!unpack_source(ctx, &pixels, unpack_bytes(ctx, format, type, width, height, 1)))
        return;
    if (!width || !height || !pixels)
        return;
    before_change(ctx, ts, t);
    cb = copy_bytes(level_class(f, level));
    if (!(sub = AllocVec((ULONG)width * height * cb, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    {
        ULONG row, image;
        const UBYTE *src = unpack_image(ctx, pixels, format, type, width, height, &row, &image);
        convert(sub, (ULONG)width * cb, src, format, type, width, height, row);
    }
    for (y = 0; y < height; y++)
        memcpy(f->shadow[level] + ((ULONG)(yoffset + y) * f->w[level] + xoffset) * cb,
               sub + (ULONG)y * width * cb, (ULONG)width * cb);
    f->blank &= ~(1UL << level);
    if (t->gpu && level < t->gpu->num_levels && consistent(f, level))
        store_box(t, k, level, sub, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    FreeVec(sub);
}

/* Timed for probe P (frame.c). */
void ogles2_glTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const void *pixels)
{
    ULONG t0 = ogles2_clock_us();
    tex_sub_image(ctx, target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (ogles2_context_valid(ctx))
        ctx->sum_teximage += (double)(ULONG)(ogles2_clock_us() - t0);
}

/* ---- 2D array and 3D textures (ES 3.0; stage B item 8) ----
 * Their layers (an array) or slices (3D) are faces, each level's images
 * given at once (a level of a 3D texture has d[l] of them), as glTexImage2D
 * gives a face's; the GPU copy as Mesa lays them out (cube_memory,
 * volume_memory). */

#define MAX_LAYERS 2048 /* GL_MAX_ARRAY_TEXTURE_LAYERS (state.c) */

/* Room for n faces (allocated past six). */
static BOOL faces_room(struct OGLES2Texture *t, int n)
{
    struct OGLES2Face *nf;
    if (n <= t->nf)
        return TRUE;
    if (!(nf = AllocVec((ULONG)n * sizeof(*nf), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    memcpy(nf, t->f, (ULONG)t->nf * sizeof(*nf));
    if (t->f != t->f6)
        FreeVec(t->f);
    t->f = nf;
    t->nf = (UWORD)n;
    return TRUE;
}

static struct OGLES2Texture *image_texture_3d(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target)
{
    if (target == GL_TEXTURE_3D)
        return ts->unit3d[ts->active];
    if (target == GL_TEXTURE_2D_ARRAY)
        return ts->unitarray[ts->active];
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

void ogles2_glTexImage3D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint internalformat,
                         GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                         const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum err, keep;
    ULONG cb, row, in;
    const UBYTE *src;
    int k, max;
    if (!ogles2_es3_entry(ctx) || !(ts = ts_get(ctx)) || !(t = image_texture_3d(ctx, ts, target)))
        return;
    type = half_type(ctx, type);
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    max = target == GL_TEXTURE_3D ? MAX_SIZE >> (level >= 0 && level < MAX_LEVELS ? level : 0) : MAX_LAYERS;
    if (level < 0 || level >= MAX_LEVELS || width < 0 || height < 0 || depth < 0 || border != 0 ||
        width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) || depth > max) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if ((GLenum)internalformat == GL_BGRA8_EXT && format == GL_BGRA_EXT) /* as glTexImage2D takes it */
        internalformat = GL_BGRA_EXT;
    if (sized_base((GLenum)internalformat, format, type) == NOT_FILLED) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (int_components((GLenum)internalformat)) { /* ES 3.0: integer textures take sized formats only */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    keep = keep_sized((GLenum)internalformat);
    internalformat = (GLint)sized_base((GLenum)internalformat, format, type);
    if ((GLenum)internalformat != format) {
        ogles2_error(ctx, internalformat == GL_RGBA || internalformat == GL_RGB ||
                          internalformat == GL_LUMINANCE || internalformat == GL_ALPHA ||
                          internalformat == GL_LUMINANCE_ALPHA || internalformat == GL_DEPTH_COMPONENT ||
                          internalformat == GL_DEPTH_STENCIL_OES || internalformat == GL_RED ||
                          internalformat == GL_RG ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    /* ES 3.0 3.8.3: no depth formats in 3D textures (arrays have them). */
    if ((format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES) && target == GL_TEXTURE_3D) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (t->immutable_levels) { /* ES 3.0 3.8.4 */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!unpack_source(ctx, &pixels, unpack_bytes(ctx, format, type, width, height, depth)))
        return;
    before_change(ctx, ts, t);
    t->es3_rules = TRUE;
    if (!faces_room(t, depth)) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (class_of(format, type) != CLASS_RGBA8)
        ogles2_wide_seen |= wide_class(format, type);
    cb = copy_bytes(class_of(format, type));
    src = unpack_image(ctx, pixels, format, type, width, height, &row, &in);
    for (k = 0; k < depth; k++) {
        UBYTE *img = width && height ? AllocVec((ULONG)width * height * cb, MEMF_ANY | MEMF_CLEAR) : 0;
        if (width && height && !img) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            depth = k; /* the level has the layers it got */
            break;
        }
        if (img && pixels)
            convert(img, (ULONG)width * cb, src + (ULONG)k * in, format, type, width, height, row);
        put_level(&t->f[k], level, img, width, height, format, type, pixels != 0, keep);
    }
    for (k = depth; k < t->nf; k++)
        drop_level(&t->f[k], level);
    t->d[level] = (UWORD)depth;
    if (!ctx->v3d)
        return;
    /* As set_level: rebuilt at the first use unless the copy has the level
     * and the level stays as the chain wants it. */
    if (!t->gpu || level == 0 || level >= t->gpu->num_levels || !depth_ok(t, level) ||
        !consistent(&t->f[0], level)) {
        free_gpu(t);
    } else {
        for (k = 0; k < faces_at(t, level); k++)
            if (t->f[k].shadow[level])
                upload_level(t, k, level);
    }
}

void ogles2_glTexSubImage3D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset, GLint yoffset,
                            GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                            const void *pixels)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    GLenum err;
    UBYTE *sub;
    ULONG cb, row, in;
    const UBYTE *src;
    int y, k;
    BOOL stored;
    if (!ogles2_es3_entry(ctx) || !(ts = ts_get(ctx)) || !(t = image_texture_3d(ctx, ts, target)))
        return;
    type = half_type(ctx, type);
    if (!pixel_size(format, type, &err)) {
        ogles2_error(ctx, err);
        return;
    }
    f = &t->f[0];
    if (level < 0 || level >= MAX_LEVELS || !(f->defined & (1UL << level)) || !t->d[level]) {
        ogles2_error(ctx, level < 0 || level >= MAX_LEVELS ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return;
    }
    if (format != f->format[level] || class_of(format, type) != level_class(f, level)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0 ||
        xoffset + width > f->w[level] || yoffset + height > f->h[level] || zoffset + depth > t->d[level]) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!unpack_source(ctx, &pixels, unpack_bytes(ctx, format, type, width, height, depth)))
        return;
    if (!width || !height || !depth || !pixels)
        return;
    before_change(ctx, ts, t);
    cb = copy_bytes(level_class(f, level));
    src = unpack_image(ctx, pixels, format, type, width, height, &row, &in);
    if (!(sub = AllocVec((ULONG)width * height * cb, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    stored = t->gpu && level < t->gpu->num_levels && consistent(f, level) && depth_ok(t, level);
    for (k = zoffset; k < zoffset + depth; k++) {
        struct OGLES2Face *fk = &t->f[k];
        convert(sub, (ULONG)width * cb, src + (ULONG)(k - zoffset) * in, format, type, width, height, row);
        for (y = 0; y < height; y++)
            memcpy(fk->shadow[level] + ((ULONG)(yoffset + y) * fk->w[level] + xoffset) * cb,
                   sub + (ULONG)y * width * cb, (ULONG)width * cb);
        fk->blank &= ~(1UL << level);
        if (stored)
            store_box(t, k, level, sub, xoffset, yoffset, width, height);
    }
    FreeVec(sub);
    if (!stored && !t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* ---- immutable storage (ES 3.0 3.8.4; stage B item 8) ----
 * glTexStorage2D and 3D for the sized formats the library stores: each
 * level made, blank, as glTexImage* without pixels makes it, in the format
 * and type a level of that sized format is kept in. */

static BOOL sized_storage(GLenum ifmt, GLenum *format, GLenum *type)
{
    if (int_sized(ifmt, format, type))
        return TRUE;
    switch (ifmt) {
    case GL_RGBA8:             *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; return TRUE;
    case GL_RGB8:              *format = GL_RGB; *type = GL_UNSIGNED_BYTE; return TRUE;
    case GL_RGBA4:             *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_4_4_4_4; return TRUE;
    case GL_RGB5_A1:           *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_5_5_5_1; return TRUE;
    case GL_RGB565:            *format = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; return TRUE;
    case GL_RGBA16F:           *format = GL_RGBA; *type = GL_HALF_FLOAT_OES; return TRUE;
    case GL_RGB16F:            *format = GL_RGB; *type = GL_HALF_FLOAT_OES; return TRUE;
    case GL_RGBA32F:           *format = GL_RGBA; *type = GL_FLOAT; return TRUE;
    case GL_RGB32F:            *format = GL_RGB; *type = GL_FLOAT; return TRUE;
    case GL_R8:                *format = GL_RED; *type = GL_UNSIGNED_BYTE; return TRUE;
    case GL_RG8:               *format = GL_RG; *type = GL_UNSIGNED_BYTE; return TRUE;
    case GL_R16F:              *format = GL_RED; *type = GL_HALF_FLOAT_OES; return TRUE;
    case GL_RG16F:             *format = GL_RG; *type = GL_HALF_FLOAT_OES; return TRUE;
    case GL_R32F:              *format = GL_RED; *type = GL_FLOAT; return TRUE;
    case GL_RG32F:             *format = GL_RG; *type = GL_FLOAT; return TRUE;
    case GL_R8_SNORM:          *format = GL_RED; *type = GL_BYTE; return TRUE;
    case GL_RG8_SNORM:         *format = GL_RG; *type = GL_BYTE; return TRUE;
    case GL_RGB8_SNORM:        *format = GL_RGB; *type = GL_BYTE; return TRUE;
    case GL_RGBA8_SNORM:       *format = GL_RGBA; *type = GL_BYTE; return TRUE;
    case GL_SRGB8:             *format = GL_RGB; *type = GL_UNSIGNED_BYTE; return TRUE; /* keep_sized: sRGB */
    case GL_SRGB8_ALPHA8:      *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; return TRUE;
    case GL_RGB10_A2:          *format = GL_RGBA; *type = GL_UNSIGNED_INT_2_10_10_10_REV; return TRUE;
    case GL_R11F_G11F_B10F:    *format = GL_RGB; *type = GL_FLOAT; return TRUE;
    case GL_RGB9_E5:           *format = GL_RGB; *type = GL_FLOAT; return TRUE;
    case GL_DEPTH_COMPONENT32F: *format = GL_DEPTH_COMPONENT; *type = GL_FLOAT; return TRUE;
    case GL_DEPTH32F_STENCIL8: *format = GL_DEPTH_STENCIL_OES; *type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; return TRUE;
    case GL_DEPTH_COMPONENT16: *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; return TRUE;
    case GL_DEPTH_COMPONENT24: *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; return TRUE;
    case GL_DEPTH24_STENCIL8:  *format = GL_DEPTH_STENCIL_OES; *type = GL_UNSIGNED_INT_24_8_OES; return TRUE;
    default:                   return FALSE;
    }
}

/* ES 3.0's sized formats the library does not store yet (the rest of
 * item 8): GL_INVALID_ENUM would say they are none, so they are refused as
 * unsupported values the program can test for. */
static BOOL es3_sized(GLenum ifmt)
{
    return (ifmt >= GL_R8 && ifmt <= GL_RG32UI) || (ifmt >= GL_RGBA32UI && ifmt <= GL_RGB8I) ||
           ifmt == GL_R8_SNORM || ifmt == GL_RG8_SNORM || ifmt == GL_RGB8_SNORM || ifmt == GL_RGBA8_SNORM ||
           ifmt == GL_SRGB8 || ifmt == GL_SRGB8_ALPHA8 || ifmt == GL_RGB10_A2 || ifmt == GL_RGB10_A2UI ||
           ifmt == GL_R11F_G11F_B10F || ifmt == GL_RGB9_E5 || ifmt == GL_DEPTH_COMPONENT32F ||
           ifmt == GL_DEPTH32F_STENCIL8;
}

static void tex_storage(struct OGLES2Context *ctx, GLenum target, GLsizei levels, GLenum ifmt, GLsizei width,
                        GLsizei height, GLsizei depth)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    GLenum format, type;
    BOOL layers = target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY;
    int l, k, max, n = 1, big;
    if (!ogles2_es3_entry(ctx) || !(ts = ts_get(ctx)))
        return;
    if (!(t = bound(ctx, ts, target)) || target == GL_TEXTURE_RECTANGLE) {
        if (target == GL_TEXTURE_RECTANGLE)
            ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!sized_storage(ifmt, &format, &type)) {
        ogles2_error(ctx, es3_sized(ifmt) ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        return;
    }
    max = target == GL_TEXTURE_2D_ARRAY ? 2048 : MAX_SIZE;
    if (levels < 1 || width < 1 || height < 1 || depth < 1 || width > MAX_SIZE || height > MAX_SIZE ||
        depth > max) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    big = width > height ? width : height;
    if (target == GL_TEXTURE_3D && depth > big)
        big = depth;
    while (big > 1) {
        big >>= 1;
        n++;
    }
    /* more levels than the chain, the default texture, already immutable,
     * a cube map's square, depth in 3D textures */
    if (levels > n || !t->name || t->immutable_levels ||
        ((format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL_OES) && target == GL_TEXTURE_3D)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (target == GL_TEXTURE_CUBE_MAP && width != height) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    before_change(ctx, ts, t);
    free_levels(t);
    if (layers && !faces_room(t, depth)) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (l = 0; l < levels; l++) {
        int w = width >> l ? width >> l : 1, h = height >> l ? height >> l : 1;
        int d = target == GL_TEXTURE_3D ? (depth >> l ? depth >> l : 1) : layers ? depth : 1;
        int nf = target == GL_TEXTURE_CUBE_MAP ? 6 : d;
        ULONG bytes = (ULONG)w * h * copy_bytes(class_of(format, type));
        for (k = 0; k < nf; k++) {
            UBYTE *img = AllocVec(bytes, MEMF_ANY | MEMF_CLEAR);
            if (!img) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            put_level(&t->f[k], l, img, w, h, format, type, FALSE, keep_sized(ifmt));
        }
        if (layers)
            t->d[l] = (UWORD)d;
    }
    if (class_of(format, type) != CLASS_RGBA8)
        ogles2_wide_seen |= wide_class(format, type);
    t->immutable_levels = (UBYTE)levels;
    t->es3_rules = TRUE;
    t->pver++; /* the levels the records name */
}

void ogles2_glTexStorage2D(struct OGLES2Context *ctx, GLenum target, GLsizei levels, GLenum internalformat,
                           GLsizei width, GLsizei height)
{
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) {
        if (ogles2_es3_entry(ctx))
            ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    tex_storage(ctx, target, levels, internalformat, width, height, 1);
}

void ogles2_glTexStorage3D(struct OGLES2Context *ctx, GLenum target, GLsizei levels, GLenum internalformat,
                           GLsizei width, GLsizei height, GLsizei depth)
{
    if (target != GL_TEXTURE_3D && target != GL_TEXTURE_2D_ARRAY) {
        if (ogles2_es3_entry(ctx))
            ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    tex_storage(ctx, target, levels, internalformat, width, height, depth);
}

/* ES 3.0's compressed 2D arrays (3.8.6): ETC2/EAC (and S3TC) layers, each
 * kept as its blocks; a 3D texture is GL_INVALID_OPERATION. */
void ogles2_glCompressedTexImage3D(struct OGLES2Context *ctx, GLenum target, GLint level, GLenum internalformat,
                                   GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize,
                                   const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    ULONG bs, layer;
    int k;
    if (!ogles2_es3_entry(ctx) || !(ts = ts_get(ctx)) || !(t = image_texture_3d(ctx, ts, target)))
        return;
    if (!(bs = dxt_block_bytes(internalformat)) || internalformat == GL_ETC1_RGB8_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (target == GL_TEXTURE_3D) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    layer = blocks4((ULONG)width) * blocks4((ULONG)height) * bs;
    if (width < 0 || height < 0 || depth < 0 || border || level < 0 || level >= MAX_LEVELS ||
        width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) || depth > MAX_LAYERS ||
        imageSize != (GLsizei)(layer * (ULONG)depth)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (t->immutable_levels) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!unpack_source(ctx, &data, (ULONG)imageSize))
        return;
    before_change(ctx, ts, t);
    t->es3_rules = TRUE;
    if (!faces_room(t, depth)) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < depth; k++) {
        UBYTE *img = layer ? AllocVec(layer, MEMF_ANY | MEMF_CLEAR) : 0;
        if (layer && !img) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            depth = k;
            break;
        }
        if (img && data)
            CopyMem((UBYTE *)data + (ULONG)k * layer, img, layer);
        put_level(&t->f[k], level, img, width, height, internalformat, 0, data != 0, 0);
    }
    for (k = depth; k < t->nf; k++)
        drop_level(&t->f[k], level);
    t->d[level] = (UWORD)depth;
    free_gpu(t); /* made again at the first use */
}

void ogles2_glCompressedTexSubImage3D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                                      GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                      GLenum format, GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    ULONG bs, bw, bh, row, j, layer;
    int k;
    BOOL stored;
    if (!ogles2_es3_entry(ctx) || !(ts = ts_get(ctx)) || !(t = image_texture_3d(ctx, ts, target)))
        return;
    if (!(bs = dxt_block_bytes(format)) || format == GL_ETC1_RGB8_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (target == GL_TEXTURE_3D) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (level < 0 || level >= MAX_LEVELS || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 ||
        depth < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    f = &t->f[0];
    if (!(f->defined & (1UL << level))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    layer = bw * bh * bs;
    if (xoffset + width > f->w[level] || yoffset + height > f->h[level] || zoffset + depth > t->d[level] ||
        imageSize != (GLsizei)(layer * (ULONG)depth)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (format != f->format[level] || ((xoffset | yoffset) & 3) ||
        ((width & 3) && xoffset + width != f->w[level]) || ((height & 3) && yoffset + height != f->h[level])) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!unpack_source(ctx, &data, (ULONG)imageSize))
        return;
    if (!width || !height || !depth || !data)
        return;
    before_change(ctx, ts, t);
    row = blocks4(f->w[level]) * bs;
    stored = t->gpu && level < t->gpu->num_levels && consistent(f, level) && depth_ok(t, level);
    for (k = zoffset; k < zoffset + depth; k++) {
        struct OGLES2Face *fk = &t->f[k];
        const UBYTE *src = (const UBYTE *)data + (ULONG)(k - zoffset) * layer;
        for (j = 0; j < bh; j++)
            CopyMem((APTR)(src + j * bw * bs),
                    fk->shadow[level] + ((ULONG)yoffset / 4 + j) * row + (ULONG)xoffset / 4 * bs, bw * bs);
        fk->blank &= ~(1UL << level);
        if (stored)
            store_box(t, k, level, src, xoffset, yoffset, width, height);
    }
    if (!stored && !t->gpu && ctx->v3d && !upload_all(t))
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* ---- copies from the framebuffer (glCopyTexImage2D, glCopyTexSubImage2D) ----
 * The region is read as glReadPixels reads it (the open job rendered first,
 * rows bottom-up, which is a texture's row order too), then given to
 * glTexImage2D/glTexSubImage2D, so targets, levels and errors are theirs.
 * The default framebuffer is RGBA8, from which every GLES2 format may be
 * copied (GLES 2.0 3.8.2, table 3.15). */

static int base_components(GLenum format)
{
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: return 1;
    case GL_LUMINANCE_ALPHA:          return 2;
    case GL_RGB:                      return 3;
    case GL_RGBA:                     return 4;
    default:                          return 0;
    }
}

/* width x height pixels from (x, y) as `format` bytes, tightly packed
 * (unpack alignment 1): LUMINANCE takes red, ALPHA alpha, RGB drops alpha
 * (table 3.15). Pixels outside the framebuffer read as 0. */
static UBYTE *read_region(struct OGLES2Context *ctx, GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format)
{
    ULONG n = (ULONG)width * height, i;
    int c = base_components(format);
    UBYTE *rgba = AllocVec(n * 4 + 1, MEMF_ANY | MEMF_CLEAR), *out;
    struct OGLES2PixelStore pack = ctx->pack;
    if (!rgba)
        return 0;
    memset(&ctx->pack, 0, sizeof(ctx->pack));
    ctx->pack.alignment = 1;
    ctx->pack.own = TRUE;
    ogles2_glReadPixels(ctx, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    ctx->pack = pack;
    out = rgba; /* converted in place: each pixel only shrinks */
    for (i = 0; i < n; i++) {
        const UBYTE *s = rgba + i * 4;
        UBYTE *d = out + i * c;
        UBYTE r = s[0], g = s[1], b = s[2], a = s[3];
        switch (format) {
        case GL_ALPHA:           d[0] = a; break;
        case GL_LUMINANCE:       d[0] = r; break;
        case GL_LUMINANCE_ALPHA: d[0] = r; d[1] = a; break;
        case GL_RGB:             d[0] = r; d[1] = g; d[2] = b; break;
        default:                 d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
        }
    }
    return out;
}

void ogles2_glCopyTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                             GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height,
                             GLint border)
{
    UBYTE *img;
    struct OGLES2PixelStore unpack;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (sized_color_base(internalformat)) /* OES_required_internalformat */
        internalformat = sized_color_base(internalformat);
    if (!base_components(internalformat)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0 || border != 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    /* RGB and RGBA on the GPU (copy.c): the level made empty, as
     * glTexImage2D makes it (its errors are glTexImage2D's), then the
     * region drawn into it. */
    if ((internalformat == GL_RGB || internalformat == GL_RGBA) && width > 0 && height > 0 &&
        ctx->error == GL_NO_ERROR && ctx->v3d && !(ctx->probe & OGLES2_PROBE_CPU_COPY)) {
        struct OGLES2TexState *ts;
        struct OGLES2Texture *t;
        int k;
        ogles2_glTexImage2D(ctx, target, level, (GLint)internalformat, width, height, 0, internalformat,
                            GL_UNSIGNED_BYTE, 0);
        if (ctx->error != GL_NO_ERROR)
            return;
        if ((ts = ts_get(ctx)) && (t = image_texture(ctx, ts, target, level, &k)) &&
            ogles2_copy_gpu(ctx, t, k, level, 0, 0, x, y, width, height))
            return;
    }
    if (!(img = read_region(ctx, x, y, width, height, internalformat))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    unpack_tight(ctx, &unpack);
    ogles2_glTexImage2D(ctx, target, level, (GLint)internalformat, width, height, 0, internalformat,
                        GL_UNSIGNED_BYTE, img);
    ctx->unpack = unpack;
    FreeVec(img);
}

void ogles2_glCopyTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    UBYTE *img;
    struct OGLES2PixelStore unpack;
    GLenum fmt;
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    /* In the level's own format, so the conversion is table 3.15's (a
     * LUMINANCE texture takes red); glTexSubImage2D reports a level that
     * does not exist. */
    {
        struct OGLES2TexState *ts = ts_get(ctx);
        struct OGLES2Texture *t;
        const struct OGLES2Face *f;
        int k;
        if (!ts || !(t = image_texture(ctx, ts, target, level, &k)))
            return;
        f = &t->f[k];
        fmt = level >= 0 && level < MAX_LEVELS ? f->format[level] : 0;
        if (!base_components(fmt))
            fmt = GL_RGBA; /* an undefined level: glTexSubImage2D refuses it */
        /* On the GPU (copy.c) when glTexSubImage2D would take the copy: a
         * level of its own, the region inside it, a format a render target
         * holds and RGBA bytes fill (not half-float: glTexSubImage2D takes
         * no bytes into a float level). */
        if (width > 0 && height > 0 && level >= 0 && level < MAX_LEVELS && (f->defined & (1UL << level)) &&
            xoffset >= 0 && yoffset >= 0 && xoffset + width <= f->w[level] && yoffset + height <= f->h[level] &&
            level_class(f, level) == CLASS_RGBA8 &&
            ogles2_copy_gpu(ctx, t, k, level, xoffset, yoffset, x, y, width, height))
            return;
    }
    if (!width || !height) { /* nothing to copy; the level and offsets are still checked */
        ogles2_glTexSubImage2D(ctx, target, level, xoffset, yoffset, width, height, fmt,
                               GL_UNSIGNED_BYTE, 0);
        return;
    }
    if (!(img = read_region(ctx, x, y, width, height, fmt))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    unpack_tight(ctx, &unpack);
    ogles2_glTexSubImage2D(ctx, target, level, xoffset, yoffset, width, height, fmt,
                           GL_UNSIGNED_BYTE, img);
    ctx->unpack = unpack;
    FreeVec(img);
}

/* ---- compressed textures ----
 * OES_compressed_ETC1_RGB8_texture, S3TC (DXT1, DXT3, DXT5; M7 step 3) and
 * ES 3.0's ETC2/EAC (stage B item 9): kept as given, in RAM and on the GPU,
 * which samples BC1-BC3 and ETC2/EAC itself (ETC1 as RGB8_ETC2, of which
 * it is a subset); no TexSubImage may change an ETC1 level.
 * glCompressedTexSubImage2D (and 3D, for arrays) replaces whole
 * blocks; glTexSubImage2D, glCopyTexSubImage2D (another format: refused by
 * glTexSubImage2D) and glGenerateMipmap give GL_INVALID_OPERATION, and a
 * framebuffer object cannot draw into one (not colour-renderable). */

/* Level `level` of an S3TC texture: `bytes` of blocks (data 0: zeros). */
static void dxt_image(struct OGLES2Context *ctx, struct OGLES2TexState *ts, GLenum target, GLint level,
                      GLenum format, GLsizei width, GLsizei height, ULONG bytes, const void *data)
{
    struct OGLES2Texture *t;
    UBYTE *img;
    int k;
    if (width > (MAX_SIZE >> level) || height > (MAX_SIZE >> level) || (face_target(target) && width != height)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    if (t->immutable_levels) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    before_change(ctx, ts, t);
    img = bytes ? AllocVec(bytes, MEMF_ANY | MEMF_CLEAR) : 0;
    if (bytes && !img) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (img && data)
        CopyMem((APTR)data, img, bytes);
    set_level(ctx, t, k, level, img, width, height, format, 0, data != 0, 0);
}

void ogles2_glCompressedTexImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                   GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                   GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    ULONG bw, bh, bs;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target != GL_TEXTURE_2D && !face_target(target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bs = dxt_block_bytes(internalformat);
    if (!bs || (etc2_format(internalformat) && !ogles2_es3(ctx))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    if (width < 0 || height < 0 || border != 0 || level < 0 || level >= MAX_LEVELS ||
        imageSize != (GLsizei)(bw * bh * bs)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!unpack_source(ctx, &data, (ULONG)imageSize))
        return;
    /* S3TC, ETC1 and ETC2/EAC are kept and sampled as their blocks */
    dxt_image(ctx, ts, target, level, internalformat, width, height, (ULONG)imageSize, data);
}

void ogles2_glCompressedTexSubImage2D(struct OGLES2Context *ctx, GLenum target, GLint level,
                                      GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                                      GLenum format, GLsizei imageSize, const void *data)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    struct OGLES2Face *f;
    ULONG bs, bw, bh, row, j;
    int k;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    bs = dxt_block_bytes(format);
    if ((target != GL_TEXTURE_2D && !face_target(target)) || !bs || (etc2_format(format) && !ogles2_es3(ctx))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* The order of the checks is dEQP's (negative_api.texture): values
     * first, then the level, its bounds and size; the format and the block
     * bounds last. */
    if (level < 0 || level >= MAX_LEVELS || xoffset < 0 || yoffset < 0 || width < 0 || height < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(t = image_texture(ctx, ts, target, level, &k)))
        return;
    f = &t->f[k];
    if (!(f->defined & (1UL << level))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    bw = blocks4((ULONG)width);
    bh = blocks4((ULONG)height);
    if (xoffset + width > f->w[level] || yoffset + height > f->h[level] ||
        imageSize != (GLsizei)(bw * bh * (bs ? bs : 8))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* OES_compressed_ETC1_RGB8_texture: ETC1 images cannot be updated; S3TC
     * (EXT_texture_compression_s3tc) and ETC2/EAC (ES 3.0): the level's own
     * format, whole blocks (a box may end at the level's edge). */
    if (format == GL_ETC1_RGB8_OES || format != f->format[level] || ((xoffset | yoffset) & 3) ||
        ((width & 3) && xoffset + width != f->w[level]) || ((height & 3) && yoffset + height != f->h[level])) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!unpack_source(ctx, &data, (ULONG)imageSize))
        return;
    if (!width || !height || !data)
        return;
    before_change(ctx, ts, t);
    row = blocks4(f->w[level]) * bs;
    for (j = 0; j < bh; j++)
        CopyMem((UBYTE *)data + j * bw * bs, f->shadow[level] + ((ULONG)yoffset / 4 + j) * row + (ULONG)xoffset / 4 * bs,
                bw * bs);
    f->blank &= ~(1UL << level);
    if (t->gpu && level < t->gpu->num_levels && consistent(f, level))
        store_box(t, k, level, (const UBYTE *)data, xoffset, yoffset, width, height);
    else if (!t->gpu && ctx->v3d && !upload_all(t)) /* renamed: a new copy, whole */
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* Level l+1 from level l: each texel the mean of a 2x2 box (a 1-texel edge
 * reuses its row or column). */
static void box_down(const UBYTE *src, int sw, int sh, UBYTE *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++) {
                ULONG sum = src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                            src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c];
                dst[(y * dw + x) * 4 + c] = (UBYTE)((sum + 2) / 4);
            }
        }
}

/* The same for an RGBA float copy. */
static void box_down_float(const GLfloat *src, int sw, int sh, GLfloat *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, y0 = y * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for (c = 0; c < 4; c++)
                dst[(y * dw + x) * 4 + c] = (src[(y0 * sw + x0) * 4 + c] + src[(y0 * sw + x1) * 4 + c] +
                                             src[(y1 * sw + x0) * 4 + c] + src[(y1 * sw + x1) * 4 + c]) * 0.25f;
        }
}

/* glGenerateMipmap on the GPU (plan 7.6's "later"): the TFU filters levels
 * 1 .. n-1 from level 0 where it is, in the texture's GPU memory. The CPU
 * path below filters the RAM copies and uploads the whole chain, after a
 * read-back when the GPU drew level 0: glmark2's refract, a 1600x1200
 * render target every frame, spent 500 ms a frame there. Only for a 2D
 * texture whose GPU copy already has the chain, laid out by an earlier
 * glGenerateMipmap or the levels given (the first call takes the CPU path),
 * in a format the TFU filters. The levels it writes are newer on the GPU
 * than their RAM copies, as a render target's are (read back when the CPU
 * needs them, sync). Probe M keeps the CPU path. FALSE: the CPU path runs. */
static BOOL generate_tfu(struct OGLES2Context *ctx, struct OGLES2TexState *ts, struct OGLES2Texture *t, int n)
{
    struct OGLES2Face *f = &t->f[0];
    struct OGLES2Context *m, *user;
    ULONG made = ((1UL << n) - 1) & ~1UL; /* levels 1 .. n-1 */
    int l, r;
    if ((ctx->probe & OGLES2_PROBE_CPU_MIPMAP) || !ctx->v3d || faces(t) != 1 || layered(t) || n < 2 || !t->gpu ||
        !t->gpu->texture_mem.hostptr || t->gpu->num_levels < n || !v3d_tfu_supports_mipmaps(t->hw_type))
        return FALSE;
    for (l = 1; l < n; l++) {
        int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
        if (!(f->defined & (1UL << l)) || !f->shadow[l] || f->w[l] != w || f->h[l] != h ||
            f->format[l] != f->format[0] || f->type[l] != f->type[0])
            return FALSE;
    }
    /* The job that draws level 0 rendered first (any member's)... */
    for (m = ctx->share->first; m; m = m->share_next)
        ogles2_fbo_texture_changing(m, t);
    /* ...and one that samples the texture and has not rendered yet: it must
     * see the old levels, which the TFU writes over in place (renaming, as
     * before_change does, would need the levels in RAM: the read-back this
     * path is for). Usually rendered already: the next job's start waits
     * for the one before. */
    if (t->used_serial && (user = ogles2_job_user(ctx, t->used_serial)) != 0 && user->v3d) {
        ogles2_job_flush(user, FALSE);
        ogles2_job_finish(user);
    }
    CacheClearE(0, ~0UL, CACRF_ClearD); /* the CPU's writes to level 0 in memory */
    if (!v3d_gpu_lock(ogles2_device_gpu_lock(), TFU_LOCK_TIMEOUT_MS))
        return FALSE;
    r = v3d_tfu_generate_mipmaps(ogles2_device(), t->gpu, t->hw_type, (v3d_u8)(n - 1));
    v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (r) {
        if (r == -2)
            ogles2_log("glGenerateMipmap: the TFU did not finish (texture %ld, %ldx%ld): "
                       "the CPU makes the levels", (ULONG)t->name, (ULONG)f->w[0], (ULONG)f->h[0]);
        return FALSE;
    }
    f->rendered |= made;
    f->blank &= ~made;
    if (!ts->logged_tfu) {
        ts->logged_tfu = TRUE;
        ogles2_log("glGenerateMipmap: the TFU (texture %ld, %ldx%ld, %ld levels; logged once)",
                   (ULONG)t->name, (ULONG)f->w[0], (ULONG)f->h[0], (ULONG)n);
    }
    return TRUE;
}

/* A 3D texture's levels 1 to n - 1 from level 0 on the CPU: each slice a
 * box filter of two (the last one of an odd depth alone, as the 2D box
 * filter takes a row). */
static BOOL generate_volume(struct OGLES2Context *ctx, struct OGLES2Texture *t, int n, int cls)
{
    int l, z;
    ULONG i, cb = copy_bytes(cls);
    for (l = 1; l < n; l++) {
        int w = t->f[0].w[l - 1] > 1 ? t->f[0].w[l - 1] / 2 : 1, h = t->f[0].h[l - 1] > 1 ? t->f[0].h[l - 1] / 2 : 1;
        int d = t->d[l - 1] > 1 ? t->d[l - 1] / 2 : 1;
        for (z = 0; z < d; z++) {
            int z1 = 2 * z + 1 < t->d[l - 1] ? 2 * z + 1 : 2 * z;
            struct OGLES2Face *a = &t->f[2 * z], *b = &t->f[z1];
            UBYTE *img = AllocVec((ULONG)w * h * cb, MEMF_ANY), *two = AllocVec((ULONG)w * h * cb, MEMF_ANY);
            if (!img || !two) {
                if (img)
                    FreeVec(img);
                if (two)
                    FreeVec(two);
                return FALSE;
            }
            if (cls == CLASS_FLOAT) {
                box_down_float((const GLfloat *)a->shadow[l - 1], a->w[l - 1], a->h[l - 1], (GLfloat *)img, w, h);
                box_down_float((const GLfloat *)b->shadow[l - 1], b->w[l - 1], b->h[l - 1], (GLfloat *)two, w, h);
                for (i = 0; i < (ULONG)w * h * 4; i++)
                    ((GLfloat *)img)[i] = (((GLfloat *)img)[i] + ((GLfloat *)two)[i]) * 0.5f;
            } else {
                box_down(a->shadow[l - 1], a->w[l - 1], a->h[l - 1], img, w, h);
                box_down(b->shadow[l - 1], b->w[l - 1], b->h[l - 1], two, w, h);
                for (i = 0; i < (ULONG)w * h * 4; i++)
                    img[i] = (UBYTE)((img[i] + two[i] + 1) >> 1);
            }
            FreeVec(two);
            put_level(&t->f[z], l, img, w, h, a->format[l - 1], a->type[l - 1], TRUE, a->sized[l - 1]);
        }
        for (z = d; z < t->nf; z++)
            drop_level(&t->f[z], l);
        t->d[l] = (UWORD)d;
    }
    (void)ctx;
    return TRUE;
}

void ogles2_glGenerateMipmap(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2TexState *ts;
    struct OGLES2Texture *t;
    int l, n, k, cls;
    if (!ogles2_context_valid(ctx) || !(ts = ts_get(ctx)))
        return;
    if (target == GL_TEXTURE_RECTANGLE) { /* ARB_texture_rectangle: no mip levels */
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(t = bound(ctx, ts, target)))
        return;
    /* A compressed level 0 (ETC1, S3TC), of any size: GLES 2.0 3.7.11; ES
     * 3.0's integer ones */
    if ((t->f[0].defined & 1) && (t->f[0].format[0] == GL_ETC1_RGB8_OES || dxt_block_bytes(t->f[0].format[0]) ||
                                  int_components(t->f[0].format[0]))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!base_ok(t)) {
        /* GLES 2.0 3.7.11: a cube map that is not cube complete. */
        if (target == GL_TEXTURE_CUBE_MAP)
            ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* Depth textures: no mip chain to filter (GLES 3.0's rule for formats
     * that are not filterable; OES_depth_texture says nothing). */
    cls = level_class(&t->f[0], 0);
    if (cls == CLASS_DEPTH16 || cls == CLASS_DEPTH24) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    n = chain_levels(t);
    if (generate_tfu(ctx, ts, t, n))
        return;
    before_change(ctx, ts, t);
    if (t->target == GL_TEXTURE_3D) {
        if (!generate_volume(ctx, t, n, cls))
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
        else if (ctx->v3d && !upload_all(t))
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (l = 1; l < n; l++)
        t->d[l] = t->d[0]; /* an array's layers: every level has them all */
    for (k = 0; k < faces(t); k++) {
        struct OGLES2Face *f = &t->f[k];
        for (l = 1; l < n; l++) {
            int w = f->w[l - 1] > 1 ? f->w[l - 1] / 2 : 1, h = f->h[l - 1] > 1 ? f->h[l - 1] / 2 : 1;
            UBYTE *img = AllocVec((ULONG)w * h * copy_bytes(cls), MEMF_ANY);
            if (!img) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            if (cls == CLASS_FLOAT)
                box_down_float((const GLfloat *)f->shadow[l - 1], f->w[l - 1], f->h[l - 1], (GLfloat *)img, w, h);
            else
                box_down(f->shadow[l - 1], f->w[l - 1], f->h[l - 1], img, w, h);
            if (f->shadow[l])
                FreeVec(f->shadow[l]);
            f->shadow[l] = img;
            f->w[l] = (UWORD)w;
            f->h[l] = (UWORD)h;
            f->format[l] = f->format[l - 1];
            f->type[l] = f->type[l - 1];
            f->sized[l] = f->sized[l - 1];
            f->defined |= 1UL << l;
            f->blank &= ~(1UL << l);
        }
    }
    if (ctx->v3d && !upload_all(t))
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* ---- for the draw path ---- */

/* textureSize's recipe values (Mesa's get_texture_size): the base level's
 * width or height (level 0's minified to it), the depth and the layers
 * (1: no 3D or array textures before stage B). A kind without textures
 * answers 1, as the fallback it samples. */
ULONG ogles2_texture_size(struct OGLES2Context *ctx, int unit, int kind, int what)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *t;
    ULONG s;
    if (!ts || kind == OGLES2_TEX_NONE)
        return 1;
    t = unit_texture(ts, kind, unit); /* the integer flag aside */
    /* the depth of a 3D texture (minified as width and height), the layers of an array */
    if (what >= 2 && !(what == 2 ? t->target == GL_TEXTURE_3D : t->target == GL_TEXTURE_2D_ARRAY))
        return 1;
    s = what == 3 ? t->d[0] : what == 2 ? t->d[0] : what ? t->f[0].h[0] : t->f[0].w[0];
    if (!s || what == 3)
        return s;
    s = t->base_level > 0 && t->base_level < 32 ? s >> t->base_level : s;
    return s ? s : 1;
}

/* ---- sampler objects (ES 3.0 3.8.2; stage B item 7) ---- */

/* The sampler named `name`; their names come from glGenSamplers only. */
static struct OGLES2Sampler *smp_by_name(struct OGLES2Share *sh, GLuint name)
{
    return name && name < sh->nsmp ? sh->smp[name] : 0;
}

void ogles2_glGenSamplers(struct OGLES2Context *ctx, GLsizei count, GLuint *samplers)
{
    struct OGLES2Share *sh;
    GLsizei k;
    GLuint i = 1, nn, j;
    if (!ogles2_es3_entry(ctx))
        return;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    sh = ctx->share;
    for (k = 0; k < count; k++) {
        struct OGLES2Sampler *so, **tab;
        while (i < sh->nsmp && sh->smp[i])
            i++;
        if (i >= sh->nsmp) { /* the table grows */
            nn = sh->nsmp ? sh->nsmp * 2 : 32;
            if (i >= OGLES2_NEAR_NAMES || !(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR))) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            for (j = 0; j < sh->nsmp; j++)
                tab[j] = sh->smp[j];
            if (sh->smp)
                FreeVec(sh->smp);
            sh->smp = tab;
            sh->nsmp = nn;
        }
        if (!(so = AllocVec(sizeof(*so), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        so->name = i;
        smp_defaults(&so->s);
        so->ver = ++smp_serial;
        sh->smp[i] = so;
        if (samplers)
            samplers[k] = i;
    }
}

/* ES 3.0 says a sampler deleted while another context has it bound lives
 * on there until unbound; here every member of the share group unbinds it. */
void ogles2_glDeleteSamplers(struct OGLES2Context *ctx, GLsizei count, const GLuint *samplers)
{
    struct OGLES2Context *m;
    GLsizei k;
    int u;
    if (!ogles2_es3_entry(ctx))
        return;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; samplers && k < count; k++) {
        struct OGLES2Sampler *so = smp_by_name(ctx->share, samplers[k]);
        if (!so)
            continue;
        for (m = ctx->share->first; m; m = m->share_next)
            if (m->tex) {
                for (u = 0; u < OGLES2_MAX_TEXTURE_UNITS; u++)
                    if (m->tex->unitsmp[u] == so)
                        m->tex->unitsmp[u] = 0;
                if (m->tex->copy_smp == so)
                    m->tex->copy_smp = 0;
            }
        ctx->share->smp[so->name] = 0;
        FreeVec(so);
    }
}

GLboolean ogles2_glIsSampler(struct OGLES2Context *ctx, GLuint sampler)
{
    if (!ogles2_es3_entry(ctx))
        return GL_FALSE;
    return smp_by_name(ctx->share, sampler) ? GL_TRUE : GL_FALSE;
}

void ogles2_glBindSampler(struct OGLES2Context *ctx, GLuint unit, GLuint sampler)
{
    struct OGLES2TexState *ts;
    struct OGLES2Sampler *so = 0;
    if (!ogles2_es3_entry(ctx))
        return;
    if (unit >= OGLES2_MAX_TEXTURE_UNITS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (sampler && !(so = smp_by_name(ctx->share, sampler))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!(ts = ts_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    ts->unitsmp[unit] = so;
}

static void sampler_param(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, GLint i, GLfloat f)
{
    struct OGLES2Sampler *so;
    GLenum err;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!(so = smp_by_name(ctx->share, sampler))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if ((err = smp_param(ctx, &so->s, pname, i, f)) != GL_NO_ERROR) {
        ogles2_error(ctx, err);
        return;
    }
    so->ver = ++smp_serial;
}

void ogles2_glSamplerParameteri(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, GLint param)
{
    sampler_param(ctx, sampler, pname, param, (GLfloat)param);
}

void ogles2_glSamplerParameterf(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, GLfloat param)
{
    sampler_param(ctx, sampler, pname, round_param(param), param);
}

void ogles2_glSamplerParameteriv(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, const GLint *param)
{
    if (param)
        sampler_param(ctx, sampler, pname, param[0], (GLfloat)param[0]);
}

void ogles2_glSamplerParameterfv(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, const GLfloat *param)
{
    if (param)
        sampler_param(ctx, sampler, pname, round_param(param[0]), param[0]);
}

static BOOL get_sampler_param(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, GLfloat *v)
{
    struct OGLES2Sampler *so;
    if (!ogles2_es3_entry(ctx))
        return FALSE;
    if (!(so = smp_by_name(ctx->share, sampler))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    if (!smp_get(ctx, &so->s, pname, v)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return FALSE;
    }
    return TRUE;
}

void ogles2_glGetSamplerParameteriv(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, GLint *params)
{
    GLfloat v;
    if (params && get_sampler_param(ctx, sampler, pname, &v))
        *params = exact_param(pname) ? (GLint)(ULONG)v : round_param(v);
}

void ogles2_glGetSamplerParameterfv(struct OGLES2Context *ctx, GLuint sampler, GLenum pname, GLfloat *params)
{
    GLfloat v;
    if (params && get_sampler_param(ctx, sampler, pname, &v))
        *params = v;
}

/* The fallback texture, 1x1 (0,0,0,1) (GLES 2.0 3.8.2: an incomplete
 * texture samples that). */
static struct OGLES2Texture *fallback(struct OGLES2TexState *ts)
{
    struct OGLES2Texture *t = &ts->fallback;
    if (!t->gpu) {
        init_object(t, 0, GL_TEXTURE_2D);
        t->s.min_filter = t->s.mag_filter = GL_NEAREST;
        if (!(t->f[0].shadow[0] = AllocVec(4, MEMF_ANY | MEMF_CLEAR)))
            return 0;
        t->f[0].shadow[0][3] = 255;
        t->f[0].w[0] = t->f[0].h[0] = 1;
        t->f[0].format[0] = GL_RGBA;
        t->f[0].type[0] = GL_UNSIGNED_BYTE;
        t->f[0].defined = 1;
        if (!upload_all(t))
            return 0;
    }
    return t;
}

/* The same for an integer sampler: (0, 0, 0, 1) in RGBA8UI. */
static struct OGLES2Texture *fallback_int(struct OGLES2TexState *ts)
{
    struct OGLES2Texture *t = &ts->fallback_int;
    if (!t->gpu) {
        init_object(t, 0, GL_TEXTURE_2D);
        t->s.min_filter = t->s.mag_filter = GL_NEAREST;
        if (!(t->f[0].shadow[0] = AllocVec(16, MEMF_ANY | MEMF_CLEAR)))
            return 0;
        ((ULONG *)t->f[0].shadow[0])[3] = 1;
        t->f[0].w[0] = t->f[0].h[0] = 1;
        t->f[0].format[0] = GL_RGBA_INTEGER;
        t->f[0].type[0] = GL_UNSIGNED_BYTE;
        t->f[0].defined = 1;
        if (!upload_all(t))
            return 0;
    }
    return t;
}

/* Claims `size` bytes of the state buffer at a 32-byte boundary (the
 * TMU_CONFIG words carry configuration in the address's low bits). */
static void *claim32(V3DContext *v, ULONG size)
{
    v3d_u8 s = v->build_slot;
    v3d_static_buffer *sb = &v->state_buf[s];
    sb->used = (sb->used + 31) & ~31;
    return v3d_cl_claim_fast(ogles2_device(), &v->state_mem[s], sb, size, &v->frame);
}

static UBYTE wrap_mode(GLenum w)
{
    return w == GL_CLAMP_TO_EDGE ? V3D_WRAP_MODE_CLAMP
         : w == GL_MIRRORED_REPEAT ? V3D_WRAP_MODE_MIRROR
         : w == GL_MIRROR_CLAMP_TO_EDGE ? V3D_WRAP_MODE_MIRRORONCE : V3D_WRAP_MODE_REPEAT;
}

/* The texture and sampler state for unit `unit`'s texture of `cube`
 * kind, written into the job's state buffer (packing as PiStorm3D's
 * v3d_texture_emit_state; values as Mesa's v3dx_state.c). FALSE: out of
 * memory. */
BOOL ogles2_texture_state(struct OGLES2Context *ctx, int unit, int kind, ULONG *tex_state,
                          ULONG *sampler_state)
{
    struct OGLES2TexState *ts = ts_get(ctx);
    struct OGLES2Texture *t;
    V3DContext *v = ctx->v3d;
    v3d_texture_shader_state *tsr;
    v3d_sampler_state *ss;
    V3DTexture *g;
    char *tiled;
    ULONG *swivel;
    UBYTE *swivel24, temp;
    UWORD *swivel16;
    int first = 0, last = 0;
    BOOL own;
    struct OGLES2Sampler *so;
    const struct OGLES2SamplerParams *sp;
    ULONG sver;
    BOOL isfb;

    if (!ts)
        return FALSE;
    so = ts->unitsmp[unit];
    sver = so ? so->ver : 0;
    /* A sampler of a kind with no textures yet (OGLES2_TEX_NONE: GLSL ES
     * 3.00's integer samplers until stage B) reads what an incomplete
     * texture reads. */
    if (kind == OGLES2_TEX_NONE && !(t = fallback(ts)))
        return FALSE;
    if (kind != OGLES2_TEX_NONE)
        t = unit_texture(ts, kind, unit);
    isfb = FALSE;
    /* The same records as the last draw with it: a texture a draw of a job
     * not yet rendered used gets a new GPU copy (another address) when its
     * images change (before_change); its parameters are compared here.
     * Records in the texture's own block hold across jobs (Quake III: most
     * of a frame's textures are drawn once a frame, 2.5 us a record). */
    if (t->gpu && (t->st_own || (t->st_gen == g_v3d_cl_generation && t->st_ctx == ctx)) &&
        t->st_mem == t->gpu->texture_mem.hostptr && t->st_pver == t->pver && t->st_sver == sver) {
        t->used_serial = ctx->job_serial;
        *tex_state = t->st_tex;
        *sampler_state = t->st_smp;
        return TRUE;
    }
    /* A renamed texture gets its new copy at its first draw at the latest. */
    isfb = t == &ts->fallback || t == &ts->fallback_int;
    if (!t->gpu && !isfb)
        upload_all(t);
    /* The unit's sampler object's parameters, else the texture's (the
     * fallback's own: it reads its one texel whatever they are). */
    sp = so && !isfb ? &so->s : &t->s;
    if (!complete(t, sp->min_filter, sp->mag_filter, &first, &last)) {
        if (!t->warned_incomplete && !isfb) {
            int l, n = mipmapped(sp->min_filter) && t->f[0].w[0] ? chain_levels(t) : 1;
            t->warned_incomplete = TRUE;
            for (l = 1; l < n && consistent(&t->f[0], l); l++)
                ;
            ogles2_log("texture %ld sampled while incomplete (it reads black): %s, level 0 %ldx%ld, "
                       "min filter 0x%04lx, level %ld of %ld missing or inconsistent (logged once a texture)",
                       (ULONG)t->name, (ULONG)(!t->gpu ? "no GPU copy" : base_ok(t) ? "mip chain" : "level 0"),
                       (ULONG)t->f[0].w[0], (ULONG)t->f[0].h[0], (ULONG)sp->min_filter, (ULONG)l, (ULONG)n);
        }
        if (!(t = (kind & OGLES2_TEX_INT) ? fallback_int(ts) : fallback(ts)))
            return FALSE;
        sp = &t->s;
        first = last = 0;
    }
    /* Into the texture's own block, unless a job not yet rendered may read
     * the records it holds (the last job that drew with the texture): then
     * into this job's state buffer, as before. */
    own = !t->used_serial || !ogles2_job_uses(ctx, t->used_serial);
    if (own && !t->rec.hostptr && v3d_mem_alloc(ogles2_device(), &t->rec, 64) < 0)
        own = FALSE;
    t->used_serial = ctx->job_serial;
    g = t->gpu;

    if (own)
        tsr = (v3d_texture_shader_state *)t->rec.hostptr;
    else if (!(tsr = claim32(v, sizeof(*tsr))))
        return FALSE;
    memset(tsr, 0, sizeof(*tsr));
    tiled = (char *)V3D_ALIGN_UP((v3d_uintptr)g->texture_mem.hostptr, 256);
    tsr->texture_base_pointer_rshift_6 = ((ULONG)tiled + g->levels[0].offset) >> 6;
    tsr->flip_texture_y_axis = FALSE;
    tsr->base_level = (ULONG)first; /* SGIS_texture_lod */
    tsr->max_level = (ULONG)last;
    tsr->image_width_lo = g->width & 0x3f;
    tsr->image_width_hi = g->width >> 6;
    tsr->image_height = g->height;
    /* A cube map: six layers, a face stride apart (Mesa's v3dx_state.c); a
     * 2D array its layers so, a 3D texture its slices (the stride level
     * 0's). The fallback is 2D: as a cube, every face reads its one texel. */
    tsr->image_depth_lo = (ULONG)faces(t) & 0x3ff; /* a 2D array's layers, a 3D texture's depth */
    tsr->image_depth_hi = (ULONG)faces(t) >> 10;
    tsr->array_stride_64_byte_aligned = faces(t) > 1 ? t->face_stride / 64 : 0;
    tsr->texture_type = t->hw_type;
    /* ES 3.0's sRGB formats: RGBA8 the TMU decodes (Mesa: util_format_is_srgb) */
    tsr->srgb = t->f[0].sized[0] == GL_SRGB8 || t->f[0].sized[0] == GL_SRGB8_ALPHA8 || srgb_etc2(t->f[0].format[0]);
    /* The GL swizzle composed with the format's (Mesa's
     * util_format_compose_swizzles): component k reads the format's channel
     * the GL swizzle names, or zero or one. */
    {
        UBYTE sw[4];
        int k;
        for (k = 0; k < 4; k++)
            sw[k] = t->gl_swizzle[k] == GL_ZERO ? v3d_SWIZZLE_ZERO
                  : t->gl_swizzle[k] == GL_ONE ? v3d_SWIZZLE_ONE
                  : t->gl_swizzle[k] == GL_GREEN ? t->swizzle[1]
                  : t->gl_swizzle[k] == GL_BLUE ? t->swizzle[2]
                  : t->gl_swizzle[k] == GL_ALPHA ? t->swizzle[3] : t->swizzle[0];
        tsr->swizzle_r = sw[0];
        tsr->swizzle_g = sw[1];
        tsr->swizzle_b = sw[2];
        tsr->swizzle_a = sw[3];
    }
    if (g->tiling_format == V3D_MEMORY_FORMAT_UIF_XOR) {
        tsr->level_0_is_strictly_uif = TRUE;
        tsr->level_0_xor_enable = TRUE;
        tsr->level_0_ubpad = g->ub_pad;
        tsr->extended = TRUE;
    }
    swivel = (ULONG *)tsr;
    swivel[0] = LE32(swivel[0]);
    swivel[1] = LE32(swivel[1]);
    swivel[2] = LE32(swivel[2]);
    swivel[3] = LE32(swivel[3]);
    swivel[4] = LE32(swivel[4]);
    *tex_state = (ULONG)tsr;

    if (own)
        ss = (v3d_sampler_state *)((UBYTE *)t->rec.hostptr + 32);
    else if (!(ss = claim32(v, sizeof(*ss))))
        return FALSE;
    memset(ss, 0, sizeof(*ss));
    ss->mag_filter_nearest = sp->mag_filter == GL_NEAREST;
    ss->min_filter_nearest = sp->min_filter == GL_NEAREST || sp->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                             sp->min_filter == GL_NEAREST_MIPMAP_LINEAR;
    ss->mip_filter_nearest = sp->min_filter != GL_NEAREST_MIPMAP_LINEAR &&
                             sp->min_filter != GL_LINEAR_MIPMAP_LINEAR;
    /* 32-bit floats and depth: the TMU does not filter them, but for a
     * depth comparison (ES 3.0 3.8.13: a shadow sampler filters as set). */
    if (unfilterable_type(t->hw_type) && sp->compare_mode != GL_COMPARE_REF_TO_TEXTURE &&
        !(ss->mag_filter_nearest && ss->min_filter_nearest && ss->mip_filter_nearest)) {
        ss->mag_filter_nearest = ss->min_filter_nearest = ss->mip_filter_nearest = TRUE;
        if (!t->warned_filter) {
            t->warned_filter = TRUE;
            ogles2_log("texture %ld: 32-bit float or depth texels are sampled nearest (no linear filtering "
                       "for them; logged once a texture)", (ULONG)t->name);
        }
    }
    /* GL's comparison functions are V3D's in the same order (Mesa:
     * compare_func when the mode compares, else NEVER). */
    ss->depth_compare_function = sp->compare_mode == GL_COMPARE_REF_TO_TEXTURE ? (UBYTE)(sp->compare_func - GL_NEVER)
                                                                              : V3D_COMPARE_FUNC_NEVER;
    ss->wrap_s = wrap_mode(sp->wrap_s);
    ss->wrap_t = wrap_mode(sp->wrap_t);
    ss->wrap_r = wrap_mode(sp->wrap_r);
    /* The LOD range, Mesa's v3dx_state.c: SGIS_texture_lod's limits within
     * the field's 0 to 15; without mipmapping clamped just above the base
     * level, so the hardware still chooses between the min and mag filters. */
    {
        GLfloat mn = sp->min_lod < 0.0f ? 0.0f : sp->min_lod > 15.0f ? 15.0f : sp->min_lod;
        GLfloat mx = sp->max_lod < mn ? mn : sp->max_lod > 15.0f ? 15.0f : sp->max_lod;
        GLfloat bias = t->lod_bias < -MAX_LOD_BIAS ? -MAX_LOD_BIAS
                     : t->lod_bias > MAX_LOD_BIAS ? MAX_LOD_BIAS : t->lod_bias;
        if (!mipmapped(sp->min_filter)) {
            if (mn > 1.0f / 256.0f) mn = 1.0f / 256.0f;
            if (mx > 1.0f / 256.0f) mx = 1.0f / 256.0f;
        }
        ss->min_level_of_detail = V3D_FLOAT_TO_U4_8(mn);
        ss->max_level_of_detail = V3D_FLOAT_TO_U4_8(mx);
        /* EXT_texture_lod_bias: s8.8 */
        ss->fixed_bias = (UWORD)(WORD)round_param(bias * 256.0f);
    }
    /* EXT_texture_filter_anisotropic, Mesa's levels: 2, 4, 8, 16 */
    if (sp->max_aniso > 1.0f && !unfilterable_type(t->hw_type)) {
        ss->anisotropy_enable = TRUE;
        ss->maximum_anisotropy = sp->max_aniso > 8.0f ? 3 : sp->max_aniso > 4.0f ? 2 : sp->max_aniso > 2.0f ? 1 : 0;
    }
    swivel24 = (UBYTE *)ss;
    temp = swivel24[3];
    swivel24[3] = swivel24[1];
    swivel24[1] = temp;
    swivel16 = (UWORD *)ss;
    swivel16[2] = LE16(swivel16[2]); /* the bias */
    swivel16[3] = LE16(swivel16[3]);
    *sampler_state = (ULONG)ss;
    t->st_gen = g_v3d_cl_generation;
    t->st_ctx = ctx;
    t->st_own = own;
    t->st_mem = g->texture_mem.hostptr;
    t->st_pver = t->pver;
    t->st_sver = sver;
    t->st_tex = *tex_state;
    t->st_smp = *sampler_state;
    return TRUE;
}
