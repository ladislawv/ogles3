/*
 * Sync objects (ES 3.0 5.2; docs/GLES3_PLAN.md §5 item 12): a fence is the
 * library's job serial (job.c), not the GPU's 8-bit counters. It is
 * signalled when no member of the share group has that job open or its
 * render still running (share.c's ogles2_job_user). A fence made while
 * nothing is outstanding is signalled at once.
 *
 * The objects are the share group's (ES 3.0 appendix D), kept in a list so
 * that a name (a pointer) is checked before it is used.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include "ogles2_internal.h"
#include "v3d_context.h"

struct OGLES2Sync
{
    struct OGLES2Sync *next;
    ULONG serial;   /* the job it waits for; 0: signalled */
};

/* sync as one of the group's, or NULL. */
static struct OGLES2Sync *find(struct OGLES2Context *ctx, GLsync sync)
{
    struct OGLES2Sync *s;
    for (s = ctx->share->syncs; s; s = s->next)
        if ((GLsync)s == sync)
            return s;
    return 0;
}

static BOOL signalled(struct OGLES2Context *ctx, struct OGLES2Sync *s)
{
    if (s->serial && !ogles2_job_uses(ctx, s->serial))
        s->serial = 0;
    return !s->serial;
}

GLsync ogles2_glFenceSync(struct OGLES2Context *ctx, GLenum condition, GLbitfield flags)
{
    struct OGLES2Sync *s;
    if (!ogles2_es3_entry(ctx))
        return 0;
    if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    if (flags) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    if (!(s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return 0;
    }
    /* the commands so far: the open job's, else the render still running */
    s->serial = ctx->v3d && ctx->v3d->frame_active ? ctx->job_serial : ctx->pend ? ctx->pend_serial : 0;
    s->next = ctx->share->syncs;
    ctx->share->syncs = s;
    return (GLsync)s;
}

GLboolean ogles2_glIsSync(struct OGLES2Context *ctx, GLsync sync)
{
    if (!ogles2_es3_entry(ctx))
        return GL_FALSE;
    return sync && find(ctx, sync) ? GL_TRUE : GL_FALSE;
}

void ogles2_glDeleteSync(struct OGLES2Context *ctx, GLsync sync)
{
    if (ctx && ctx->share && ctx->share->labels)
        ogles2_label_forget(ctx, GL_SYNC_FENCE, (ULONG)sync); /* KHR_debug's label */
    struct OGLES2Sync **p, *s;
    if (!ogles2_es3_entry(ctx) || !sync)
        return;
    for (p = &ctx->share->syncs; (s = *p); p = &s->next)
        if ((GLsync)s == sync) {
            *p = s->next;
            FreeVec(s);
            return;
        }
    ogles2_error(ctx, GL_INVALID_VALUE);
}

/* The job a fence waits for rendered: the context's own open job flushed
 * (also without GL_SYNC_FLUSH_COMMANDS_BIT: the wait would never end), a
 * render of the group's waited for. Another member's open job is not
 * flushed from here: FALSE. */
static BOOL wait_for(struct OGLES2Context *ctx, struct OGLES2Sync *s)
{
    struct OGLES2Context *user = ogles2_job_user(ctx, s->serial);
    if (!user)
        return TRUE;
    if (user->v3d && user->v3d->frame_active && user->job_serial == s->serial) {
        if (user != ctx)
            return FALSE;
        ogles2_job_flush(ctx, FALSE);
    }
    ogles2_job_finish(user);
    return signalled(ctx, s);
}

GLenum ogles2_glClientWaitSync(struct OGLES2Context *ctx, GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    struct OGLES2Sync *s;
    if (!ogles2_es3_entry(ctx))
        return GL_WAIT_FAILED;
    if (!(s = find(ctx, sync))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return GL_WAIT_FAILED;
    }
    if (flags & ~(GLbitfield)GL_SYNC_FLUSH_COMMANDS_BIT) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return GL_WAIT_FAILED;
    }
    if (signalled(ctx, s))
        return GL_ALREADY_SIGNALED;
    if (!timeout) {
        /* not waiting, but the flush the bit asks for */
        if ((flags & GL_SYNC_FLUSH_COMMANDS_BIT) && ctx->v3d && ctx->v3d->frame_active &&
            ctx->job_serial == s->serial)
            ogles2_job_flush(ctx, FALSE);
        return GL_TIMEOUT_EXPIRED;
    }
    /* A render takes milliseconds: any timeout is waited out in full. */
    return wait_for(ctx, s) ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
}

/* The GPU runs the jobs in order: a server-side wait has nothing to wait for. */
void ogles2_glWaitSync(struct OGLES2Context *ctx, GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    if (!ogles2_es3_entry(ctx))
        return;
    if (!find(ctx, sync) || flags || timeout != GL_TIMEOUT_IGNORED)
        ogles2_error(ctx, GL_INVALID_VALUE);
}

void ogles2_glGetSynciv(struct OGLES2Context *ctx, GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length,
                        GLint *values)
{
    struct OGLES2Sync *s;
    GLint v;
    if (!ogles2_es3_entry(ctx))
        return;
    if (!(s = find(ctx, sync))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    switch (pname) {
    case GL_OBJECT_TYPE:    v = GL_SYNC_FENCE; break;
    case GL_SYNC_STATUS:    v = signalled(ctx, s) ? GL_SIGNALED : GL_UNSIGNALED; break;
    case GL_SYNC_CONDITION: v = GL_SYNC_GPU_COMMANDS_COMPLETE; break;
    case GL_SYNC_FLAGS:     v = 0; break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (bufSize > 0 && values)
        values[0] = v;
    if (length)
        *length = bufSize > 0 ? 1 : 0;
}

/* aglDestroyContext: the group's with its last member. */
void ogles2_syncs_free(struct OGLES2Context *ctx)
{
    struct OGLES2Share *sh = ctx->share;
    struct OGLES2Sync *s;
    if (!sh || sh->members != 1)
        return;
    while ((s = sh->syncs)) {
        sh->syncs = s->next;
        FreeVec(s);
    }
}
