/*
 * Query objects (ES 3.0 2.13; docs/GLES3_PLAN.md §5 item 12): occlusion
 * queries (GL_ANY_SAMPLES_PASSED and _CONSERVATIVE, both answered as the
 * exact one) through the GPU's counter, Mesa's way (v3d_query_pipe.c): the
 * CPU zeroes a counter word, OCCLUSION_QUERY_COUNTER in the binning list
 * points the draws that follow at it (0 turns it off), and the word is read
 * once the last job of the query has rendered. The counters come in groups
 * of 16 consecutive words, each group on a 1024-byte boundary (Mesa's
 * V3D_OQ_GROUP_SIZE; v3dv's "each group of 16 queries is aligned to a 1024
 * byte boundary"): the hardware writes a counter elsewhere than an address
 * past a group's 64 bytes names, so each query's is a group's first word. A job begun while a query is
 * active points at it too (job.c, ctx->oq_addr), so a query spans jobs.
 * GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN counts on the CPU what each draw
 * feeds back (xfb.c: ES 3.0's draw rules make that exact).
 *
 * Not shared: each context has its own (ES 3.0 appendix D).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles3_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_hw.h"

struct OGLES3Query
{
    GLuint name;
    GLenum target;              /* 0 until first begun */
    BOOL active;
    v3d_mem counter;            /* the occlusion counter's block */
    ULONG *word;                /* the counter: the block's first 1024-byte boundary */
    ULONG serial;               /* the query's last job; 0: none outstanding */
    ULONG primitives;           /* GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN's, counted on the CPU (xfb.c) */
};

struct OGLES3QueryState
{
    struct OGLES3Query **tab;   /* by name (glGenQueries'), slot 0 unused */
    GLuint ntab;
    struct OGLES3Query *occlusion, *xfb; /* the active one of each kind */
};

static struct OGLES3QueryState *qs_get(struct OGLES3Context *ctx)
{
    if (!ctx->query)
        ctx->query = AllocVec(sizeof(struct OGLES3QueryState), MEMF_ANY | MEMF_CLEAR);
    return ctx->query;
}

static struct OGLES3Query *by_name(struct OGLES3QueryState *qs, GLuint name)
{
    return qs && name && name < qs->ntab ? qs->tab[name] : 0;
}

static BOOL occlusion_target(GLenum t)
{
    return t == GL_ANY_SAMPLES_PASSED || t == GL_ANY_SAMPLES_PASSED_CONSERVATIVE;
}

/* The open job's binning list points the draws that follow at `addr`. */
static void emit_counter(struct OGLES3Context *ctx, ULONG addr)
{
    V3DContext *v = ctx->v3d;
    if (!v || !v->frame_active)
        return;
    v->current_buf = &v->binning_buf[v->build_slot];
    OcclusionQueryCounter(v, addr);
}

/* The library's own draws (clears drawn as quads, copies) count no samples:
 * the counter off for them (`pause`), then back. */
void ogles2_query_pause(struct OGLES3Context *ctx, BOOL pause)
{
    if (ctx->oq_addr)
        emit_counter(ctx, pause ? 0 : ctx->oq_addr);
}

