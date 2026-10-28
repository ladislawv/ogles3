/*
 * KHR_debug (docs/GLES3_PLAN.md §7, item 7): debug messages, their filters
 * and debug groups, the message log or the program's callback, and object
 * labels. Every GL error is also a message (ogles2_error): source API, type
 * error, high severity, its enum the id. A context is never a debug
 * context: GL_DEBUG_OUTPUT starts disabled. Messages are always given
 * during the call that makes them (synchronous whatever
 * GL_DEBUG_OUTPUT_SYNCHRONOUS says). The callback is called as a C function
 * with its arguments on the stack, from the calling task.
 *
 * Filters: each group keeps the controls given in it, newest first; a
 * message's state is the first control that matches it, from the top group
 * down, else the default (all enabled but low severity). A pushed group
 * starts empty, so it sees its parent's state (KHR_debug's copy), and a pop
 * drops what was given in it.
 *
 * Labels: one list per share group, by identifier and name (a vertex array,
 * framebuffer, query, transform feedback or pipeline object's also by its
 * context, whose own they are); dropped when the object's name is deleted.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"

#define LOG_MAX     64  /* GL_MAX_DEBUG_LOGGED_MESSAGES */
#define MESSAGE_MAX 256 /* GL_MAX_DEBUG_MESSAGE_LENGTH */
#define LABEL_MAX   256 /* GL_MAX_LABEL_LENGTH */
#define GROUPS_MAX  64  /* GL_MAX_DEBUG_GROUP_STACK_DEPTH */

struct control
{
    struct control *next;           /* older */
    GLenum source, type, severity;  /* GL_DONT_CARE: any */
    GLsizei count;                  /* ids[count]; 0: by severity */
    GLboolean enabled;
    GLuint ids[1];
};

struct group
{
    struct control *controls;       /* given in this group, newest first */
    GLenum source;
    GLuint id;
    char *message;                  /* its push message (group 0: none) */
    GLsizei length;
};

struct message
{
    GLenum source, type, severity;
    GLuint id;
    GLsizei length;                 /* without the NUL */
    char *text;
};

struct OGLES3Debug
{
    BOOL output, synchronous;
    GLDEBUGPROCKHR callback;
    const void *user;
    UBYTE calling;                  /* in the callback (a message from there is dropped) */
    struct message log[LOG_MAX];    /* oldest at first */
    ULONG first, count;
    struct group group[GROUPS_MAX]; /* group[0] the default */
    ULONG depth;                    /* GL_DEBUG_GROUP_STACK_DEPTH */
};

struct OGLES3Label
{
    struct OGLES3Label *next;
    GLenum identifier;
    ULONG name;                     /* a sync object's address */
    struct OGLES3Context *owner;    /* a container object's context, else NULL */
    char *text;
};

static struct OGLES3Debug *dbg_get(struct OGLES3Context *ctx)
{
    if (!ctx->dbg && (ctx->dbg = AllocVec(sizeof(struct OGLES3Debug), MEMF_ANY | MEMF_CLEAR)) != 0)
        ctx->dbg->depth = 1;
    if (!ctx->dbg)
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
    return ctx->dbg;
}

static void free_controls(struct group *g)
{
    while (g->controls) {
        struct control *c = g->controls;
        g->controls = c->next;
        FreeVec(c);
    }
}

void ogles2_debug_free(struct OGLES3Context *ctx)
{
    struct OGLES3Debug *d = ctx->dbg;
    struct OGLES3Label **at;
    ULONG k;
    if (d) {
        for (k = 0; k < d->count; k++)
            FreeVec(d->log[(d->first + k) % LOG_MAX].text);
        for (k = 0; k < d->depth; k++) {
            free_controls(&d->group[k]);
            if (d->group[k].message)
                FreeVec(d->group[k].message);
        }
        FreeVec(d);
        ctx->dbg = 0;
    }
    /* the context's own objects' labels */
    for (at = ctx->share ? &ctx->share->labels : 0; at && *at;) {
        struct OGLES3Label *l = *at;
        if (l->owner == ctx) {
            *at = l->next;
            FreeVec(l->text);
            FreeVec(l);
        } else {
            at = &l->next;
        }
    }
}

