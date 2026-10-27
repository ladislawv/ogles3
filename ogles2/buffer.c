/*
 * Buffer objects (plan section 7.5): glGenBuffers, glBindBuffer,
 * glBufferData, glBufferSubData, and the GPU copies the draw path reads.
 *
 * A buffer keeps a CPU shadow in the application's byte order (the 68k's:
 * the data are typeless until a draw says how to read them). A draw that
 * sources an attribute from a buffer gets a GPU copy of the whole buffer
 * read with that attribute's layout (offset, size, type, stride), converted
 * once (byte-swapped, tightly packed at a 4-byte stride, as client arrays
 * are) and kept, up to OGLES2_BUFFER_COPIES layouts per buffer. A change of
 * part of the data (glBufferSubData) re-converts only the elements it
 * touches in each copy the open job does not use; a copy the open job uses
 * is retired with the job instead (renaming, plan 7.3), so nothing waits.
 * glBufferData and glUnmapBufferOES (OES_mapbuffer: the mapping is the
 * shadow itself) start every copy afresh. A buffer given as GL_DYNAMIC_DRAW
 * or GL_STREAM_DRAW has no copies: each draw converts the vertices it reads
 * from the shadow, as a client array's (ogles2_buffer_stream).
 * Element buffers are read from the shadow: their indices are copied per
 * draw, as client indices are.
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_hw.h"

#define OGLES2_BUFFER_COPIES 8

/* OES_mapbuffer (gl2ext.h) */
#ifndef GL_WRITE_ONLY_OES
#define GL_WRITE_ONLY_OES         0x88B9
#define GL_BUFFER_ACCESS_OES      0x88BB
#define GL_BUFFER_MAPPED_OES      0x88BC
#define GL_BUFFER_MAP_POINTER_OES 0x88BD
#endif

struct gpu_copy
{
    v3d_mem mem;
    ULONG serial;            /* the data serial it was made from; 0: empty */
    ULONG offset;            /* of element 0: the arrays' offset modulo their stride */
    GLint size;
    GLenum type;
    GLsizei stride;
    ULONG vertices;          /* how many it holds */
    ULONG dst_stride;
    ULONG used_job;          /* the job that last drew from it */
    BOOL normalized;
    ULONG elem_bytes;        /* one element as the application stores it */
    void (*convert)(UBYTE *dst, ULONG dst_stride, const UBYTE *src, ULONG src_stride, ULONG count,
                    GLint size, GLenum type); /* draw.c's, for partial updates */
};

struct OGLES2Buffer
{
    GLuint name;
    GLenum usage;
    ULONG size;
    UBYTE *data;             /* shadow, AllocVec'd; NULL while size is 0 */
    ULONG serial;            /* bumped on every change of the data */
    BOOL bound_once;         /* a buffer object exists once bound (GLES 2.0 2.9) */
    BOOL mapped;             /* OES_mapbuffer: glMapBufferOES handed out the shadow */
    ULONG map_offset, map_length; /* the mapping (ES 3.0's glMapBufferRange; OES's: all of it) */
    GLbitfield map_access;   /* its GL_MAP_* bits (OES's: GL_MAP_WRITE_BIT) */
    struct gpu_copy copy[OGLES2_BUFFER_COPIES];
    /* The data as the GPU itself reads them (docs/GLES3_PLAN.md §5 item 1:
     * a uniform buffer; later transform feedback and storage): each 32-bit
     * word in the GPU's byte order (ogles2_buffer_words). */
    v3d_mem words;
    ULONG words_bytes;       /* its allocation */
    ULONG words_serial;      /* the data serial it holds; 0: none */
    ULONG words_job;         /* the job that last read it */
    /* Deleted while a vertex array object not bound uses it (ES 3.0
     * appendix D.1.2): out of the name table, on the share group's list of
     * them, freed when nothing uses it any more (ogles2_buffer_release). */
    BOOL deleted;
    struct OGLES2Buffer *next_orphan;
};

#define MAX_UNIFORM_BINDINGS 108 /* GL_MAX_UNIFORM_BUFFER_BINDINGS (state.c) */
#define MAX_XFB_BINDINGS     4   /* GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS */

/* An indexed binding (glBindBufferBase, glBindBufferRange); size 0: the
 * whole buffer (glBindBufferBase). */
struct buf_range
{
    struct OGLES2Buffer *b;
    ULONG offset, size;
};

struct OGLES2BufState
{
    struct OGLES2Share *sh;               /* the buffers: sh->buf, sh->nbuf (share.c) */
    struct OGLES2Buffer *array, *element; /* GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER */
    /* ES 3.0's targets (docs/GLES3_PLAN.md §5 item 2): bindings the data
     * calls take; the pixel buffers are read and written (texture.c,
     * frame.c), the uniform and transform feedback bindings held for
     * items 6 and stage C. */
    struct OGLES2Buffer *copy_read, *copy_write, *pack, *unpack, *xfb, *uniform;
    struct buf_range ubo[MAX_UNIFORM_BINDINGS], xfb_range[MAX_XFB_BINDINGS];
};

static struct OGLES2BufState *bs_get(struct OGLES2Context *ctx)
{
    if (!ctx->buf && (ctx->buf = AllocVec(sizeof(struct OGLES2BufState), MEMF_ANY | MEMF_CLEAR)))
        ctx->buf->sh = ctx->share;
    return ctx->buf;
}