void ogles2_glGenQueries(struct OGLES3Context *ctx, GLsizei n, GLuint *ids)
{
    struct OGLES3QueryState *qs;
    GLsizei k;
    GLuint i = 1, nn, j;
    if (!ogles2_es3_entry(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(qs = qs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES3Query *q, **tab;
        while (i < qs->ntab && qs->tab[i])
            i++;
        if (i >= qs->ntab) {
            nn = qs->ntab ? qs->ntab * 2 : 32;
            if (i >= OGLES3_NEAR_NAMES || !(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR))) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            for (j = 0; j < qs->ntab; j++)
                tab[j] = qs->tab[j];
            if (qs->tab)
                FreeVec(qs->tab);
            qs->tab = tab;
            qs->ntab = nn;
        }
        if (!(q = AllocVec(sizeof(*q), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        q->name = i;
        qs->tab[i] = q;
        if (ids)
            ids[k] = i;
    }
}

static void free_query(struct OGLES3Context *ctx, struct OGLES3Query *q)
{
    if (q->counter.hostptr)
        ogles2_share_defer_free(ctx, q->serial, &q->counter);
    FreeVec(q);
}

static void end(struct OGLES3Context *ctx, struct OGLES3QueryState *qs, struct OGLES3Query *q);

/* ES 3.0 2.13: deleting an active query ends it. */
void ogles2_glDeleteQueries(struct OGLES3Context *ctx, GLsizei n, const GLuint *ids)
{
    ogles2_labels_forget(ctx, GL_QUERY_KHR, n, ids); /* KHR_debug's labels of the names */
    struct OGLES3QueryState *qs;
    GLsizei k;
    if (!ogles2_es3_entry(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(qs = ctx->query) || !ids)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES3Query *q = by_name(qs, ids[k]);
        if (!q)
            continue;
        if (q->active)
            end(ctx, qs, q);
        qs->tab[q->name] = 0;
        free_query(ctx, q);
    }
}

GLboolean ogles2_glIsQuery(struct OGLES3Context *ctx, GLuint id)
{
    struct OGLES3Query *q;
    if (!ogles2_es3_entry(ctx))
        return GL_FALSE;
    q = by_name(ctx->query, id);
    return q && q->target ? GL_TRUE : GL_FALSE; /* exists once begun */
}

void ogles2_glBeginQuery(struct OGLES3Context *ctx, GLenum target, GLuint id)
{
    struct OGLES3QueryState *qs;
    struct OGLES3Query *q;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!occlusion_target(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    qs = qs_get(ctx);
    q = by_name(qs, id);
    /* a name glGenQueries gave, not active, of this target if begun before;
     * none of its kind active already */
    if (!qs || !q || q->active || (q->target && q->target != target) ||
        (occlusion_target(target) ? qs->occlusion != 0 : qs->xfb != 0)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    q->target = target;
    q->active = TRUE;
    if (!occlusion_target(target)) {
        q->primitives = 0;
        qs->xfb = q;
        return;
    }
    /* A counter a job not yet rendered still writes is left to it. */
    if (q->counter.hostptr && q->serial && ogles2_job_uses(ctx, q->serial)) {
        ogles2_share_defer_free(ctx, q->serial, &q->counter);
        memset(&q->counter, 0, sizeof(q->counter));
    }
    if (!q->counter.hostptr && v3d_mem_alloc(ogles2_device(), &q->counter, 2048) < 0) {
        memset(&q->counter, 0, sizeof(q->counter));
        q->active = FALSE;
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    q->word = (ULONG *)(((ULONG)q->counter.hostptr + 1023) & ~1023UL);
    *(volatile ULONG *)q->word = 0; /* the clean before each submission (job.c) covers it */
    q->serial = 0;
    qs->occlusion = q;
    ctx->oq_addr = (ULONG)q->word;
    emit_counter(ctx, ctx->oq_addr);
}

static void end(struct OGLES3Context *ctx, struct OGLES3QueryState *qs, struct OGLES3Query *q)
{
    q->active = FALSE;
    if (q == qs->xfb) {
        qs->xfb = 0;
        return;
    }
    qs->occlusion = 0;
    ctx->oq_addr = 0;
    emit_counter(ctx, 0);
    /* its last job: the open one, else the render still running */
    q->serial = ctx->v3d && ctx->v3d->frame_active ? ctx->job_serial : ctx->pend ? ctx->pend_serial : 0;
}

void ogles2_glEndQuery(struct OGLES3Context *ctx, GLenum target)
{
    struct OGLES3QueryState *qs;
    struct OGLES3Query *q;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!occlusion_target(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    qs = ctx->query;
    q = !qs ? 0 : occlusion_target(target) ? qs->occlusion : qs->xfb;
    if (!q || q->target != target) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    end(ctx, qs, q);
}

void ogles2_glGetQueryiv(struct OGLES3Context *ctx, GLenum target, GLenum pname, GLint *params)
{
    struct OGLES3QueryState *qs;
    struct OGLES3Query *q;
    if (!ogles2_es3_entry(ctx))
        return;
    if ((!occlusion_target(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) ||
        pname != GL_CURRENT_QUERY) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    qs = ctx->query;
    q = !qs ? 0 : occlusion_target(target) ? qs->occlusion : qs->xfb;
    if (params) /* the active query of that target's kind, if it is of that target */
        *params = q && q->target == target ? (GLint)q->name : 0;
}

/* The query's jobs rendered: TRUE. `wait`: flushed and waited for (a job
 * of another member open still: FALSE); else only the context's own open
 * job submitted, as Mesa flushes even for a check
 * (v3d_query_pipe.c:234-240). */
static BOOL rendered(struct OGLES3Context *ctx, struct OGLES3Query *q, BOOL wait)
{
    struct OGLES3Context *user;
    if (!q->serial || !(user = ogles2_job_user(ctx, q->serial)))
        return TRUE;
    if (user->v3d && user->v3d->frame_active && user->job_serial == q->serial) {
        if (user != ctx)
            return FALSE;
        ogles2_job_flush(ctx, FALSE);
    }
    if (wait)
        ogles2_job_finish(user);
    return !ogles2_job_uses(ctx, q->serial);
}

void ogles2_glGetQueryObjectuiv(struct OGLES3Context *ctx, GLuint id, GLenum pname, GLuint *params)
{
    struct OGLES3Query *q;
    ULONG len = 4, v;
    if (!ogles2_es3_entry(ctx))
        return;
    if (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    q = by_name(ctx->query, id);
    if (!q || !q->target || q->active) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!params)
        return;
    if (pname == GL_QUERY_RESULT_AVAILABLE) {
        *params = rendered(ctx, q, FALSE) ? GL_TRUE : GL_FALSE;
        return;
    }
    rendered(ctx, q, TRUE);
    if (!occlusion_target(q->target)) { /* known on the CPU, at every draw (xfb.c) */
        *params = q->primitives;
        return;
    }
    if (!q->counter.hostptr) {
        *params = 0;
        return;
    }
    CachePostDMA(q->word, &len, 0); /* the GPU's word, not a cache line's */
    v = *(volatile ULONG *)q->word;
    *params = v ? GL_TRUE : GL_FALSE;
}

/* A draw fed back `primitives` (xfb.c): the active query of them counts them. */
void ogles2_query_xfb_add(struct OGLES3Context *ctx, ULONG primitives)
{
    if (ctx->query && ctx->query->xfb)
        ctx->query->xfb->primitives += primitives;
}

/* aglDestroyContext (its jobs finished). */
void ogles2_queries_free(struct OGLES3Context *ctx)
{
    struct OGLES3QueryState *qs = ctx->query;
    GLuint i;
    if (!qs)
        return;
    for (i = 1; i < qs->ntab; i++)
        if (qs->tab[i])
            free_query(ctx, qs->tab[i]);
    if (qs->tab)
        FreeVec(qs->tab);
    FreeVec(qs);
    ctx->query = 0;
    ctx->oq_addr = 0;
}