void ogles2_labels_free(struct OGLES3Share *sh)
{
    while (sh->labels) {
        struct OGLES3Label *l = sh->labels;
        sh->labels = l->next;
        FreeVec(l->text);
        FreeVec(l);
    }
}

/* ---- messages ---- */

static BOOL is_source(GLenum e)
{
    return e == GL_DEBUG_SOURCE_API_KHR || e == GL_DEBUG_SOURCE_WINDOW_SYSTEM_KHR ||
           e == GL_DEBUG_SOURCE_SHADER_COMPILER_KHR || e == GL_DEBUG_SOURCE_THIRD_PARTY_KHR ||
           e == GL_DEBUG_SOURCE_APPLICATION_KHR || e == GL_DEBUG_SOURCE_OTHER_KHR;
}

static BOOL is_type(GLenum e)
{
    return e == GL_DEBUG_TYPE_ERROR_KHR || e == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR ||
           e == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR || e == GL_DEBUG_TYPE_PORTABILITY_KHR ||
           e == GL_DEBUG_TYPE_PERFORMANCE_KHR || e == GL_DEBUG_TYPE_OTHER_KHR || e == GL_DEBUG_TYPE_MARKER_KHR ||
           e == GL_DEBUG_TYPE_PUSH_GROUP_KHR || e == GL_DEBUG_TYPE_POP_GROUP_KHR;
}

static BOOL is_severity(GLenum e)
{
    return e == GL_DEBUG_SEVERITY_HIGH_KHR || e == GL_DEBUG_SEVERITY_MEDIUM_KHR ||
           e == GL_DEBUG_SEVERITY_LOW_KHR || e == GL_DEBUG_SEVERITY_NOTIFICATION_KHR;
}

static BOOL enabled(const struct OGLES3Debug *d, GLenum source, GLenum type, GLuint id, GLenum severity)
{
    LONG g;
    for (g = (LONG)d->depth - 1; g >= 0; g--) {
        const struct control *c;
        for (c = d->group[g].controls; c; c = c->next) {
            GLsizei k;
            if ((c->source != GL_DONT_CARE && c->source != source) || (c->type != GL_DONT_CARE && c->type != type))
                continue;
            if (c->count) {
                for (k = 0; k < c->count && c->ids[k] != id; k++)
                    ;
                if (k == c->count)
                    continue;
            } else if (c->severity != GL_DONT_CARE && c->severity != severity) {
                continue;
            }
            return c->enabled;
        }
    }
    return severity != GL_DEBUG_SEVERITY_LOW_KHR;
}

/* A message (`length` characters at `text`, below MESSAGE_MAX) to the
 * callback or the log, when debug output is on and its filters let it. */
static void emit(struct OGLES3Context *ctx, GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                 const char *text)
{
    struct OGLES3Debug *d = ctx->dbg;
    struct message *m;
    if (!d || !d->output || d->calling || !enabled(d, source, type, id, severity))
        return;
    if (d->callback) {
        char buf[MESSAGE_MAX];
        memcpy(buf, text, (ULONG)length);
        buf[length] = 0;
        d->calling = 1;
        d->callback(source, type, id, severity, length, buf, d->user);
        d->calling = 0;
        return;
    }
    if (d->count == LOG_MAX) /* full: later messages are lost */
        return;
    m = &d->log[(d->first + d->count) % LOG_MAX];
    if (!(m->text = AllocVec((ULONG)length + 1, MEMF_ANY)))
        return;
    memcpy(m->text, text, (ULONG)length);
    m->text[length] = 0;
    m->source = source;
    m->type = type;
    m->id = id;
    m->severity = severity;
    m->length = length;
    d->count++;
}

