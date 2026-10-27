/*
 * Vertex array objects (ES 3.0, and OES_vertex_array_object in GLES2
 * contexts; docs/GLES3_PLAN.md §5 item 3): a context's
 * attribute arrays and element buffer binding, by name. Not shared: each
 * context has its own (ES 3.0 appendix D).
 *
 * The bound object's state is the context's own (ctx->array and buffer.c's
 * element binding), so the draw path reads the context as before: binding
 * another saves it into the object bound until then and loads the new
 * one's. Object 0's waits in `zero` while another is bound.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"

struct OGLES2VertexArray
{
    GLuint name;
    BOOL bound_once;                  /* it exists once bound (glIsVertexArray) */
    struct OGLES2Array array[OGLES2_MAX_VERTEX_ATTRIBS];
    struct OGLES2Buffer *element;
};

struct OGLES2VaoState
{
    struct OGLES2VertexArray **tab;   /* by name (glGenVertexArrays'), slot 0 unused */
    GLuint ntab;
    struct OGLES2VertexArray zero;    /* object 0's state while another is bound */
    struct OGLES2VertexArray *bound;  /* NULL: object 0 */
};

static struct OGLES2VaoState *vs_get(struct OGLES2Context *ctx)
{
    if (!ctx->vao)
        ctx->vao = AllocVec(sizeof(struct OGLES2VaoState), MEMF_ANY | MEMF_CLEAR);
    return ctx->vao;
}

static struct OGLES2VertexArray *by_name(struct OGLES2VaoState *vs, GLuint name)
{
    return name && name < vs->ntab ? vs->tab[name] : 0;
}

/* The bound object becomes `to` (&vs->zero: object 0). */
static void bind(struct OGLES2Context *ctx, struct OGLES2VaoState *vs, struct OGLES2VertexArray *to)
{
    struct OGLES2VertexArray *from = vs->bound ? vs->bound : &vs->zero;
    if (to == from)
        return;
    memcpy(from->array, ctx->array, sizeof(ctx->array));
    from->element = ogles2_buffer_element(ctx);
    memcpy(ctx->array, to->array, sizeof(ctx->array));
    ogles2_buffer_set_element(ctx, to->element);
    vs->bound = to == &vs->zero ? 0 : to;
}

static void genvertexarrays(struct OGLES2Context *ctx, GLsizei n, GLuint *arrays)
{
    struct OGLES2VaoState *vs;
    GLsizei k;
    GLuint i = 1, nn, j;
    int a;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(vs = vs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        struct OGLES2VertexArray *o, **tab;
        while (i < vs->ntab && vs->tab[i])
            i++;
        if (i >= vs->ntab) { /* the table grows */
            nn = vs->ntab ? vs->ntab * 2 : 32;
            if (i >= OGLES2_NEAR_NAMES || !(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR))) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            for (j = 0; j < vs->ntab; j++)
                tab[j] = vs->tab[j];
            if (vs->tab)
                FreeVec(vs->tab);
            vs->tab = tab;
            vs->ntab = nn;
        }
        if (!(o = AllocVec(sizeof(*o), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        o->name = i;
        for (a = 0; a < OGLES2_MAX_VERTEX_ATTRIBS; a++) { /* as a context's (state.c) */
            o->array[a].size = 4;
            o->array[a].type = GL_FLOAT;
            o->array[a].binding = (UBYTE)a; /* ES 3.1: its own binding, of stride 16 */
            o->array[a].bstride = 16;
        }
        vs->tab[i] = o;
        if (arrays)
            arrays[k] = i;
    }
}

/* An object going away (o out of the table already): its buffers' references
 * dropped, each buffer once, so a deleted one nothing else uses is freed. */
static void drop_buffers(struct OGLES2Context *ctx, struct OGLES2VertexArray *o)
{
    struct OGLES2Buffer *b;
    int a, j;
    for (a = 0; a <= 2 * OGLES2_MAX_VERTEX_ATTRIBS; a++) {
        b = a < OGLES2_MAX_VERTEX_ATTRIBS ? o->array[a].buffer
          : a < 2 * OGLES2_MAX_VERTEX_ATTRIBS ? o->array[a - OGLES2_MAX_VERTEX_ATTRIBS].bbuffer /* ES 3.1's bindings */
                                              : o->element;
        if (!b)
            continue;
        for (j = 0; j < OGLES2_MAX_VERTEX_ATTRIBS; j++) {
            if (o->array[j].buffer == b)
                o->array[j].buffer = 0;
            if (o->array[j].bbuffer == b)
                o->array[j].bbuffer = 0;
        }
        if (o->element == b)
            o->element = 0;
        ogles2_buffer_release(ctx, b);
    }
}

/* ES 3.0 2.10: deleting the bound object binds object 0. */
static void deletevertexarrays(struct OGLES2Context *ctx, GLsizei n, const GLuint *arrays)
{
    struct OGLES2VaoState *vs;
    GLsizei k;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(vs = ctx->vao) || !arrays)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2VertexArray *o = by_name(vs, arrays[k]);
        if (!o)
            continue;
        if (vs->bound == o)
            bind(ctx, vs, &vs->zero);
        vs->tab[o->name] = 0;
        drop_buffers(ctx, o);
        FreeVec(o);
    }
}

