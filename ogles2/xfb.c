/*
 * Transform feedback (ES 3.0 2.15; docs/GLES3_PLAN.md stage C item 1): the
 * objects (glGenTransformFeedbacks and friends), glBeginTransformFeedback,
 * pause, resume and end, and the packets around a draw that feeds back.
 * Objects are a context's own, not shared (ES 3.0 appendix D); each holds
 * the indexed GL_TRANSFORM_FEEDBACK_BUFFER bindings (buffer.c binds them
 * through ogles2_xfb_bindings).
 *
 * The binning (coordinate) shader writes the varyings: the program's file
 * has its specs (TRANSFORM_FEEDBACK_OUTPUT_DATA_SPECs) and its binning
 * variants output them (v3dp/FORMAT.md, TRANSFORM_FEEDBACK). A draw that
 * feeds back gets the specs and each buffer's address and room, as Mesa's
 * v3dx_emit.c gives them, and TRANSFORM_FEEDBACK_FLUSH_AND_COUNT after its
 * primitives (Mesa v3dx_draw.c: else the GPU may hang); the job's binning
 * list ends with PRIMITIVE_COUNTS_FEEDBACK and the specs turned off (Mesa's
 * v3dx_job.c, SWVC5-718). ES 3.0 lets no draw feed back an indexed draw, a
 * primitive mode other than the one begun with, or more vertices than the
 * buffers hold (GL_INVALID_OPERATION), so the CPU knows what each draw
 * writes: the next draw's offsets and the primitives-written query count
 * it, without reading the GPU's counts back. The buffers' GPU copies are
 * their 32-bit words (buffer.c), which the CPU waits for and takes back at
 * its next access (ogles2_buffer_cpu).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"

struct OGLES2Xfb
{
    GLuint name;
    BOOL bound_once;                  /* it exists once bound (glIsTransformFeedback) */
    BOOL active, paused;
    GLenum mode;                      /* glBeginTransformFeedback's primitive mode */
    struct OGLES2Program *program;    /* the program it began with (active only) */
    ULONG written[OGLES2_MAX_XFB_BUFFERS]; /* vertices written into each buffer since it began */
    struct OGLES2XfbBinding bind[OGLES2_MAX_XFB_BUFFERS];
};

struct OGLES2XfbState
{
    struct OGLES2Xfb **tab;           /* by name (glGenTransformFeedbacks'), slot 0 unused */
    GLuint ntab;
    struct OGLES2Xfb zero;            /* object 0, the default */
    struct OGLES2Xfb *bound;          /* never NULL once made: &zero or a named one */
    v3d_mem counts;                   /* PRIMITIVE_COUNTS_FEEDBACK's words (never read) */
    BOOL specs_on;                    /* the open job's last specs packet enabled feedback */
};

static struct OGLES2XfbState *xs_get(struct OGLES2Context *ctx)
{
    if (!ctx->xfb && (ctx->xfb = AllocVec(sizeof(struct OGLES2XfbState), MEMF_ANY | MEMF_CLEAR)))
        ctx->xfb->bound = &ctx->xfb->zero;
    return ctx->xfb;
}

static struct OGLES2Xfb *by_name(struct OGLES2XfbState *xs, GLuint name)
{
    return name && name < xs->ntab ? xs->tab[name] : 0;
}

/* The bound object, or NULL if none was ever made (object 0, inactive). */
static const struct OGLES2Xfb *cur(const struct OGLES2Context *ctx)
{
    return ctx->xfb ? ctx->xfb->bound : 0;
}

struct OGLES2XfbBinding *ogles2_xfb_bindings(struct OGLES2Context *ctx)
{
    struct OGLES2XfbState *xs = xs_get(ctx);
    return xs ? xs->bound->bind : 0;
}

BOOL ogles2_xfb_active(const struct OGLES2Context *ctx)
{
    const struct OGLES2Xfb *o = cur(ctx);
    return o && o->active;
}

BOOL ogles2_xfb_drawing(const struct OGLES2Context *ctx)
{
    const struct OGLES2Xfb *o = cur(ctx);
    return o && o->active && !o->paused;
}

BOOL ogles2_xfb_program_busy(const struct OGLES2Context *ctx, const struct OGLES2Program *p)
{
    const struct OGLES2XfbState *xs = ctx->xfb;
    GLuint i;
    if (!xs)
        return FALSE;
    if (xs->zero.active && xs->zero.program == p)
        return TRUE;
    for (i = 1; i < xs->ntab; i++)
        if (xs->tab[i] && xs->tab[i]->active && xs->tab[i]->program == p)
            return TRUE;
    return FALSE;
}

static BOOL holds(const struct OGLES2Xfb *o, const struct OGLES2Buffer *b)
{
    int k;
    for (k = 0; k < OGLES2_MAX_XFB_BUFFERS; k++)
        if (o->bind[k].b == b)
            return TRUE;
    return FALSE;
}