void ogles2_debug_error(struct OGLES3Context *ctx, GLenum error)
{
    const char *t;
    if (!ctx->dbg || !ctx->dbg->output)
        return;
    switch (error) {
    case GL_INVALID_ENUM:                  t = "GL_INVALID_ENUM: an enum argument out of range"; break;
    case GL_INVALID_VALUE:                 t = "GL_INVALID_VALUE: a numeric argument out of range"; break;
    case GL_INVALID_OPERATION:             t = "GL_INVALID_OPERATION: not allowed in the current state"; break;
    case GL_INVALID_FRAMEBUFFER_OPERATION: t = "GL_INVALID_FRAMEBUFFER_OPERATION: the framebuffer is not complete"; break;
    case GL_OUT_OF_MEMORY:                 t = "GL_OUT_OF_MEMORY: not enough memory left"; break;
    case GL_STACK_OVERFLOW_KHR:            t = "GL_STACK_OVERFLOW: the debug group stack is full"; break;
    case GL_STACK_UNDERFLOW_KHR:           t = "GL_STACK_UNDERFLOW: no debug group to pop"; break;
    default:                               t = "a GL error"; break;
    }
    emit(ctx, GL_DEBUG_SOURCE_API_KHR, GL_DEBUG_TYPE_ERROR_KHR, error, GL_DEBUG_SEVERITY_HIGH_KHR, (GLsizei)strlen(t),
         t);
}

/* A message's length (negative: up to the NUL); -1 (GL_INVALID_VALUE set)
 * when it reaches the limit. */
static GLsizei message_length(struct OGLES3Context *ctx, GLsizei length, const GLchar *buf)
{
    if (length < 0)
        length = buf ? (GLsizei)strlen(buf) : 0;
    if (length >= MESSAGE_MAX) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return -1;
    }
    return length;
}