static GLboolean isvertexarray(struct OGLES2Context *ctx, GLuint array)
{
    struct OGLES2VertexArray *o;
    if (!ctx->vao)
        return GL_FALSE;
    o = by_name(ctx->vao, array);
    return o && o->bound_once ? GL_TRUE : GL_FALSE;
}

static void bindvertexarray(struct OGLES2Context *ctx, GLuint array)
{
    struct OGLES2VaoState *vs;
    struct OGLES2VertexArray *o = 0;
    if (!(vs = vs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    /* a name glGenVertexArrays gave and not deleted since */
    if (array && !(o = by_name(vs, array))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (o)
        o->bound_once = TRUE;
    bind(ctx, vs, o ? o : &vs->zero);
}

/* ES 3.0's entries, and OES_vertex_array_object's in GLES2 contexts (the
 * same objects; ES3 contexts have both too). */
void ogles2_glGenVertexArrays(struct OGLES2Context *ctx, GLsizei n, GLuint *arrays)
{
    if (ogles2_es3_entry(ctx))
        genvertexarrays(ctx, n, arrays);
}

void ogles2_glDeleteVertexArrays(struct OGLES2Context *ctx, GLsizei n, const GLuint *arrays)
{
    if (ogles2_es3_entry(ctx))
        deletevertexarrays(ctx, n, arrays);
}

GLboolean ogles2_glIsVertexArray(struct OGLES2Context *ctx, GLuint array)
{
    return ogles2_es3_entry(ctx) ? isvertexarray(ctx, array) : GL_FALSE;
}

void ogles2_glBindVertexArray(struct OGLES2Context *ctx, GLuint array)
{
    if (ogles2_es3_entry(ctx))
        bindvertexarray(ctx, array);
}

void ogles2_glGenVertexArraysOES(struct OGLES2Context *ctx, GLsizei n, GLuint *arrays)
{
    if (ogles2_context_valid(ctx))
        genvertexarrays(ctx, n, arrays);
}

void ogles2_glDeleteVertexArraysOES(struct OGLES2Context *ctx, GLsizei n, const GLuint *arrays)
{
    if (ogles2_context_valid(ctx))
        deletevertexarrays(ctx, n, arrays);
}

GLboolean ogles2_glIsVertexArrayOES(struct OGLES2Context *ctx, GLuint array)
{
    return ogles2_context_valid(ctx) ? isvertexarray(ctx, array) : GL_FALSE;
}

void ogles2_glBindVertexArrayOES(struct OGLES2Context *ctx, GLuint array)
{
    if (ogles2_context_valid(ctx))
        bindvertexarray(ctx, array);
}

/* The bound object's name (GL_VERTEX_ARRAY_BINDING); 0: object 0. */
GLuint ogles2_vao_bound(const struct OGLES2Context *ctx)
{
    return ctx->vao && ctx->vao->bound ? ctx->vao->bound->name : 0;
}

/* Whether one of the context's objects not bound now uses b (the bound
 * one's state is the context's): glDeleteBuffers keeps such a buffer
 * (ES 3.0 appendix D.1.2), and buffer.c frees it when none does. */
BOOL ogles2_vao_uses_buffer(struct OGLES2Context *ctx, const struct OGLES2Buffer *b)
{
    struct OGLES2VaoState *vs = ctx->vao;
    GLuint i;
    int a;
    if (!vs)
        return FALSE;
    for (i = 0; i <= vs->ntab; i++) {
        struct OGLES2VertexArray *o = i < vs->ntab ? vs->tab[i] : &vs->zero;
        if (!o || o == vs->bound || (o == &vs->zero && !vs->bound))
            continue;
        if (o->element == b)
            return TRUE;
        for (a = 0; a < OGLES2_MAX_VERTEX_ATTRIBS; a++)
            if (o->array[a].buffer == b || o->array[a].bbuffer == b)
                return TRUE;
    }
    return FALSE;
}

/* aglDestroyContext: the objects, and the references they and the
 * context's own arrays hold (a deleted buffer only they used is freed). */
void ogles2_vaos_free(struct OGLES2Context *ctx)
{
    struct OGLES2VaoState *vs = ctx->vao;
    struct OGLES2VertexArray *o;
    GLuint i;
    if (!vs)
        return;
    bind(ctx, vs, &vs->zero);
    o = &vs->zero;
    memcpy(o->array, ctx->array, sizeof(ctx->array));
    o->element = ogles2_buffer_element(ctx);
    for (i = 0; i < OGLES2_MAX_VERTEX_ATTRIBS; i++)
        ctx->array[i].buffer = 0;
    ogles2_buffer_set_element(ctx, 0);
    drop_buffers(ctx, o);
    for (i = 1; i < vs->ntab; i++)
        if ((o = vs->tab[i])) {
            vs->tab[i] = 0;
            drop_buffers(ctx, o);
            FreeVec(o);
        }
    if (vs->tab)
        FreeVec(vs->tab);
    FreeVec(vs);
    ctx->vao = 0;
}