BOOL ogles2_xfb_uses_buffer(const struct OGLES2Context *ctx, const struct OGLES2Buffer *b)
{
    const struct OGLES2XfbState *xs = ctx->xfb;
    GLuint i;
    if (!xs)
        return FALSE;
    if (holds(&xs->zero, b))
        return TRUE;
    for (i = 1; i < xs->ntab; i++)
        if (xs->tab[i] && holds(xs->tab[i], b))
            return TRUE;
    return FALSE;
}

/* An object's references go (deleted, or the context freed). */
static void drop_refs(struct OGLES2Context *ctx, struct OGLES2Xfb *o)
{
    struct OGLES2Program *p = o->program;
    int k;
    o->active = o->paused = FALSE;
    o->program = 0;
    ogles2_program_unref(ctx, p);
    /* each buffer once, after every reference went: one bound at two
     * indices was freed twice (as draw.c's vab_refresh) */
    for (k = 0; k < OGLES2_MAX_XFB_BUFFERS; k++) {
        struct OGLES2Buffer *b = o->bind[k].b;
        int j;
        if (!b)
            continue;
        for (j = k; j < OGLES2_MAX_XFB_BUFFERS; j++)
            if (o->bind[j].b == b)
                o->bind[j].b = 0;
        ogles2_buffer_release(ctx, b);
    }
}

void ogles2_xfbs_free(struct OGLES2Context *ctx)
{
    struct OGLES2XfbState *xs = ctx->xfb;
    GLuint i;
    if (!xs)
        return;
    for (i = 1; i < xs->ntab; i++)
        if (xs->tab[i]) {
            drop_refs(ctx, xs->tab[i]);
            FreeVec(xs->tab[i]);
        }
    drop_refs(ctx, &xs->zero);
    if (xs->tab)
        FreeVec(xs->tab);
    if (xs->counts.hostptr)
        v3d_mem_free(ogles2_device(), &xs->counts);
    FreeVec(xs);
    ctx->xfb = 0;
}

/* ---- the objects ---- */