void ogles2_glDebugMessageControlKHR(struct OGLES3Context *ctx, GLenum source, GLenum type, GLenum severity,
                                     GLsizei count, const GLuint *ids, GLboolean enable)
{
    struct OGLES3Debug *d;
    struct group *g;
    struct control *c;
    if (!ogles2_context_valid(ctx))
        return;
    if ((source != GL_DONT_CARE && !is_source(source)) || (type != GL_DONT_CARE && !is_type(type)) ||
        (severity != GL_DONT_CARE && !is_severity(severity))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (count > 0 && (source == GL_DONT_CARE || type == GL_DONT_CARE || severity != GL_DONT_CARE)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (count > 0 && !ids)
        return;
    if (!(d = dbg_get(ctx)))
        return;
    g = &d->group[d->depth - 1];
    /* one for every message: the group's older ones no longer matter */
    if (!count && source == GL_DONT_CARE && type == GL_DONT_CARE && severity == GL_DONT_CARE)
        free_controls(g);
    if (!(c = AllocVec(sizeof(*c) + (count > 1 ? (ULONG)(count - 1) * sizeof(GLuint) : 0), MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    c->source = source;
    c->type = type;
    c->severity = severity;
    c->count = count;
    c->enabled = enable ? GL_TRUE : GL_FALSE;
    if (count)
        memcpy(c->ids, ids, (ULONG)count * sizeof(GLuint));
    c->next = g->controls;
    g->controls = c;
}

void ogles2_glDebugMessageInsertKHR(struct OGLES3Context *ctx, GLenum source, GLenum type, GLuint id,
                                    GLenum severity, GLsizei length, const GLchar *buf)
{
    if (!ogles2_context_valid(ctx))
        return;
    if ((source != GL_DEBUG_SOURCE_APPLICATION_KHR && source != GL_DEBUG_SOURCE_THIRD_PARTY_KHR) ||
        !is_type(type) || !is_severity(severity)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if ((length = message_length(ctx, length, buf)) < 0)
        return;
    emit(ctx, source, type, id, severity, length, buf ? buf : "");
}

void ogles2_glDebugMessageCallbackKHR(struct OGLES3Context *ctx, GLDEBUGPROCKHR callback, const void *userParam)
{
    struct OGLES3Debug *d;
    if (!ogles2_context_valid(ctx) || !(d = dbg_get(ctx)))
        return;
    d->callback = callback;
    d->user = userParam;
}

GLuint ogles2_glGetDebugMessageLogKHR(struct OGLES3Context *ctx, GLuint count, GLsizei bufSize, GLenum *sources,
                                      GLenum *types, GLuint *ids, GLenum *severities, GLsizei *lengths,
                                      GLchar *messageLog)
{
    struct OGLES3Debug *d;
    GLuint n = 0;
    if (!ogles2_context_valid(ctx))
        return 0;
    if (bufSize < 0 && messageLog) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    if (!(d = ctx->dbg))
        return 0;
    while (n < count && d->count) {
        struct message *m = &d->log[d->first];
        if (messageLog) {
            if (m->length + 1 > bufSize)
                break;
            memcpy(messageLog, m->text, (ULONG)m->length + 1);
            messageLog += m->length + 1;
            bufSize -= m->length + 1;
        }
        if (sources)
            sources[n] = m->source;
        if (types)
            types[n] = m->type;
        if (ids)
            ids[n] = m->id;
        if (severities)
            severities[n] = m->severity;
        if (lengths)
            lengths[n] = m->length + 1;
        FreeVec(m->text);
        m->text = 0;
        d->first = (d->first + 1) % LOG_MAX;
        d->count--;
        n++;
    }
    return n;
}

void ogles2_glPushDebugGroupKHR(struct OGLES3Context *ctx, GLenum source, GLuint id, GLsizei length,
                                const GLchar *message)
{
    struct OGLES3Debug *d;
    struct group *g;
    if (!ogles2_context_valid(ctx))
        return;
    if (source != GL_DEBUG_SOURCE_APPLICATION_KHR && source != GL_DEBUG_SOURCE_THIRD_PARTY_KHR) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if ((length = message_length(ctx, length, message)) < 0 || !(d = dbg_get(ctx)))
        return;
    if (d->depth >= GROUPS_MAX) {
        ogles2_error(ctx, GL_STACK_OVERFLOW_KHR);
        return;
    }
    g = &d->group[d->depth];
    memset(g, 0, sizeof(*g));
    if (!(g->message = AllocVec((ULONG)length + 1, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (length)
        memcpy(g->message, message, (ULONG)length);
    g->message[length] = 0;
    g->source = source;
    g->id = id;
    g->length = length;
    emit(ctx, source, GL_DEBUG_TYPE_PUSH_GROUP_KHR, id, GL_DEBUG_SEVERITY_NOTIFICATION_KHR, length, g->message);
    d->depth++;
}

void ogles2_glPopDebugGroupKHR(struct OGLES3Context *ctx)
{
    struct OGLES3Debug *d;
    struct group g;
    if (!ogles2_context_valid(ctx))
        return;
    d = ctx->dbg;
    if (!d || d->depth <= 1) {
        ogles2_error(ctx, GL_STACK_UNDERFLOW_KHR);
        return;
    }
    g = d->group[--d->depth];
    free_controls(&g);
    emit(ctx, g.source, GL_DEBUG_TYPE_POP_GROUP_KHR, g.id, GL_DEBUG_SEVERITY_NOTIFICATION_KHR, g.length, g.message);
    FreeVec(g.message);
}

void ogles2_glGetPointervKHR(struct OGLES3Context *ctx, GLenum pname, void **params)
{
    if (!ogles2_context_valid(ctx) || !params)
        return;
    switch (pname) {
    case GL_DEBUG_CALLBACK_FUNCTION_KHR:   *params = ctx->dbg ? (void *)ctx->dbg->callback : 0; break;
    case GL_DEBUG_CALLBACK_USER_PARAM_KHR: *params = ctx->dbg ? (void *)ctx->dbg->user : 0; break;
    default:                               ogles2_error(ctx, GL_INVALID_ENUM); break;
    }
}

/* GL_DEBUG_OUTPUT and GL_DEBUG_OUTPUT_SYNCHRONOUS: on 1, off 0, a query
 * -1 (*state); FALSE for another capability. */
BOOL ogles2_debug_cap(struct OGLES3Context *ctx, GLenum cap, int on, GLboolean *state)
{
    struct OGLES3Debug *d;
    BOOL *flag;
    if (cap != GL_DEBUG_OUTPUT_KHR && cap != GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR)
        return FALSE;
    if (on < 0) {
        *state = ctx->dbg && (cap == GL_DEBUG_OUTPUT_KHR ? ctx->dbg->output : ctx->dbg->synchronous);
        return TRUE;
    }
    if (!(d = dbg_get(ctx)))
        return TRUE;
    flag = cap == GL_DEBUG_OUTPUT_KHR ? &d->output : &d->synchronous;
    *flag = on ? TRUE : FALSE;
    return TRUE;
}

/* glGet*'s debug state and limits; FALSE for another pname. */
BOOL ogles2_debug_query(struct OGLES3Context *ctx, GLenum pname, GLint *v, BOOL *boolean)
{
    const struct OGLES3Debug *d = ctx->dbg;
    GLboolean b = GL_FALSE;
    *boolean = FALSE;
    switch (pname) {
    case GL_DEBUG_LOGGED_MESSAGES_KHR:          *v = d ? (GLint)d->count : 0; return TRUE;
    case GL_DEBUG_NEXT_LOGGED_MESSAGE_LENGTH_KHR: *v = d && d->count ? d->log[d->first].length + 1 : 0; return TRUE;
    case GL_DEBUG_GROUP_STACK_DEPTH_KHR:        *v = d ? (GLint)d->depth : 1; return TRUE;
    case GL_MAX_DEBUG_MESSAGE_LENGTH_KHR:       *v = MESSAGE_MAX; return TRUE;
    case GL_MAX_DEBUG_LOGGED_MESSAGES_KHR:      *v = LOG_MAX; return TRUE;
    case GL_MAX_DEBUG_GROUP_STACK_DEPTH_KHR:    *v = GROUPS_MAX; return TRUE;
    case GL_MAX_LABEL_LENGTH_KHR:               *v = LABEL_MAX; return TRUE;
    case GL_DEBUG_OUTPUT_KHR:
    case GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR:
        ogles2_debug_cap(ctx, pname, -1, &b);
        *v = b;
        *boolean = TRUE;
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---- object labels ---- */

/* The object `name` of `identifier` exists: 1; -1 GL_INVALID_ENUM (not an
 * identifier of this context's version), 0 GL_INVALID_VALUE. *own: a
 * container object, the context's own. */
static int object_exists(struct OGLES3Context *ctx, GLenum identifier, GLuint name, BOOL *own)
{
    BOOL es3 = ogles2_es3(ctx);
    *own = FALSE;
    switch (identifier) {
    case GL_BUFFER_KHR:       return ogles2_glIsBuffer(ctx, name) ? 1 : 0;
    case GL_SHADER_KHR:       return ogles2_glIsShader(ctx, name) ? 1 : 0;
    case GL_PROGRAM_KHR:      return ogles2_glIsProgram(ctx, name) ? 1 : 0;
    case GL_TEXTURE:          return ogles2_glIsTexture(ctx, name) ? 1 : 0;
    case GL_RENDERBUFFER:     return ogles2_glIsRenderbuffer(ctx, name) ? 1 : 0;
    case GL_FRAMEBUFFER:      *own = TRUE; return ogles2_glIsFramebuffer(ctx, name) ? 1 : 0;
    case GL_VERTEX_ARRAY_KHR: *own = TRUE; return ogles2_glIsVertexArrayOES(ctx, name) ? 1 : 0;
    case GL_SAMPLER_KHR:      return !es3 ? -1 : ogles2_glIsSampler(ctx, name) ? 1 : 0;
    case GL_QUERY_KHR:        *own = TRUE; return !es3 ? -1 : ogles2_glIsQuery(ctx, name) ? 1 : 0;
    case GL_TRANSFORM_FEEDBACK: *own = TRUE; return !es3 ? -1 : ogles2_glIsTransformFeedback(ctx, name) ? 1 : 0;
    case GL_PROGRAM_PIPELINE_KHR:
        *own = TRUE;
        return !ogles2_es31(ctx) ? -1 : ogles2_glIsProgramPipeline(ctx, name) ? 1 : 0;
    default:
        return -1;
    }
}

static struct OGLES3Label **label_at(struct OGLES3Context *ctx, GLenum identifier, ULONG name, BOOL own)
{
    struct OGLES3Label **at;
    for (at = &ctx->share->labels; *at; at = &(*at)->next)
        if ((*at)->identifier == identifier && (*at)->name == name && (*at)->owner == (own ? ctx : 0))
            return at;
    return at;
}

void ogles2_label_forget(struct OGLES3Context *ctx, GLenum identifier, ULONG name)
{
    BOOL own = identifier == GL_FRAMEBUFFER || identifier == GL_VERTEX_ARRAY_KHR || identifier == GL_QUERY_KHR ||
               identifier == GL_TRANSFORM_FEEDBACK || identifier == GL_PROGRAM_PIPELINE_KHR;
    struct OGLES3Label **at, *l;
    if (!ctx || !ctx->share || !ctx->share->labels)
        return;
    at = label_at(ctx, identifier, name, own);
    if ((l = *at) != 0) {
        *at = l->next;
        FreeVec(l->text);
        FreeVec(l);
    }
}

void ogles2_labels_forget(struct OGLES3Context *ctx, GLenum identifier, GLsizei n, const GLuint *names)
{
    GLsizei k;
    if (ctx && ctx->share && ctx->share->labels && names)
        for (k = 0; k < n; k++)
            ogles2_label_forget(ctx, identifier, names[k]);
}

static void set_label(struct OGLES3Context *ctx, GLenum identifier, ULONG name, BOOL own, GLsizei length,
                      const GLchar *label)
{
    struct OGLES3Label **at, *l;
    if (label) {
        if (length < 0)
            length = (GLsizei)strlen(label);
        if (length >= LABEL_MAX) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
    }
    at = label_at(ctx, identifier, name, own);
    if ((l = *at) != 0) { /* replaced or removed */
        *at = l->next;
        FreeVec(l->text);
        FreeVec(l);
    }
    if (!label)
        return;
    if (!(l = AllocVec(sizeof(*l), MEMF_ANY)) || !(l->text = AllocVec((ULONG)length + 1, MEMF_ANY))) {
        if (l)
            FreeVec(l);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    memcpy(l->text, label, (ULONG)length);
    l->text[length] = 0;
    l->identifier = identifier;
    l->name = name;
    l->owner = own ? ctx : 0;
    l->next = ctx->share->labels;
    ctx->share->labels = l;
}

static void get_label(struct OGLES3Context *ctx, GLenum identifier, ULONG name, BOOL own, GLsizei bufSize,
                      GLsizei *length, GLchar *label)
{
    const struct OGLES3Label *l = *label_at(ctx, identifier, name, own);
    GLsizei n = l ? (GLsizei)strlen(l->text) : 0;
    if (label) {
        if (bufSize <= 0) {
            n = 0;
        } else {
            if (n > bufSize - 1)
                n = bufSize - 1;
            if (n)
                memcpy(label, l->text, (ULONG)n);
            label[n] = 0;
        }
    }
    if (length)
        *length = n;
}

void ogles2_glObjectLabelKHR(struct OGLES3Context *ctx, GLenum identifier, GLuint name, GLsizei length,
                             const GLchar *label)
{
    BOOL own;
    int e;
    if (!ogles2_context_valid(ctx))
        return;
    if ((e = object_exists(ctx, identifier, name, &own)) <= 0) {
        ogles2_error(ctx, e < 0 ? GL_INVALID_ENUM : GL_INVALID_VALUE);
        return;
    }
    set_label(ctx, identifier, name, own, length, label);
}

void ogles2_glGetObjectLabelKHR(struct OGLES3Context *ctx, GLenum identifier, GLuint name, GLsizei bufSize,
                                GLsizei *length, GLchar *label)
{
    BOOL own;
    int e;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if ((e = object_exists(ctx, identifier, name, &own)) <= 0) {
        ogles2_error(ctx, e < 0 ? GL_INVALID_ENUM : GL_INVALID_VALUE);
        return;
    }
    get_label(ctx, identifier, name, own, bufSize, length, label);
}

void ogles2_glObjectPtrLabelKHR(struct OGLES3Context *ctx, const void *ptr, GLsizei length, const GLchar *label)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (!ogles2_es3(ctx) || !ogles2_glIsSync(ctx, (GLsync)ptr)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    set_label(ctx, GL_SYNC_FENCE, (ULONG)ptr, FALSE, length, label);
}

void ogles2_glGetObjectPtrLabelKHR(struct OGLES3Context *ctx, const void *ptr, GLsizei bufSize, GLsizei *length,
                                   GLchar *label)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ogles2_es3(ctx) || !ogles2_glIsSync(ctx, (GLsync)ptr)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    get_label(ctx, GL_SYNC_FENCE, (ULONG)ptr, FALSE, bufSize, length, label);
}