/* Frees a GPU copy now, or after the job that reads it: an open one, or one
 * still running, of any member of the share group. */
static void drop_copy(struct OGLES2Context *ctx, struct gpu_copy *c)
{
    if (!c->mem.hostptr)
        return;
    ogles2_share_defer_free(ctx, c->used_job, &c->mem);
    memset(c, 0, sizeof(*c));
}

static void drop_words(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    if (!b->words.hostptr)
        return;
    ogles2_share_defer_free(ctx, b->words_job, &b->words);
    memset(&b->words, 0, sizeof(b->words));
    b->words_bytes = b->words_serial = b->words_job = 0;
}

static void drop_copies(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    int k;
    for (k = 0; k < OGLES2_BUFFER_COPIES; k++)
        drop_copy(ctx, &b->copy[k]);
    drop_words(ctx, b);
}

static void free_buffer(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    drop_copies(ctx, b);
    if (b->data)
        FreeVec(b->data);
    FreeVec(b);
}

/* aglDestroyContext (its jobs finished): the bindings; the buffers with the
 * share group's last member. */
void ogles2_buffers_free(struct OGLES2Context *ctx)
{
    struct OGLES2BufState *bs = ctx->buf;
    struct OGLES2Share *sh = ctx->share;
    GLuint i;
    if (sh && sh->members == 1) {
        for (i = 1; i < sh->nbuf; i++)
            if (sh->buf[i])
                free_buffer(ctx, sh->buf[i]);
        if (sh->buf)
            FreeVec(sh->buf);
        sh->buf = 0;
        sh->nbuf = 0;
        ogles2_names_free(&sh->far_buf);
        while (sh->orphans) {
            struct OGLES2Buffer *b = sh->orphans;
            sh->orphans = b->next_orphan;
            free_buffer(ctx, b);
        }
    }
    if (!bs)
        return;
    FreeVec(bs);
    ctx->buf = 0;
}

/* ---- names and binding ---- */

/* The binding of `target` (ES 3.0's in ES3 contexts only); NULL: none. */
static struct OGLES2Buffer **slot(struct OGLES2Context *ctx, struct OGLES2BufState *bs, GLenum target)
{
    if (target == GL_ARRAY_BUFFER)
        return &bs->array;
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return &bs->element;
    if (!ogles2_es3(ctx))
        return 0;
    switch (target) {
    case GL_COPY_READ_BUFFER:         return &bs->copy_read;
    case GL_COPY_WRITE_BUFFER:        return &bs->copy_write;
    case GL_PIXEL_PACK_BUFFER:        return &bs->pack;
    case GL_PIXEL_UNPACK_BUFFER:      return &bs->unpack;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return &bs->xfb;
    case GL_UNIFORM_BUFFER:           return &bs->uniform;
    default:                          return 0;
    }
}

/* GLES2's three usages; ES 3.0's _READ and _COPY ones too. */
static BOOL valid_usage(struct OGLES2Context *ctx, GLenum usage)
{
    if (usage == GL_STREAM_DRAW || usage == GL_STATIC_DRAW || usage == GL_DYNAMIC_DRAW)
        return TRUE;
    return ogles2_es3(ctx) && (usage == GL_STREAM_READ || usage == GL_STREAM_COPY || usage == GL_STATIC_READ ||
                               usage == GL_STATIC_COPY || usage == GL_DYNAMIC_READ || usage == GL_DYNAMIC_COPY);
}

/* Unbinds b from every binding of a context's (glDeleteBuffers). */
static void unbind_all(struct OGLES2BufState *bs, struct OGLES2Buffer *b)
{
    struct OGLES2Buffer **one[8];
    int i;
    one[0] = &bs->array;
    one[1] = &bs->element;
    one[2] = &bs->copy_read;
    one[3] = &bs->copy_write;
    one[4] = &bs->pack;
    one[5] = &bs->unpack;
    one[6] = &bs->xfb;
    one[7] = &bs->uniform;
    for (i = 0; i < 8; i++)
        if (*one[i] == b)
            *one[i] = 0;
    for (i = 0; i < MAX_UNIFORM_BINDINGS; i++)
        if (bs->ubo[i].b == b)
            memset(&bs->ubo[i], 0, sizeof(bs->ubo[i]));
    for (i = 0; i < MAX_XFB_BINDINGS; i++)
        if (bs->xfb_range[i].b == b)
            memset(&bs->xfb_range[i], 0, sizeof(bs->xfb_range[i]));
}

/* Whether a member of ctx's share group uses b: its attribute arrays and
 * element binding, or one of its vertex array objects not bound. */
static BOOL in_use(struct OGLES2Context *ctx, const struct OGLES2Buffer *b)
{
    struct OGLES2Context *m;
    int a;
    for (m = ctx->share->first; m; m = m->share_next) {
        if (m->buf && m->buf->element == b)
            return TRUE;
        for (a = 0; a < OGLES2_MAX_VERTEX_ATTRIBS; a++)
            if (m->array[a].buffer == b)
                return TRUE;
        if (ogles2_vao_uses_buffer(m, b))
            return TRUE;
    }
    return FALSE;
}