void ogles2_glGenTransformFeedbacks(struct OGLES2Context *ctx, GLsizei n, GLuint *ids)
{
    struct OGLES2XfbState *xs;
    GLsizei k;
    GLuint i = 1, nn, j;
    if (!ogles2_es3_entry(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(xs = xs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES2Xfb *o, **tab;
        while (i < xs->ntab && xs->tab[i])
            i++;
        if (i >= xs->ntab) { /* the table grows */
            nn = xs->ntab ? xs->ntab * 2 : 32;
            if (i >= OGLES2_NEAR_NAMES || !(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR))) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            for (j = 0; j < xs->ntab; j++)
                tab[j] = xs->tab[j];
            if (xs->tab)
                FreeVec(xs->tab);
            xs->tab = tab;
            xs->ntab = nn;
        }
        if (!(o = AllocVec(sizeof(*o), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        o->name = i;
        xs->tab[i] = o;
        if (ids)
            ids[k] = i;
    }
}

void ogles2_glDeleteTransformFeedbacks(struct OGLES2Context *ctx, GLsizei n, const GLuint *ids)
{
    struct OGLES2XfbState *xs;
    GLsizei k;
    if (!ogles2_es3_entry(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ids || !(xs = ctx->xfb))
        return;
    /* ES 3.0 2.15.1: an active object is not deleted (none of them is) */
    for (k = 0; k < n; k++) {
        struct OGLES2Xfb *o = by_name(xs, ids[k]);
        if (o && o->active) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    for (k = 0; k < n; k++) {
        struct OGLES2Xfb *o = by_name(xs, ids[k]);
        if (!o)
            continue;
        if (xs->bound == o)
            xs->bound = &xs->zero;
        xs->tab[o->name] = 0;
        drop_refs(ctx, o);
        FreeVec(o);
    }
}

GLboolean ogles2_glIsTransformFeedback(struct OGLES2Context *ctx, GLuint id)
{
    struct OGLES2Xfb *o;
    if (!ogles2_es3_entry(ctx) || !ctx->xfb)
        return GL_FALSE;
    o = by_name(ctx->xfb, id);
    return o && o->bound_once ? GL_TRUE : GL_FALSE;
}

void ogles2_glBindTransformFeedback(struct OGLES2Context *ctx, GLenum target, GLuint id)
{
    struct OGLES2XfbState *xs;
    struct OGLES2Xfb *o;
    if (!ogles2_es3_entry(ctx))
        return;
    if (target != GL_TRANSFORM_FEEDBACK) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(xs = xs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    o = id ? by_name(xs, id) : &xs->zero;
    /* a name glGenTransformFeedbacks did not give; or the bound object is
     * active and not paused (ES 3.0 2.15.1) */
    if (!o || (xs->bound->active && !xs->bound->paused)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    o->bound_once = TRUE;
    xs->bound = o;
}

/* ---- feedback begun, paused, resumed, ended ---- */

/* The buffers program p's varyings go to: bit k for buffer k. */
static ULONG used_buffers(const struct OGLES2Program *p)
{
    const struct OGLES2ImageXfb *x = p && p->image ? p->image->xfb : 0;
    ULONG m = 0;
    int k;
    for (k = 0; x && k < OGLES2_MAX_XFB_BUFFERS; k++)
        if (x->stride[k])
            m |= 1UL << k;
    return m;
}

void ogles2_glBeginTransformFeedback(struct OGLES2Context *ctx, GLenum primitiveMode)
{
    struct OGLES2XfbState *xs;
    struct OGLES2Xfb *o;
    ULONG m;
    int k;
    if (!ogles2_es3_entry(ctx))
        return;
    if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES && primitiveMode != GL_TRIANGLES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(xs = xs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    o = xs->bound;
    /* ES 3.0 2.15.2: not active already; a program with varyings to feed
     * back; a buffer bound to each binding they go to */
    m = used_buffers(ctx->program);
    if (o->active || !m) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    for (k = 0; k < OGLES2_MAX_XFB_BUFFERS; k++)
        if ((m & (1UL << k)) && !o->bind[k].b) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    o->active = TRUE;
    o->paused = FALSE;
    o->mode = primitiveMode;
    o->program = ctx->program;
    memset(o->written, 0, sizeof(o->written));
}

void ogles2_glEndTransformFeedback(struct OGLES2Context *ctx)
{
    struct OGLES2Xfb *o;
    struct OGLES2Program *p;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!ctx->xfb || !(o = ctx->xfb->bound)->active) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    p = o->program;
    o->active = o->paused = FALSE;
    o->program = 0;
    ogles2_program_unref(ctx, p); /* deleted while feedback ran */
}

void ogles2_glPauseTransformFeedback(struct OGLES2Context *ctx)
{
    struct OGLES2Xfb *o;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!ctx->xfb || !(o = ctx->xfb->bound)->active || o->paused) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    o->paused = TRUE;
}

void ogles2_glResumeTransformFeedback(struct OGLES2Context *ctx)
{
    struct OGLES2Xfb *o;
    if (!ogles2_es3_entry(ctx))
        return;
    /* ES 3.0 2.15.2: and with the program it began with in use */
    if (!ctx->xfb || !(o = ctx->xfb->bound)->active || !o->paused || ctx->program != o->program) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    o->paused = FALSE;
}

/* GL_TRANSFORM_FEEDBACK_BINDING, _ACTIVE, _PAUSED (state.c) */
GLuint ogles2_xfb_bound_name(const struct OGLES2Context *ctx)
{
    const struct OGLES2Xfb *o = cur(ctx);
    return o ? o->name : 0;
}

BOOL ogles2_xfb_paused(const struct OGLES2Context *ctx)
{
    const struct OGLES2Xfb *o = cur(ctx);
    return o && o->active && o->paused;
}

/* ---- draws ---- */

/* The vertices a draw of `count` writes in `mode` (whole primitives only). */
static ULONG verts(GLenum mode, GLsizei count)
{
    ULONG n = (ULONG)count;
    return mode == GL_TRIANGLES ? n - n % 3 : mode == GL_LINES ? n & ~1UL : n;
}

/* Binding k's room in bytes. */
static ULONG room(const struct OGLES2XfbBinding *r)
{
    ULONG size = r->b ? ogles2_buffer_size(r->b) : 0; /* deleted since it began: none */
    if (r->offset >= size)
        return 0;
    return r->size && r->size < size - r->offset ? r->size : size - r->offset;
}

BOOL ogles2_xfb_draw_ok(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLsizei instances)
{
    struct OGLES2Xfb *o;
    const struct OGLES2ImageXfb *x;
    ULONG n;
    int k;
    if (!ogles2_xfb_drawing(ctx))
        return TRUE;
    o = ctx->xfb->bound;
    x = o->program && o->program->image ? o->program->image->xfb : 0;
    /* ES 3.0 2.15.2: the mode begun with, and room for every vertex */
    if (mode != o->mode || !x) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    n = verts(mode, count) * (ULONG)(instances > 0 ? instances : 0);
    for (k = 0; k < OGLES2_MAX_XFB_BUFFERS; k++)
        if (x->stride[k] && (o->written[k] + n) * x->stride[k] * 4 > room(&o->bind[k])) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return FALSE;
        }
    return TRUE;
}

static void put32(UBYTE *p, ULONG w)
{
    p[0] = (UBYTE)w;
    p[1] = (UBYTE)(w >> 8);
    p[2] = (UBYTE)(w >> 16);
    p[3] = (UBYTE)(w >> 24);
}

/* Before a draw's primitives, after its state (draw.c's emit_state): with
 * feedback on, its specs (those for points of a vertex shader that writes
 * gl_PointSize with `psiz`) and each buffer's address and room; else
 * feedback off again if a draw of the job turned it on. TRUE: it feeds back. */
BOOL ogles2_xfb_before_draw(struct OGLES2Context *ctx, GLenum mode, BOOL psiz)
{
    V3DContext *v = ctx->v3d;
    struct OGLES2XfbState *xs = ctx->xfb;
    struct OGLES2Xfb *o;
    const struct OGLES2ImageXfb *x;
    UBYTE *p;
    int k;
    if (!ogles2_xfb_drawing(ctx)) {
        if (xs && xs->specs_on && ctx->job_xfb) {
            TransformFeedbackSpecs(v, FALSE, 0);
            xs->specs_on = FALSE;
        }
        return FALSE;
    }
    o = xs->bound;
    x = o->program->image->xfb;
    if (!(p = v3d_buffer_claim_memory(v->current_buf, 2 + 2 * (ULONG)x->nspecs)))
        return FALSE;
    p[0] = v3d_OP_TRANSFORM_FEEDBACK_SPECS;
    p[1] = (UBYTE)(0x80 | x->nspecs); /* enable, then the count */
    for (k = 0; k < x->nspecs; k++) {
        UWORD s = psiz && mode == GL_POINTS ? x->specs_psiz[k] : x->specs[k];
        p[2 + 2 * k] = (UBYTE)s;
        p[3 + 2 * k] = (UBYTE)(s >> 8);
    }
    for (k = 0; k < OGLES2_MAX_XFB_BUFFERS; k++) {
        const struct OGLES2XfbBinding *r = &o->bind[k];
        ULONG done = o->written[k] * x->stride[k] * 4, words, addr;
        if (!x->stride[k])
            continue;
        if (!(addr = ogles2_buffer_xfb(ctx, r->b)) || !(p = v3d_buffer_claim_memory(v->current_buf, 9)))
            return FALSE;
        words = (room(r) - done) / 4;
        p[0] = v3d_OP_TRANSFORM_FEEDBACK_BUFFER;
        put32(p + 1, (ULONG)k | words << 2);
        put32(p + 5, addr + r->offset + done);
    }
    xs->specs_on = TRUE;
    ctx->job_xfb = TRUE;
    return TRUE;
}

/* After the primitives of a draw that fed back: the flush Mesa emits (else
 * the GPU may hang at the next specs), the vertices written counted. */
void ogles2_xfb_after_draw(struct OGLES2Context *ctx, GLenum mode, GLsizei count, GLsizei instances)
{
    struct OGLES2Xfb *o = ctx->xfb->bound;
    const struct OGLES2ImageXfb *x = o->program->image->xfb;
    ULONG n = verts(mode, count) * (ULONG)instances;
    int k;
    DoCommand(ctx->v3d, v3d_OP_TRANSFORM_FEEDBACK_FLUSH_AND_COUNT);
    for (k = 0; k < OGLES2_MAX_XFB_BUFFERS; k++)
        if (x->stride[k])
            o->written[k] += n;
    ogles2_query_xfb_add(ctx, n / (mode == GL_TRIANGLES ? 3 : mode == GL_LINES ? 2 : 1));
}

/* The end of the open job's binning list, before its FLUSH (job.c): after
 * feedback, the primitive counts stored (where nothing reads them) and the
 * specs off, so the feedback block finishes before the next job begins
 * (Mesa's v3dx_job.c, SWVC5-718). */
void ogles2_xfb_job_end(struct OGLES2Context *ctx)
{
    struct OGLES2XfbState *xs = ctx->xfb;
    UBYTE *p;
    if (!ctx->job_xfb)
        return;
    ctx->job_xfb = FALSE;
    if (!xs)
        return;
    xs->specs_on = FALSE;
    if (!xs->counts.hostptr && v3d_mem_alloc(ogles2_device(), &xs->counts, 96) < 0)
        memset(&xs->counts, 0, sizeof(xs->counts));
    if (xs->counts.hostptr && (p = v3d_buffer_claim_memory(ctx->v3d->current_buf, 5))) {
        p[0] = v3d_OP_PRIMITIVE_COUNTS_FEEDBACK; /* seven words, at a 32-byte boundary */
        put32(p + 1, (((ULONG)xs->counts.hostptr + 31) & ~31UL) | v3d_STORE_PRIMITIVE_COUNTS);
    }
    TransformFeedbackSpecs(ctx->v3d, FALSE, 0);
}