/* A reference to b dropped: a deleted buffer nothing uses any more is freed. */
void ogles2_buffer_release(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    struct OGLES2Buffer **at;
    if (!b || !b->deleted || in_use(ctx, b))
        return;
    for (at = &ctx->share->orphans; *at; at = &(*at)->next_orphan)
        if (*at == b) {
            *at = b->next_orphan;
            break;
        }
    free_buffer(ctx, b);
}

static BOOL table_room(struct OGLES2BufState *bs, GLuint name)
{
    GLuint nn, j;
    struct OGLES2Buffer **tab;
    if (name < bs->sh->nbuf)
        return TRUE;
    nn = bs->sh->nbuf ? bs->sh->nbuf : 32;
    while (nn <= name)
        nn *= 2;
    if (!(tab = AllocVec(nn * sizeof(*tab), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (j = 0; j < bs->sh->nbuf; j++)
        tab[j] = bs->sh->buf[j];
    if (bs->sh->buf)
        FreeVec(bs->sh->buf);
    bs->sh->buf = tab;
    bs->sh->nbuf = nn;
    return TRUE;
}

static struct OGLES2Buffer *create(struct OGLES2BufState *bs, GLuint name)
{
    struct OGLES2Buffer *b;
    GLuint slot = ogles2_name_slot(&bs->sh->far_buf, name, TRUE);
    if (slot == OGLES2_NO_SLOT || !table_room(bs, slot) ||
        !(b = AllocVec(sizeof(*b), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_name_forget(&bs->sh->far_buf, name);
        return 0;
    }
    b->name = name;
    b->usage = GL_STATIC_DRAW;
    bs->sh->buf[slot] = b;
    return b;
}

static struct OGLES2Buffer *by_name(struct OGLES2BufState *bs, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&bs->sh->far_buf, name, FALSE) : OGLES2_NO_SLOT;
    return slot < bs->sh->nbuf ? bs->sh->buf[slot] : 0;
}

void ogles2_glGenBuffers(struct OGLES2Context *ctx, GLsizei n, GLuint *buffers)
{
    struct OGLES2BufState *bs;
    GLsizei k;
    GLuint i = 1;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(bs = bs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < n; k++) {
        while (i < bs->sh->nbuf && bs->sh->buf[i])
            i++;
        if (i >= OGLES2_NEAR_NAMES || !create(bs, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (buffers)
            buffers[k] = i;
    }
}

void ogles2_glDeleteBuffers(struct OGLES2Context *ctx, GLsizei n, const GLuint *buffers)
{
    if (ogles2_context_valid(ctx))
        ogles2_trace(ctx, "glDeleteBuffers", n, n > 0 && buffers ? buffers[0] : 0);
    struct OGLES2BufState *bs;
    struct OGLES2Context *m;
    GLsizei k;
    int a;
    if (!ogles2_context_valid(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(bs = bs_get(ctx)) || !buffers)
        return;
    for (k = 0; k < n; k++) {
        struct OGLES2Buffer *b = by_name(bs, buffers[k]);
        if (!b)
            continue;
        /* GLES 2.0 2.9: a deleted buffer is unbound wherever it is bound
         * in the context, attribute arrays included; here in every member
         * of the share group (GL would keep it while bound elsewhere).
         * ES 3.0 2.9.1: only the bound vertex array object's; the others
         * keep it (below). */
        for (m = ctx->share->first; m; m = m->share_next) {
            struct OGLES2BufState *mbs = m->buf;
            if (mbs)
                unbind_all(mbs, b);
            for (a = 0; a < OGLES2_MAX_VERTEX_ATTRIBS; a++)
                if (m->array[a].buffer == b) {
                    m->array[a].buffer = 0;
                    m->array[a].pointer = 0;
                }
        }
        bs->sh->buf[ogles2_name_slot(&bs->sh->far_buf, b->name, FALSE)] = 0;
        ogles2_name_forget(&bs->sh->far_buf, b->name);
        if (in_use(ctx, b)) {
            b->deleted = TRUE;
            b->next_orphan = bs->sh->orphans;
            bs->sh->orphans = b;
        } else
            free_buffer(ctx, b);
    }
}

GLboolean ogles2_glIsBuffer(struct OGLES2Context *ctx, GLuint buffer)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return GL_FALSE;
    b = by_name(bs, buffer);
    return b && b->bound_once ? GL_TRUE : GL_FALSE;
}

void ogles2_glBindBuffer(struct OGLES2Context *ctx, GLenum target, GLuint buffer)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b = 0, **at;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!(at = slot(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (buffer) {
        b = by_name(bs, buffer);
        /* GLES2 lets a program bind a name glGenBuffers did not return. */
        if (!b && !(b = create(bs, buffer))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        b->bound_once = TRUE;
    }
    if (*at != b) {
        struct OGLES2Buffer *old = *at;
        *at = b;
        ogles2_buffer_release(ctx, old); /* a vertex array object's deleted element buffer */
    }
}

static struct OGLES2Buffer *bound(struct OGLES2Context *ctx, struct OGLES2BufState *bs, GLenum target)
{
    struct OGLES2Buffer **at = slot(ctx, bs, target);
    if (at)
        return *at;
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

static void buffer_data(struct OGLES2Context *ctx, GLenum target, GLsizeiptr size, const void *data,
                         GLenum usage)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    UBYTE *shadow = 0;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!valid_usage(ctx, usage)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (size < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* The same size again (a program orphaning a buffer it streams through,
     * ioquake3's renderergl2 at every draw): the shadow is kept. Only the CPU
     * reads it (the GPU reads copies, and draws copy what they read), so
     * nothing waits; GL leaves the contents undefined without data. */
    if (size && b->data && (ULONG)size == b->size && !b->mapped) {
        if (data)
            memcpy(b->data, data, (ULONG)size);
        drop_copies(ctx, b);
        b->usage = usage;
        b->serial++;
        return;
    }
    if (size && !(shadow = AllocVec((ULONG)size, MEMF_ANY | MEMF_CLEAR))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (shadow && data)
        memcpy(shadow, data, (ULONG)size);
    drop_copies(ctx, b);
    if (b->data)
        FreeVec(b->data);
    b->data = shadow;
    b->size = (ULONG)size;
    b->usage = usage;
    b->mapped = FALSE; /* new storage: an old mapping is gone */
    b->serial++;
}

/* Timed for probe P (frame.c). */
void ogles2_glBufferData(struct OGLES2Context *ctx, GLenum target, GLsizeiptr size, const void *data,
                         GLenum usage)
{
    ULONG t0 = ogles2_clock_us();
    buffer_data(ctx, target, size, data, usage);
    if (ogles2_context_valid(ctx))
        ctx->sum_bufdata += (double)(ULONG)(ogles2_clock_us() - t0);
}

/* Words [from / 4, (to + 3) / 4) of a buffer's `size` bytes at src into the
 * GPU's byte order at dst (a partial last word padded with zeros). */
static void swap_words(ULONG *dst, const UBYTE *src, ULONG size, ULONG from, ULONG to)
{
    ULONG i, end = (to > size ? size : to) / 4;
    for (i = from / 4; i < end; i++)
        dst[i] = LE32(((const ULONG *)src)[i]);
    if (to >= size && (size & 3)) {
        UBYTE w[4] = { 0, 0, 0, 0 };
        memcpy(w, src + (size & ~3UL), size & 3);
        dst[size / 4] = LE32(*(const ULONG *)w);
    }
}

/* Bytes [off, off+len) of the shadow changed (plan 7.5): each copy the open
 * job does not read re-converts the elements those bytes touch, in place;
 * a copy the open job reads is retired with it and rebuilt at its next use.
 * The data serial has been bumped already. */
static void patch_copies(struct OGLES2Context *ctx, struct OGLES2Buffer *b, ULONG off, ULONG len)
{
    int k;
    for (k = 0; k < OGLES2_BUFFER_COPIES; k++) {
        struct gpu_copy *c = &b->copy[k];
        ULONG stride, i0, i1, n;
        if (!c->mem.hostptr)
            continue;
        if (c->serial != b->serial - 1 || !c->convert ||
            (c->used_job && ogles2_job_uses(ctx, c->used_job))) {
            drop_copy(ctx, c);
            continue;
        }
        c->serial = b->serial;
        stride = c->stride ? (ULONG)c->stride : c->elem_bytes;
        /* Element i spans [c->offset + i*stride, + elem_bytes). */
        if (off + len <= c->offset)
            continue;
        i0 = off + 1 > c->offset + c->elem_bytes ? (off + 1 - c->offset - c->elem_bytes + stride - 1) / stride : 0;
        i1 = (off + len - 1 - c->offset) / stride;
        if (i1 >= c->vertices)
            i1 = c->vertices - 1;
        if (i0 > i1)
            continue;
        n = i1 - i0 + 1;
        /* No cache call: the next flush cleans the whole data cache before
         * the GPU reads (job.c, clean_all). */
        c->convert((UBYTE *)c->mem.hostptr + i0 * c->dst_stride, c->dst_stride,
                   b->data + c->offset + i0 * stride, stride, n, c->size, c->type);
    }
    /* The words the GPU reads: those the bytes touch swapped again, unless
     * a job not yet rendered reads them (then made anew at the next use). */
    if (b->words.hostptr && b->words_serial == b->serial - 1 &&
        !(b->words_job && ogles2_job_uses(ctx, b->words_job))) {
        swap_words((ULONG *)b->words.hostptr, b->data, b->size, off & ~3UL, off + len);
        b->words_serial = b->serial;
    }
}

static void buffer_sub_data(struct OGLES2Context *ctx, GLenum target, GLintptr offset, GLsizeiptr size,
                            const void *data)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || size < 0 || (ULONG)offset + (ULONG)size > b->size) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (b->mapped && ogles2_es3(ctx)) { /* ES 3.0 2.10.2 */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!size || !data)
        return;
    memcpy(b->data + offset, data, (ULONG)size);
    b->serial++;
    patch_copies(ctx, b, (ULONG)offset, (ULONG)size);
}

/* Timed for probe P (frame.c). */
void ogles2_glBufferSubData(struct OGLES2Context *ctx, GLenum target, GLintptr offset, GLsizeiptr size,
                            const void *data)
{
    ULONG t0 = ogles2_clock_us();
    buffer_sub_data(ctx, target, offset, size, data);
    if (ogles2_context_valid(ctx))
        ctx->sum_bufdata += (double)(ULONG)(ogles2_clock_us() - t0);
}

void ogles2_glGetBufferParameteriv(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                   GLint *params)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !params || !(bs = bs_get(ctx)))
        return;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (pname == GL_BUFFER_SIZE)
        *params = (GLint)b->size;
    else if (pname == GL_BUFFER_USAGE)
        *params = (GLint)b->usage;
    else if (pname == GL_BUFFER_ACCESS_OES && !ogles2_es3(ctx)) /* not ES 3.0's */
        *params = GL_WRITE_ONLY_OES;
    else if (pname == GL_BUFFER_MAPPED_OES)
        *params = b->mapped ? GL_TRUE : GL_FALSE;
    else if (pname == GL_BUFFER_ACCESS_FLAGS && ogles2_es3(ctx))
        *params = b->mapped ? (GLint)b->map_access : 0;
    else if (pname == GL_BUFFER_MAP_LENGTH && ogles2_es3(ctx))
        *params = b->mapped ? (GLint)b->map_length : 0;
    else if (pname == GL_BUFFER_MAP_OFFSET && ogles2_es3(ctx))
        *params = b->mapped ? (GLint)b->map_offset : 0;
    else
        ogles2_error(ctx, GL_INVALID_ENUM);
}

void ogles2_glGetBufferParameteri64v(struct OGLES2Context *ctx, GLenum target, GLenum pname, GLint64 *params)
{
    GLint v = 0;
    if (!ogles2_es3_entry(ctx) || !params)
        return;
    ogles2_glGetBufferParameteriv(ctx, target, pname, &v);
    *params = pname == GL_BUFFER_ACCESS_FLAGS ? (GLint64)(ULONG)v : (GLint64)v;
}

/* AmigaOS 4's name for the same query (its OES_mapbuffer). */
void ogles2_glGetBufferParameterivOES(struct OGLES2Context *ctx, GLenum target, GLenum pname,
                                      GLint *params)
{
    ogles2_glGetBufferParameteriv(ctx, target, pname, params);
}

/* ---- OES_mapbuffer: the mapping is the shadow ---- */

void *ogles2_glMapBufferOES(struct OGLES2Context *ctx, GLenum target, GLenum access)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return 0;
    if (!slot(ctx, bs, target) || access != GL_WRITE_ONLY_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    if (!(b = bound(ctx, bs, target)) || b->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    b->mapped = TRUE;
    b->map_offset = 0;
    b->map_length = b->size;
    b->map_access = GL_MAP_WRITE_BIT;
    return b->data;
}

GLboolean ogles2_glUnmapBufferOES(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return GL_FALSE;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    if (!(b = bound(ctx, bs, target)) || !b->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    b->mapped = FALSE;
    /* Anything may have changed: every copy starts afresh. */
    drop_copies(ctx, b);
    b->serial++;
    return GL_TRUE;
}

/* ES 3.0 2.10.3. The mapping is the shadow, as OES_mapbuffer's: a draw
 * reads its vertices and indices from the shadow when it is made or from
 * copies made earlier, so nothing waits for the GPU; what the application
 * wrote goes into the copies when it is unmapped (or flushed, with
 * GL_MAP_FLUSH_EXPLICIT_BIT). Unsynchronized and invalidating mappings are
 * the same. */
void *ogles2_glMapBufferRange(struct OGLES2Context *ctx, GLenum target, GLintptr offset, GLsizeiptr length,
                              GLbitfield access)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_es3_entry(ctx) || !(bs = bs_get(ctx)))
        return 0;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    /* Mesa's order of the checks (bufferobj.c, validate_map_buffer_range) */
    if (offset < 0 || length < 0 ||
        (access & ~(GLbitfield)(GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    if (!length || !(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) ||
        ((access & GL_MAP_READ_BIT) &&
         (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) ||
        ((access & GL_MAP_FLUSH_EXPLICIT_BIT) && !(access & GL_MAP_WRITE_BIT))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    if ((ULONG)offset + (ULONG)length > b->size) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    if (b->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    b->mapped = TRUE;
    b->map_offset = (ULONG)offset;
    b->map_length = (ULONG)length;
    b->map_access = access;
    return b->data + offset;
}

void ogles2_glFlushMappedBufferRange(struct OGLES2Context *ctx, GLenum target, GLintptr offset, GLsizeiptr length)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_es3_entry(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target)) || !b->mapped || !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || length < 0 || (ULONG)offset + (ULONG)length > b->map_length) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!length)
        return;
    b->serial++;
    patch_copies(ctx, b, b->map_offset + (ULONG)offset, (ULONG)length);
}

/* ES 3.0's name; OES_mapbuffer's mapping too. */
GLboolean ogles2_glUnmapBuffer(struct OGLES2Context *ctx, GLenum target)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_es3_entry(ctx) || !(bs = bs_get(ctx)))
        return GL_FALSE;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    if (!(b = bound(ctx, bs, target)) || !b->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    b->mapped = FALSE;
    /* what was written, unless flushed already */
    if ((b->map_access & GL_MAP_WRITE_BIT) && !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT) && b->map_length) {
        b->serial++;
        patch_copies(ctx, b, b->map_offset, b->map_length);
    }
    return GL_TRUE;
}

void ogles2_glGetBufferPointerv(struct OGLES2Context *ctx, GLenum target, GLenum pname, void **params)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glGetBufferPointervOES(ctx, target, pname, params);
}

void ogles2_glGetBufferPointervOES(struct OGLES2Context *ctx, GLenum target, GLenum pname, void **params)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b;
    if (!ogles2_context_valid(ctx) || !params || !(bs = bs_get(ctx)))
        return;
    if (!slot(ctx, bs, target) || pname != GL_BUFFER_MAP_POINTER_OES) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!(b = bound(ctx, bs, target))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    *params = b->mapped ? b->data + b->map_offset : 0;
}

/* ---- for the rest of the library ---- */

BOOL ogles2_buffer_query(struct OGLES2Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    struct OGLES2Buffer **at;
    GLenum target;
    switch (pname) {
    case GL_ARRAY_BUFFER_BINDING:              target = GL_ARRAY_BUFFER; break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:      target = GL_ELEMENT_ARRAY_BUFFER; break;
    case GL_COPY_READ_BUFFER_BINDING:          target = GL_COPY_READ_BUFFER; break;
    case GL_COPY_WRITE_BUFFER_BINDING:         target = GL_COPY_WRITE_BUFFER; break;
    case GL_PIXEL_PACK_BUFFER_BINDING:         target = GL_PIXEL_PACK_BUFFER; break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING:       target = GL_PIXEL_UNPACK_BUFFER; break;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: target = GL_TRANSFORM_FEEDBACK_BUFFER; break;
    case GL_UNIFORM_BUFFER_BINDING:            target = GL_UNIFORM_BUFFER; break;
    default:                                   return FALSE;
    }
    if (!bs) {
        *v = 0;
        return TRUE;
    }
    if (!(at = slot(ctx, bs, target)))
        return FALSE; /* ES 3.0's in a GLES2 context */
    *v = *at ? (GLint)(*at)->name : 0;
    return TRUE;
}

/* ES 3.0's pixel buffers: with a buffer bound to GL_PIXEL_PACK_BUFFER
 * (`pack`) or GL_PIXEL_UNPACK_BUFFER, the address of the `bytes` at
 * `offset` in its data (an application's pixel pointer is that offset);
 * NULL with none bound, or for the library's own images (OGLES2PixelStore's
 * own). *bad (GL_INVALID_OPERATION): past its end, or it is mapped. A pack
 * buffer's data change: its GPU copies start afresh. */
UBYTE *ogles2_buffer_pixels(struct OGLES2Context *ctx, BOOL pack, ULONG offset, ULONG bytes, BOOL *bad)
{
    static UBYTE none; /* an empty buffer's address for an empty image */
    struct OGLES2BufState *bs = ctx->buf;
    struct OGLES2Buffer *b = bs ? (pack ? bs->pack : bs->unpack) : 0;
    *bad = FALSE;
    if (!b || (pack ? ctx->pack.own : ctx->unpack.own))
        return 0;
    if (b->mapped || offset + bytes < offset || offset + bytes > b->size) {
        *bad = TRUE;
        return 0;
    }
    if (!b->data)
        return &none;
    if (pack && bytes) {
        drop_copies(ctx, b);
        b->serial++;
    }
    return b->data + offset;
}

/* The buffer as the GPU reads it itself (§5 item 1): every 32-bit word in
 * the GPU's byte order (GLSL ES 3.x's blocks hold 32-bit values only),
 * padded to whole 64-byte cache lines. Made at the first such use and
 * again after a change: in place, or, while a job not yet rendered reads
 * it, a new one (the old goes with that job, renaming as for the draws'
 * copies). The data cache clean before each submission (job.c) covers it.
 * Its address; 0 without data or memory. */
ULONG ogles2_buffer_words(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    ULONG bytes = (b->size + 63) & ~63UL;
    if (!b->data || !b->size)
        return 0;
    if (b->words.hostptr && b->words_serial == b->serial) {
        b->words_job = ctx->job_serial;
        return (ULONG)b->words.hostptr;
    }
    if (b->words.hostptr && (b->words_bytes != bytes || (b->words_job && ogles2_job_uses(ctx, b->words_job))))
        drop_words(ctx, b);
    if (!b->words.hostptr) {
        if (v3d_mem_alloc(ogles2_device(), &b->words, bytes) < 0) {
            memset(&b->words, 0, sizeof(b->words));
            return 0;
        }
        b->words_bytes = bytes;
    }
    swap_words((ULONG *)b->words.hostptr, b->data, b->size, 0, b->size);
    b->words_serial = b->serial;
    b->words_job = ctx->job_serial;
    return (ULONG)b->words.hostptr;
}

/* Uniform buffer binding `index` as a draw reads it (§5 item 6): the GPU
 * address of its range's start and the range's bytes (*size); 0 with no
 * buffer there, an empty one, or a range starting past its end. */
ULONG ogles2_buffer_uniform(struct OGLES2Context *ctx, ULONG index, ULONG *size)
{
    struct OGLES2BufState *bs = ctx->buf;
    struct buf_range *r;
    ULONG a;
    if (!bs || index >= MAX_UNIFORM_BINDINGS)
        return 0;
    r = &bs->ubo[index];
    if (!r->b || r->offset >= r->b->size || !(a = ogles2_buffer_words(ctx, r->b)))
        return 0;
    *size = r->b->size - r->offset;
    if (r->size && r->size < *size)
        *size = r->size;
    return a + r->offset;
}

/* ---- ES 3.0's indexed bindings and copies ---- */

static void bind_range(struct OGLES2Context *ctx, GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                       GLsizeiptr size, BOOL ranged)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer *b = 0;
    struct buf_range *r;
    BOOL uniform = target == GL_UNIFORM_BUFFER;
    if (!ogles2_es3_entry(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!uniform && target != GL_TRANSFORM_FEEDBACK_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (index >= (uniform ? MAX_UNIFORM_BINDINGS : MAX_XFB_BINDINGS)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* Mesa's checks (bufferobj.c): a range of a buffer is not empty, and
     * starts at GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT; transform feedback's
     * at a word, a whole number of words long. */
    if (ranged && buffer && (size <= 0 || offset < 0 || (uniform ? (offset & 255) : ((offset | size) & 3)))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (buffer) {
        if (!(b = by_name(bs, buffer)) && !(b = create(bs, buffer))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        b->bound_once = TRUE;
    }
    r = uniform ? &bs->ubo[index] : &bs->xfb_range[index];
    r->b = b;
    r->offset = ranged && b ? (ULONG)offset : 0;
    r->size = ranged && b ? (ULONG)size : 0;
    /* the target's own binding too (ES 3.0 2.9.1) */
    if (uniform)
        bs->uniform = b;
    else
        bs->xfb = b;
}

void ogles2_glBindBufferRange(struct OGLES2Context *ctx, GLenum target, GLuint index, GLuint buffer,
                              GLintptr offset, GLsizeiptr size)
{
    bind_range(ctx, target, index, buffer, offset, size, TRUE);
}

void ogles2_glBindBufferBase(struct OGLES2Context *ctx, GLenum target, GLuint index, GLuint buffer)
{
    bind_range(ctx, target, index, buffer, 0, 0, FALSE);
}

/* glGetIntegeri_v and glGetInteger64i_v: the indexed state ES 3.0 has, the
 * uniform and transform feedback buffer bindings. */
static BOOL indexed(struct OGLES2Context *ctx, GLenum target, GLuint index, GLint64 *v)
{
    struct OGLES2BufState *bs;
    struct buf_range *r;
    BOOL uniform = target == GL_UNIFORM_BUFFER_BINDING || target == GL_UNIFORM_BUFFER_START ||
                   target == GL_UNIFORM_BUFFER_SIZE;
    if (!ogles2_es3_entry(ctx))
        return FALSE;
    if (!uniform && target != GL_TRANSFORM_FEEDBACK_BUFFER_BINDING && target != GL_TRANSFORM_FEEDBACK_BUFFER_START &&
        target != GL_TRANSFORM_FEEDBACK_BUFFER_SIZE) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return FALSE;
    }
    if (index >= (uniform ? MAX_UNIFORM_BINDINGS : MAX_XFB_BINDINGS)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return FALSE;
    }
    if (!(bs = bs_get(ctx))) {
        *v = 0;
        return TRUE;
    }
    r = uniform ? &bs->ubo[index] : &bs->xfb_range[index];
    *v = target == GL_UNIFORM_BUFFER_BINDING || target == GL_TRANSFORM_FEEDBACK_BUFFER_BINDING
             ? (r->b ? (GLint64)r->b->name : 0)
         : target == GL_UNIFORM_BUFFER_START || target == GL_TRANSFORM_FEEDBACK_BUFFER_START ? (GLint64)r->offset
                                                                                           : (GLint64)r->size;
    return TRUE;
}

void ogles2_glGetIntegeri_v(struct OGLES2Context *ctx, GLenum target, GLuint index, GLint *data)
{
    GLint64 v;
    if (data && indexed(ctx, target, index, &v))
        *data = (GLint)v;
}

void ogles2_glGetInteger64i_v(struct OGLES2Context *ctx, GLenum target, GLuint index, GLint64 *data)
{
    GLint64 v;
    if (data && indexed(ctx, target, index, &v))
        *data = v;
}

void ogles2_glCopyBufferSubData(struct OGLES2Context *ctx, GLenum readTarget, GLenum writeTarget,
                                GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
    struct OGLES2BufState *bs;
    struct OGLES2Buffer **rs, **ws, *r, *w;
    if (!ogles2_es3_entry(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!(rs = slot(ctx, bs, readTarget)) || !(ws = slot(ctx, bs, writeTarget))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    r = *rs;
    w = *ws;
    if (!r || !w || r->mapped || w->mapped) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* ES 3.0 2.9.5: within both, and not overlapping in one buffer */
    if (readOffset < 0 || writeOffset < 0 || size < 0 || (ULONG)readOffset + (ULONG)size > r->size ||
        (ULONG)writeOffset + (ULONG)size > w->size ||
        (r == w && readOffset < writeOffset + size && writeOffset < readOffset + size)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!size)
        return;
    memmove(w->data + writeOffset, r->data + readOffset, (ULONG)size);
    w->serial++;
    patch_copies(ctx, w, (ULONG)writeOffset, (ULONG)size);
}

struct OGLES2Buffer *ogles2_buffer_element(struct OGLES2Context *ctx)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    return bs ? bs->element : 0;
}

void ogles2_buffer_set_element(struct OGLES2Context *ctx, struct OGLES2Buffer *b)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    if (bs)
        bs->element = b;
}

/* The GL_ARRAY_BUFFER binding (glVertexAttribPointer captures it). */
struct OGLES2Buffer *ogles2_buffer_array(struct OGLES2Context *ctx)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    return bs ? bs->array : 0;
}

GLuint ogles2_buffer_name(const struct OGLES2Buffer *b)
{
    return b ? b->name : 0;
}

ULONG ogles2_buffer_size(const struct OGLES2Buffer *b)
{
    return b ? b->size : 0;
}

/* glDrawElements' indices in the GL_ELEMENT_ARRAY_BUFFER: `offset` bytes
 * into it, `bytes` long; NULL without an element buffer (the indices are
 * then the application's pointer) or with the range outside it (*bad). */
const void *ogles2_buffer_indices(struct OGLES2Context *ctx, ULONG offset, ULONG bytes, BOOL *bad)
{
    struct OGLES2BufState *bs = bs_get(ctx);
    *bad = FALSE;
    if (!bs || !bs->element)
        return 0;
    if (!bs->element->data || offset + bytes > bs->element->size || offset + bytes < offset) {
        *bad = TRUE;
        return 0;
    }
    return bs->element->data + offset;
}

/* A buffer the application changes as it draws (GL_DYNAMIC_DRAW or
 * GL_STREAM_DRAW, and ES 3.0's _READ and _COPY usages but the static ones): its shadow, which a draw converts the vertices it reads
 * from into the job's state buffer, as from a client array. NULL for a
 * GL_STATIC_DRAW buffer: draws read its kept GPU copies (ogles2_buffer_copy),
 * which would be made again, whole, after every change. */
const UBYTE *ogles2_buffer_stream(const struct OGLES2Buffer *b)
{
    return b->usage != GL_STATIC_DRAW && b->usage != GL_STATIC_READ && b->usage != GL_STATIC_COPY ? b->data : 0;
}

/* A GPU copy of buffer `b` read as an attribute array of `size` components
 * of `type` at `offset` with `stride` (0: packed): converted with
 * `convert` (draw.c's, as for client arrays) into tightly packed elements.
 * Returns its GPU address of vertex 0 and its stride; 0 if out of memory or
 * the buffer holds no complete element at `offset`. *vertices: how many. */
ULONG ogles2_buffer_copy(struct OGLES2Context *ctx, struct OGLES2Buffer *b, ULONG offset, GLint size,
                         GLenum type, GLboolean normalized, GLsizei stride, ULONG elem_bytes, ULONG out_stride,
                         ULONG *dst_stride, ULONG *vertices,
                         void (*convert)(UBYTE *dst, ULONG dst_stride, const UBYTE *src,
                                         ULONG src_stride, ULONG count, GLint size, GLenum type))
{
    struct gpu_copy *c, *victim = 0;
    ULONG src_stride = stride ? (ULONG)stride : elem_bytes, n;
    /* A copy holds every element of the buffer at the same place within a
     * vertex (offset modulo stride), from the first: arrays that differ only
     * by whole vertices share it (ioquake3's renderergl2 keeps all of a
     * model's animation frames in one buffer and moves the offset a frame
     * at a time). The array's vertex 0 is element `skip` of the copy. */
    ULONG phase = offset % src_stride, skip = offset / src_stride;
    int k;
    for (k = 0; k < OGLES2_BUFFER_COPIES; k++) {
        c = &b->copy[k];
        if (c->serial == b->serial && c->mem.hostptr && c->offset == phase && c->size == size &&
            c->type == type && c->stride == stride && c->normalized == normalized) {
            if (skip >= c->vertices)
                return 0;
            c->used_job = ctx->job_serial;
            *dst_stride = c->dst_stride;
            *vertices = c->vertices - skip;
            return (ULONG)c->mem.hostptr + skip * c->dst_stride;
        }
        if (!c->mem.hostptr && !victim)
            victim = c;
    }
    if (!victim) { /* all in use for other layouts: replace the first */
        victim = &b->copy[0];
        drop_copy(ctx, victim);
    }
    if (!b->data || offset + elem_bytes > b->size)
        return 0;
    n = (b->size - phase - elem_bytes) / src_stride + 1;
    c = victim;
    c->dst_stride = out_stride; /* the converted element's, a whole number of words */
    if (v3d_mem_alloc(ogles2_device(), &c->mem, c->dst_stride * n) < 0) {
        memset(c, 0, sizeof(*c));
        return 0;
    }
    /* The job that reads it is flushed after a clean of the whole data
     * cache (job.c, clean_all): no cache call here. */
    convert((UBYTE *)c->mem.hostptr, c->dst_stride, b->data + phase, src_stride, n, size, type);
    c->serial = b->serial;
    c->elem_bytes = elem_bytes;
    c->convert = convert;
    c->offset = phase;
    c->size = size;
    c->type = type;
    c->stride = stride;
    c->normalized = normalized;
    c->vertices = n;
    c->used_job = ctx->job_serial;
    *dst_stride = c->dst_stride;
    *vertices = n - skip;
    return (ULONG)c->mem.hostptr + skip * c->dst_stride;
}
