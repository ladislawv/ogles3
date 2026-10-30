/*
 * Buffer objects (plan section 7.5): glGenBuffers, glBindBuffer,
 * glBufferData, glBufferSubData, and the GPU copies the draw path reads.
 *
 * A buffer keeps a CPU shadow in the application's byte order (the 68k's:
 * the data are typeless until a draw says how to read them). A draw that
 * sources an attribute from a buffer gets a GPU copy of the whole buffer
 * read with that attribute's layout (offset, size, type, stride), converted
 * once (byte-swapped, tightly packed at a 4-byte stride, as client arrays
 * are) and kept, up to OGLES3_BUFFER_COPIES layouts per buffer. A change of
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
#include <stddef.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_frame.h"
#include "v3d_hw.h"

#define OGLES3_BUFFER_COPIES 8

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

struct OGLES3Buffer
{
    GLuint name;
    GLenum usage;
    ULONG size;
    UBYTE *data;             /* shadow, AllocVec'd; NULL while size is 0 */
    ULONG serial;            /* bumped on every change of the data */
    ULONG uid;               /* unique to it among the library's buffers (ogles2_buffer_serial) */
    /* Deleted while a vertex array object not bound uses it (ES 3.0
     * appendix D.1.2): out of the name table, on the share group's list of
     * them, freed when nothing uses it any more (ogles2_buffer_release). */
    BOOL deleted;
    BOOL copied;             /* a GPU copy was made once (ogles2_buffer_copy): patch_copies has work */
    BOOL bound_once;         /* a buffer object exists once bound (GLES 2.0 2.9) */
    BOOL mapped;             /* OES_mapbuffer: glMapBufferOES handed out the shadow */
    ULONG map_offset, map_length; /* the mapping (ES 3.0's glMapBufferRange; OES's: all of it) */
    GLbitfield map_access;   /* its GL_MAP_* bits (OES's: GL_MAP_WRITE_BIT) */
    struct gpu_copy copy[OGLES3_BUFFER_COPIES];
    /* The data as the GPU itself reads them (docs/GLES3_PLAN.md §5 item 1:
     * a uniform buffer; later transform feedback and storage): each 32-bit
     * word in the GPU's byte order (ogles2_buffer_words). */
    v3d_mem words;
    ULONG words_bytes;       /* its allocation */
    ULONG words_serial;      /* the data serial it holds; 0: none */
    ULONG words_job;         /* the job that last read it */
    ULONG xfb_job;           /* a job that writes it (transform feedback), not known rendered: the
                              * shadow is stale until gpu_written (0: none) */
    BOOL cs_written;         /* a compute dispatch wrote its words (finished): the shadow is stale
                              * until gpu_written (ES 3.1 storage buffers, compute.c) */
    ULONG sto_job;           /* a job whose draws write it as a storage buffer (their fragment
                              * shaders run at its render): no draw of that job reads its words
                              * directly (ogles2_buffer_gpu_words). 0: none */
    struct OGLES3Buffer *next_orphan;
};

#define MAX_UNIFORM_BINDINGS 108 /* GL_MAX_UNIFORM_BUFFER_BINDINGS (state.c) */
#define MAX_SSBO_BINDINGS    32  /* GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS (ES 3.1) */
#define MAX_ATOMIC_BINDINGS  16  /* GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS (ES 3.1) */

/* An indexed binding (glBindBufferBase, glBindBufferRange); size 0: the
 * whole buffer (glBindBufferBase). */
struct buf_range
{
    struct OGLES3Buffer *b;
    ULONG offset, size;
};

struct OGLES3BufState
{
    struct OGLES3Share *sh;               /* the buffers: sh->buf, sh->nbuf (share.c) */
    struct OGLES3Buffer *array, *element; /* GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER */
    /* ES 3.0's targets (docs/GLES3_PLAN.md §5 item 2): bindings the data
     * calls take; the pixel buffers are read and written (texture.c,
     * frame.c), the uniform and transform feedback bindings held for
     * items 6 and stage C. */
    struct OGLES3Buffer *copy_read, *copy_write, *pack, *unpack, *xfb, *uniform;
    struct buf_range ubo[MAX_UNIFORM_BINDINGS]; /* (transform feedback's: xfb.c's objects) */
    /* ES 3.1's (docs/GLES3_PLAN.md §7): the indirect draw and dispatch
     * commands' buffers (items 1 and 3), the storage and atomic counter
     * buffers and their indexed bindings (item 2). */
    struct OGLES3Buffer *draw_indirect, *dispatch_indirect, *storage, *atomic;
    struct buf_range ssbo[MAX_SSBO_BINDINGS], acbo[MAX_ATOMIC_BINDINGS];
};

static struct OGLES3BufState *bs_get(struct OGLES3Context *ctx)
{
    if (!ctx->buf && (ctx->buf = AllocVec(sizeof(struct OGLES3BufState), MEMF_ANY | MEMF_CLEAR)))
        ctx->buf->sh = ctx->share;
    return ctx->buf;
}

/* Frees a GPU copy now, or after the job that reads it: an open one, or one
 * still running, of any member of the share group. */
static void drop_copy(struct OGLES3Context *ctx, struct gpu_copy *c)
{
    if (!c->mem.hostptr)
        return;
    ogles2_share_defer_free(ctx, c->used_job, &c->mem);
    memset(c, 0, sizeof(*c));
}

static void drop_words(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    if (!b->words.hostptr)
        return;
    ogles2_share_defer_free(ctx, b->words_job, &b->words);
    memset(&b->words, 0, sizeof(b->words));
    b->words_bytes = b->words_serial = b->words_job = 0;
}

/* The CPU reads or writes b next: if a job not known rendered writes its
 * words (transform feedback), that job is rendered and waited for, and the
 * words are copied back into the shadow (docs/GLES3_PLAN.md §5 item 1).
 * Never while a draw builds its job: draws call it before (arrays_ok,
 * ogles2_buffer_indices). The test inline, the work in gpu_written_now:
 * draws ask for every buffer they read, and almost none has work. */
static void gpu_written_now(struct OGLES3Context *ctx, struct OGLES3Buffer *b);

static inline void gpu_written(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    if (b && (b->xfb_job || b->cs_written))
        gpu_written_now(ctx, b);
}

static __attribute__((noinline)) void gpu_written_now(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    struct OGLES3Context *user;
    ULONG i, n, len;
    if (!b || (!b->xfb_job && !b->cs_written))
        return;
    if (b->xfb_job && (user = ogles2_job_user(ctx, b->xfb_job)) != 0 && user->v3d) {
        ogles2_job_flush(user, FALSE);
        ogles2_job_finish(user);
    }
    b->xfb_job = 0;
    b->cs_written = FALSE;
    b->sto_job = 0;
    if (!b->words.hostptr || !b->data || b->words_serial != b->serial)
        return;
    len = b->words_bytes;
    CachePostDMA(b->words.hostptr, &len, 0);
    n = b->size / 4;
    for (i = 0; i < n; i++)
        ((ULONG *)b->data)[i] = LE32(((const ULONG *)b->words.hostptr)[i]);
    if (b->size & 3) {
        ULONG w = LE32(((const ULONG *)b->words.hostptr)[n]);
        memcpy(b->data + 4 * n, &w, b->size & 3);
    }
    b->serial++;
    b->words_serial = b->serial; /* the words are the data still */
}

/* A buffer was ever given to the GPU to write (transform feedback, a
 * storage block, an atomic counter): until then no draw needs to look at
 * its arrays' buffers for words to copy back (draw.c, arrays_ok). */
BOOL ogles2_gpu_writes_seen;

void ogles2_buffer_cpu(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    gpu_written(ctx, b);
}

static void drop_copies(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    int k;
    for (k = 0; k < OGLES3_BUFFER_COPIES; k++)
        drop_copy(ctx, &b->copy[k]);
    drop_words(ctx, b);
}

static void free_buffer(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    drop_copies(ctx, b);
    if (b->data)
        FreeVec(b->data);
    FreeVec(b);
}

/* aglDestroyContext (its jobs finished): the bindings; the buffers with the
 * share group's last member. */
void ogles2_buffers_free(struct OGLES3Context *ctx)
{
    struct OGLES3BufState *bs = ctx->buf;
    struct OGLES3Share *sh = ctx->share;
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
            struct OGLES3Buffer *b = sh->orphans;
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
static __attribute__((noinline)) struct OGLES3Buffer **slot_es3(struct OGLES3Context *ctx, struct OGLES3BufState *bs,
                                                                 GLenum target);

/* GLES2's two targets inline, ES 3's in slot_es3 (buffer updates and
 * binds ask for every call; tests/perf/drawbench). */
static inline struct OGLES3Buffer **slot(struct OGLES3Context *ctx, struct OGLES3BufState *bs, GLenum target)
{
    if (target == GL_ARRAY_BUFFER)
        return &bs->array;
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return &bs->element;
    return slot_es3(ctx, bs, target);
}

static struct OGLES3Buffer **slot_es3(struct OGLES3Context *ctx, struct OGLES3BufState *bs, GLenum target)
{
    if (!ogles2_es3(ctx))
        return 0;
    switch (target) {
    case GL_COPY_READ_BUFFER:         return &bs->copy_read;
    case GL_COPY_WRITE_BUFFER:        return &bs->copy_write;
    case GL_PIXEL_PACK_BUFFER:        return &bs->pack;
    case GL_PIXEL_UNPACK_BUFFER:      return &bs->unpack;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return &bs->xfb;
    case GL_UNIFORM_BUFFER:           return &bs->uniform;
    }
    if (!ogles2_es31(ctx))
        return 0;
    switch (target) {
    case GL_DRAW_INDIRECT_BUFFER:     return &bs->draw_indirect;
    case GL_DISPATCH_INDIRECT_BUFFER: return &bs->dispatch_indirect;
    case GL_SHADER_STORAGE_BUFFER:    return &bs->storage;
    case GL_ATOMIC_COUNTER_BUFFER:    return &bs->atomic;
    default:                          return 0;
    }
}

/* GLES2's three usages; ES 3.0's _READ and _COPY ones too. */
static BOOL valid_usage(struct OGLES3Context *ctx, GLenum usage)
{
    if (usage == GL_STREAM_DRAW || usage == GL_STATIC_DRAW || usage == GL_DYNAMIC_DRAW)
        return TRUE;
    return ogles2_es3(ctx) && (usage == GL_STREAM_READ || usage == GL_STREAM_COPY || usage == GL_STATIC_READ ||
                               usage == GL_STATIC_COPY || usage == GL_DYNAMIC_READ || usage == GL_DYNAMIC_COPY);
}

/* Unbinds b from every binding of a context's (glDeleteBuffers), the bound
 * transform feedback object's too (ES 3.0 2.9.1: not those of objects not
 * bound). */
static void unbind_all(struct OGLES3Context *ctx, struct OGLES3BufState *bs, struct OGLES3Buffer *b)
{
    struct OGLES3XfbBinding *x = ogles2_xfb_bindings(ctx);
    struct OGLES3Buffer **one[12];
    int i;
    one[0] = &bs->array;
    one[1] = &bs->element;
    one[2] = &bs->copy_read;
    one[3] = &bs->copy_write;
    one[4] = &bs->pack;
    one[5] = &bs->unpack;
    one[6] = &bs->xfb;
    one[7] = &bs->uniform;
    one[8] = &bs->draw_indirect;
    one[9] = &bs->dispatch_indirect;
    one[10] = &bs->storage;
    one[11] = &bs->atomic;
    for (i = 0; i < 12; i++)
        if (*one[i] == b)
            *one[i] = 0;
    for (i = 0; i < MAX_UNIFORM_BINDINGS; i++)
        if (bs->ubo[i].b == b)
            memset(&bs->ubo[i], 0, sizeof(bs->ubo[i]));
    for (i = 0; i < MAX_SSBO_BINDINGS; i++)
        if (bs->ssbo[i].b == b)
            memset(&bs->ssbo[i], 0, sizeof(bs->ssbo[i]));
    for (i = 0; i < MAX_ATOMIC_BINDINGS; i++)
        if (bs->acbo[i].b == b)
            memset(&bs->acbo[i], 0, sizeof(bs->acbo[i]));
    for (i = 0; x && i < OGLES3_MAX_XFB_BUFFERS; i++)
        if (x[i].b == b)
            memset(&x[i], 0, sizeof(x[i]));
}

/* Whether a member of ctx's share group uses b: its attribute arrays and
 * element binding, or one of its vertex array objects not bound. */
static BOOL in_use(struct OGLES3Context *ctx, const struct OGLES3Buffer *b)
{
    struct OGLES3Context *m;
    int a;
    for (m = ctx->share->first; m; m = m->share_next) {
        if (m->buf && m->buf->element == b)
            return TRUE;
        for (a = 0; a < OGLES3_MAX_VERTEX_ATTRIBS; a++)
            if (m->array[a].buffer == b || m->array[a].bbuffer == b) /* ES 3.1's bindings too */
                return TRUE;
        if (ogles2_vao_uses_buffer(m, b) || ogles2_xfb_uses_buffer(m, b))
            return TRUE;
    }
    return FALSE;
}

/* A reference to b dropped: a deleted buffer nothing uses any more is freed. */
void ogles2_buffer_release(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    struct OGLES3Buffer **at;
    if (!b || !b->deleted || in_use(ctx, b))
        return;
    for (at = &ctx->share->orphans; *at; at = &(*at)->next_orphan)
        if (*at == b) {
            *at = b->next_orphan;
            break;
        }
    free_buffer(ctx, b);
}

static BOOL table_room(struct OGLES3BufState *bs, GLuint name)
{
    GLuint nn, j;
    struct OGLES3Buffer **tab;
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

static ULONG next_uid;

/* The head draw.c reads inline (ogles2_internal.h) */
_Static_assert(offsetof(struct OGLES3BufState, sh) == offsetof(struct OGLES3BufStateHead, sh) &&
               offsetof(struct OGLES3BufState, array) == offsetof(struct OGLES3BufStateHead, array) &&
               offsetof(struct OGLES3BufState, element) == offsetof(struct OGLES3BufStateHead, element),
               "struct OGLES3BufState starts with struct OGLES3BufStateHead");
_Static_assert(offsetof(struct OGLES3Buffer, name) == offsetof(struct OGLES3BufferHead, name) &&
               offsetof(struct OGLES3Buffer, usage) == offsetof(struct OGLES3BufferHead, usage) &&
               offsetof(struct OGLES3Buffer, size) == offsetof(struct OGLES3BufferHead, size) &&
               offsetof(struct OGLES3Buffer, data) == offsetof(struct OGLES3BufferHead, data) &&
               offsetof(struct OGLES3Buffer, serial) == offsetof(struct OGLES3BufferHead, serial) &&
               offsetof(struct OGLES3Buffer, uid) == offsetof(struct OGLES3BufferHead, uid) &&
               offsetof(struct OGLES3Buffer, deleted) == offsetof(struct OGLES3BufferHead, deleted),
               "struct OGLES3Buffer starts with struct OGLES3BufferHead");

static struct OGLES3Buffer *create(struct OGLES3BufState *bs, GLuint name)
{
    struct OGLES3Buffer *b;
    GLuint slot = ogles2_name_slot(&bs->sh->far_buf, name, TRUE);
    if (slot == OGLES3_NO_SLOT || !table_room(bs, slot) ||
        !(b = AllocVec(sizeof(*b), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_name_forget(&bs->sh->far_buf, name);
        return 0;
    }
    b->name = name;
    b->usage = GL_STATIC_DRAW;
    Forbid(); /* every context's buffers: one counter */
    b->uid = ++next_uid ? next_uid : ++next_uid;
    Permit();
    bs->sh->buf[slot] = b;
    return b;
}

static struct OGLES3Buffer *by_name(struct OGLES3BufState *bs, GLuint name)
{
    GLuint slot = name ? ogles2_name_slot(&bs->sh->far_buf, name, FALSE) : OGLES3_NO_SLOT;
    return slot < bs->sh->nbuf ? bs->sh->buf[slot] : 0;
}

/* The buffer named `name` (not 0): glGenBuffers' names have one (ES 3.1's
 * glBindVertexBuffer, draw.c); NULL: a name it did not give, or deleted. */
struct OGLES3Buffer *ogles2_buffer_get(struct OGLES3Context *ctx, GLuint name)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    struct OGLES3Buffer *b;
    if (!bs || !(b = by_name(bs, name)))
        return 0;
    b->bound_once = TRUE;
    return b;
}

void ogles2_glGenBuffers(struct OGLES3Context *ctx, GLsizei n, GLuint *buffers)
{
    struct OGLES3BufState *bs;
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
        if (i >= OGLES3_NEAR_NAMES || !create(bs, i)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        if (buffers)
            buffers[k] = i;
    }
}

void ogles2_glDeleteBuffers(struct OGLES3Context *ctx, GLsizei n, const GLuint *buffers)
{
    ogles2_labels_forget(ctx, GL_BUFFER_KHR, n, buffers); /* KHR_debug's labels of the names */
    if (ogles2_context_valid(ctx))
        ogles2_trace(ctx, "glDeleteBuffers", n, n > 0 && buffers ? buffers[0] : 0);
    struct OGLES3BufState *bs;
    struct OGLES3Context *m;
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
        struct OGLES3Buffer *b = by_name(bs, buffers[k]);
        if (!b)
            continue;
        /* GLES 2.0 2.9: a deleted buffer is unbound wherever it is bound
         * in the context, attribute arrays included; here in every member
         * of the share group (GL would keep it while bound elsewhere).
         * ES 3.0 2.9.1: only the bound vertex array object's; the others
         * keep it (below). */
        for (m = ctx->share->first; m; m = m->share_next) {
            struct OGLES3BufState *mbs = m->buf;
            if (mbs)
                unbind_all(m, mbs, b);
            for (a = 0; a < OGLES3_MAX_VERTEX_ATTRIBS; a++) {
                if (m->array[a].buffer == b) {
                    m->array[a].buffer = 0;
                    m->array[a].pointer = 0;
                }
                if (m->array[a].bbuffer == b) { /* ES 3.1's bindings: GL_VERTEX_BINDING_BUFFER 0 */
                    m->array[a].bbuffer = 0;
                    m->array[a].boffset = 0;
                }
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

GLboolean ogles2_glIsBuffer(struct OGLES3Context *ctx, GLuint buffer)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
    if (!ogles2_context_valid(ctx) || !(bs = bs_get(ctx)))
        return GL_FALSE;
    b = by_name(bs, buffer);
    return b && b->bound_once ? GL_TRUE : GL_FALSE;
}

void ogles2_glBindBuffer(struct OGLES3Context *ctx, GLenum target, GLuint buffer)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b = 0, **at;
    if (!ogles2_entry_valid(ctx) || !(bs = bs_get(ctx)))
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
        struct OGLES3Buffer *old = *at;
        *at = b;
        if (old && old->deleted)
            ogles2_buffer_release(ctx, old); /* a vertex array object's deleted element buffer */
    }
}

static struct OGLES3Buffer *bound(struct OGLES3Context *ctx, struct OGLES3BufState *bs, GLenum target)
{
    struct OGLES3Buffer **at = slot(ctx, bs, target);
    if (at)
        return *at;
    ogles2_error(ctx, GL_INVALID_ENUM);
    return 0;
}

static void buffer_data(struct OGLES3Context *ctx, GLenum target, GLsizeiptr size, const void *data,
                         GLenum usage)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
    UBYTE *shadow = 0;
    if (!ogles2_entry_valid(ctx) || !(bs = bs_get(ctx)))
        return;
    if (!slot(ctx, bs, target)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (usage != GL_STREAM_DRAW && usage != GL_DYNAMIC_DRAW && usage != GL_STATIC_DRAW &&
        !valid_usage(ctx, usage)) { /* GLES2's three without the call */
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
    b->xfb_job = 0; /* new contents: what transform feedback wrote is gone */
    b->sto_job = 0;
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
void ogles2_glBufferData(struct OGLES3Context *ctx, GLenum target, GLsizeiptr size, const void *data,
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
static __attribute__((noinline)) void patch_copies_now(struct OGLES3Context *ctx, struct OGLES3Buffer *b, ULONG off,
                                                       ULONG len);

/* The test inline: a buffer the program streams into (GL_DYNAMIC_DRAW,
 * GL_STREAM_DRAW) never has a copy, nor words unless a uniform block read
 * it, and its every glBufferSubData paid the loop over the copies
 * (tests/perf/drawbench: 0.5 us a call). */
static inline void patch_copies(struct OGLES3Context *ctx, struct OGLES3Buffer *b, ULONG off, ULONG len)
{
    if (b->copied || b->words.hostptr)
        patch_copies_now(ctx, b, off, len);
}

static void patch_copies_now(struct OGLES3Context *ctx, struct OGLES3Buffer *b, ULONG off, ULONG len)
{
    int k;
    for (k = 0; k < OGLES3_BUFFER_COPIES; k++) {
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

static void buffer_sub_data(struct OGLES3Context *ctx, GLenum target, GLintptr offset, GLsizeiptr size,
                            const void *data)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
    if (!ogles2_entry_valid(ctx) || !(bs = bs_get(ctx)))
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
    gpu_written(ctx, b);
    memcpy(b->data + offset, data, (ULONG)size);
    b->serial++;
    patch_copies(ctx, b, (ULONG)offset, (ULONG)size);
}

/* Timed for probe P (frame.c). */
void ogles2_glBufferSubData(struct OGLES3Context *ctx, GLenum target, GLintptr offset, GLsizeiptr size,
                            const void *data)
{
    ULONG t0 = ogles2_clock_us();
    buffer_sub_data(ctx, target, offset, size, data);
    if (ogles2_context_valid(ctx))
        ctx->sum_bufdata += (double)(ULONG)(ogles2_clock_us() - t0);
}

void ogles2_glGetBufferParameteriv(struct OGLES3Context *ctx, GLenum target, GLenum pname,
                                   GLint *params)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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

void ogles2_glGetBufferParameteri64v(struct OGLES3Context *ctx, GLenum target, GLenum pname, GLint64 *params)
{
    GLint v = 0;
    if (!ogles2_es3_entry(ctx) || !params)
        return;
    ogles2_glGetBufferParameteriv(ctx, target, pname, &v);
    *params = pname == GL_BUFFER_ACCESS_FLAGS ? (GLint64)(ULONG)v : (GLint64)v;
}

/* AmigaOS 4's name for the same query (its OES_mapbuffer). */
void ogles2_glGetBufferParameterivOES(struct OGLES3Context *ctx, GLenum target, GLenum pname,
                                      GLint *params)
{
    ogles2_glGetBufferParameteriv(ctx, target, pname, params);
}

/* ---- OES_mapbuffer: the mapping is the shadow ---- */

void *ogles2_glMapBufferOES(struct OGLES3Context *ctx, GLenum target, GLenum access)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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
    gpu_written(ctx, b);
    b->mapped = TRUE;
    b->map_offset = 0;
    b->map_length = b->size;
    b->map_access = GL_MAP_WRITE_BIT;
    return b->data;
}

GLboolean ogles2_glUnmapBufferOES(struct OGLES3Context *ctx, GLenum target)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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
void *ogles2_glMapBufferRange(struct OGLES3Context *ctx, GLenum target, GLintptr offset, GLsizeiptr length,
                              GLbitfield access)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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
    gpu_written(ctx, b); /* transform feedback's results (unsynchronized too: they would be lost) */
    b->mapped = TRUE;
    b->map_offset = (ULONG)offset;
    b->map_length = (ULONG)length;
    b->map_access = access;
    return b->data + offset;
}

void ogles2_glFlushMappedBufferRange(struct OGLES3Context *ctx, GLenum target, GLintptr offset, GLsizeiptr length)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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
GLboolean ogles2_glUnmapBuffer(struct OGLES3Context *ctx, GLenum target)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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

void ogles2_glGetBufferPointerv(struct OGLES3Context *ctx, GLenum target, GLenum pname, void **params)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glGetBufferPointervOES(ctx, target, pname, params);
}

void ogles2_glGetBufferPointervOES(struct OGLES3Context *ctx, GLenum target, GLenum pname, void **params)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b;
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

BOOL ogles2_buffer_query(struct OGLES3Context *ctx, GLenum pname, GLint *v)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    struct OGLES3Buffer **at;
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
    case GL_DRAW_INDIRECT_BUFFER_BINDING:      target = GL_DRAW_INDIRECT_BUFFER; break;
    case GL_DISPATCH_INDIRECT_BUFFER_BINDING:  target = GL_DISPATCH_INDIRECT_BUFFER; break;
    case GL_SHADER_STORAGE_BUFFER_BINDING:     target = GL_SHADER_STORAGE_BUFFER; break;
    case GL_ATOMIC_COUNTER_BUFFER_BINDING:     target = GL_ATOMIC_COUNTER_BUFFER; break;
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
 * NULL with none bound, or for the library's own images (OGLES3PixelStore's
 * own). *bad (GL_INVALID_OPERATION): past its end, or it is mapped. A pack
 * buffer's data change: its GPU copies start afresh. */
UBYTE *ogles2_buffer_pixels(struct OGLES3Context *ctx, BOOL pack, ULONG offset, ULONG bytes, BOOL *bad)
{
    static UBYTE none; /* an empty buffer's address for an empty image */
    struct OGLES3BufState *bs = ctx->buf;
    struct OGLES3Buffer *b = bs ? (pack ? bs->pack : bs->unpack) : 0;
    *bad = FALSE;
    if (!b || (pack ? ctx->pack.own : ctx->unpack.own))
        return 0;
    if (b->mapped || offset + bytes < offset || offset + bytes > b->size) {
        *bad = TRUE;
        return 0;
    }
    if (!b->data)
        return &none;
    gpu_written(ctx, b);
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
ULONG ogles2_buffer_words(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
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
ULONG ogles2_buffer_uniform(struct OGLES3Context *ctx, ULONG index, ULONG *size)
{
    struct OGLES3BufState *bs = ctx->buf;
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

BOOL ogles2_buffer_uniform_fed_back(struct OGLES3Context *ctx, ULONG index)
{
    struct OGLES3BufState *bs = ctx->buf;
    return bs && index < MAX_UNIFORM_BINDINGS && bs->ubo[index].b && bs->ubo[index].b->xfb_job == ctx->job_serial;
}

/* Storage buffer binding `index` as a compute dispatch reads and writes it
 * (ES 3.1, compute.c): as ogles2_buffer_uniform; the dispatch is waited for,
 * so the buffer's shadow is then stale (gpu_written copies the words back
 * before the CPU or a draw next uses it). */
static ULONG storage_range(struct OGLES3Context *ctx, struct buf_range *r, ULONG *size)
{
    ULONG a;
    if (!r->b || r->offset >= r->b->size)
        return 0;
    gpu_written(ctx, r->b); /* transform feedback's, or an earlier dispatch's (already the words) */
    if (!(a = ogles2_buffer_words(ctx, r->b)))
        return 0;
    r->b->cs_written = TRUE;
    ogles2_gpu_writes_seen = TRUE;
    *size = r->b->size - r->offset;
    if (r->size && r->size < *size)
        *size = r->size;
    return a + r->offset;
}

ULONG ogles2_buffer_storage(struct OGLES3Context *ctx, ULONG index, ULONG *size)
{
    struct OGLES3BufState *bs = ctx->buf;
    if (!bs || index >= MAX_SSBO_BINDINGS)
        return 0;
    return storage_range(ctx, &bs->ssbo[index], size);
}

/* Atomic counter buffer binding `index` (ES 3.1): a storage buffer to the
 * shader (Mesa lowers the counters to storage accesses), as above. */
ULONG ogles2_buffer_atomic(struct OGLES3Context *ctx, ULONG index, ULONG *size)
{
    struct OGLES3BufState *bs = ctx->buf;
    if (!bs || index >= MAX_ATOMIC_BINDINGS)
        return 0;
    return storage_range(ctx, &bs->acbo[index], size);
}

/* A storage buffer (or atomic counter buffer) binding a draw's fragment
 * shader reads and writes (ES 3.1): its words, the range's address and
 * bytes; the open job writes them, so the CPU's next access waits for it
 * and copies them back (as transform feedback's, gpu_written). Not
 * gpu_written here: a draw is building the job (the GPU reads the words,
 * which are the newest data whoever wrote them). 0: no buffer or memory. */
ULONG ogles2_buffer_storage_draw(struct OGLES3Context *ctx, BOOL atomic, ULONG index, ULONG *size)
{
    struct OGLES3BufState *bs = ctx->buf;
    struct buf_range *r;
    ULONG a;
    *size = 0;
    if (!bs || index >= (atomic ? MAX_ATOMIC_BINDINGS : MAX_SSBO_BINDINGS))
        return 0;
    r = atomic ? &bs->acbo[index] : &bs->ssbo[index];
    if (!r->b || r->offset >= r->b->size || !(a = ogles2_buffer_words(ctx, r->b)))
        return 0;
    r->b->xfb_job = ctx->job_serial;
    r->b->sto_job = ctx->job_serial;
    ogles2_gpu_writes_seen = TRUE;
    *size = r->b->size - r->offset;
    if (r->size && r->size < *size)
        *size = r->size;
    return a + r->offset;
}

/* A buffer the GPU wrote (transform feedback, a compute shader, a draw's
 * storage writes), read by a draw as the GPU wrote it: its words, with no
 * wait for the render and no copy back into the shadow (docs/GLES3_PLAN.md,
 * the remaining gaps, item 5). 0 when the GPU did not write it (its data are
 * the shadow's), when a draw of the open job writes it as a storage buffer
 * (those fragment shaders run at the render, after the binning that would
 * read: the CPU path renders the job first), or without words. *fed:
 * transform feedback wrote it in the open job (the draw waits for the
 * feedback, draw.c). *size: the buffer's bytes. The open job reads the
 * words from now: a CPU change renames them (ogles2_buffer_words). */
ULONG ogles2_buffer_gpu_words(struct OGLES3Context *ctx, struct OGLES3Buffer *b, BOOL *fed, ULONG *size)
{
    if (!b || (!b->xfb_job && !b->cs_written) || !b->words.hostptr || b->words_serial != b->serial ||
        (b->sto_job && b->sto_job == ctx->job_serial))
        return 0;
    *fed = b->xfb_job == ctx->job_serial;
    *size = b->size;
    b->words_job = ctx->job_serial;
    return (ULONG)b->words.hostptr;
}

/* The element array buffer's words for a draw that reads indices on the
 * GPU (draw.c, gpu_elements and the indirect draws): as the GPU wrote them
 * (ogles2_buffer_gpu_words), or with `cpu` the shadow's in the GPU's order
 * (ogles2_buffer_words; 32-bit indices only, which those words hold as
 * the program gave them). 0: no buffer, none of those. */
ULONG ogles2_buffer_element_words(struct OGLES3Context *ctx, BOOL cpu, BOOL *fed, ULONG *size)
{
    struct OGLES3BufState *bs = ctx->buf;
    struct OGLES3Buffer *b = bs ? bs->element : 0;
    ULONG a;
    *fed = FALSE;
    if (!b || !b->data)
        return 0;
    if ((a = ogles2_buffer_gpu_words(ctx, b, fed, size)) != 0 || !cpu)
        return a;
    if (b->xfb_job || b->cs_written)
        return 0; /* GPU-written but not readable now: the CPU path */
    *size = b->size;
    return ogles2_buffer_words(ctx, b);
}

/* glDrawArraysIndirect's and glDrawElementsIndirect's command buffer when
 * the GPU wrote it: the checks of ogles2_buffer_draw_indirect, then the
 * command's address in the buffer's words for the hardware's indirect
 * packets (*addr 0: the GPU did not write it, or it cannot be read so:
 * the CPU reads the command). 0, or the error. */
GLenum ogles2_buffer_draw_indirect_gpu(struct OGLES3Context *ctx, GLintptr offset, ULONG bytes, ULONG *addr,
                                       BOOL *fed)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    struct OGLES3Buffer *b = bs ? bs->draw_indirect : 0;
    ULONG w, size;
    *addr = 0;
    *fed = FALSE;
    if (!b)
        return GL_INVALID_OPERATION;
    if (offset & 3)
        return GL_INVALID_VALUE;
    if (!b->data || (ULONG)offset > b->size || b->size - (ULONG)offset < bytes)
        return GL_INVALID_OPERATION;
    if ((w = ogles2_buffer_gpu_words(ctx, b, fed, &size)) != 0)
        *addr = w + (ULONG)offset;
    return 0;
}

/* glDispatchComputeIndirect's three work group counts at `offset` in the
 * GL_DISPATCH_INDIRECT_BUFFER binding: 0, or the error (ES 3.1 17:
 * GL_INVALID_VALUE for an offset not a word's, GL_INVALID_OPERATION without
 * a buffer or past its end). */
GLenum ogles2_buffer_dispatch_indirect(struct OGLES3Context *ctx, GLintptr offset, ULONG *groups)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    struct OGLES3Buffer *b = bs ? bs->dispatch_indirect : 0;
    if (offset < 0 || (offset & 3))
        return GL_INVALID_VALUE;
    if (!b || !b->data || (ULONG)offset > b->size || b->size - (ULONG)offset < 12)
        return GL_INVALID_OPERATION;
    gpu_written(ctx, b);
    memcpy(groups, b->data + offset, 12);
    return 0;
}

/* glDrawArraysIndirect's and glDrawElementsIndirect's command (ES 3.1
 * 10.5), `bytes` at `offset` in the GL_DRAW_INDIRECT_BUFFER binding, into
 * words: 0, or the error (GL_INVALID_OPERATION without a buffer, then
 * GL_INVALID_VALUE for an offset not a word's, GL_INVALID_OPERATION past
 * its end; a negative offset is past it, dEQP-GLES31's signed32_wrap
 * case). */
GLenum ogles2_buffer_draw_indirect(struct OGLES3Context *ctx, GLintptr offset, ULONG bytes, ULONG *words)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    struct OGLES3Buffer *b = bs ? bs->draw_indirect : 0;
    /* no buffer before the alignment (Mesa checks the alignment first): a
     * client pointer, dEQP's negative.client_command_array, is aligned on
     * a PC, not always on the 68k; ES 3.1 orders neither */
    if (!b)
        return GL_INVALID_OPERATION;
    if (offset & 3)
        return GL_INVALID_VALUE;
    if (!b || !b->data || (ULONG)offset > b->size || b->size - (ULONG)offset < bytes)
        return GL_INVALID_OPERATION;
    gpu_written(ctx, b);
    memcpy(words, b->data + offset, bytes);
    return 0;
}

/* Transform feedback writes b in the open job (xfb.c): the address of its
 * words (made current with the shadow, ogles2_buffer_words), which the
 * shadow then waits for (gpu_written). 0: no data or memory. */
ULONG ogles2_buffer_xfb(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    ULONG a = ogles2_buffer_words(ctx, b);
    if (a) {
        b->xfb_job = ctx->job_serial;
        ogles2_gpu_writes_seen = TRUE;
    }
    return a;
}

/* ---- ES 3.0's indexed bindings and copies ---- */

/* The indexed bindings of a target that has them (ES 3.0's uniform buffers;
 * ES 3.1's storage and atomic counter buffers in 3.1 contexts): their
 * array, count and general binding; NULL for transform feedback's (the
 * bound object's, xfb.c) and every other target. */
static struct buf_range *ranges(struct OGLES3Context *ctx, struct OGLES3BufState *bs, GLenum target, ULONG *n,
                                struct OGLES3Buffer ***general)
{
    switch (target) {
    case GL_UNIFORM_BUFFER:
        *n = MAX_UNIFORM_BINDINGS;
        *general = &bs->uniform;
        return bs->ubo;
    case GL_SHADER_STORAGE_BUFFER:
        *n = MAX_SSBO_BINDINGS;
        *general = &bs->storage;
        return ogles2_es31(ctx) ? bs->ssbo : 0;
    case GL_ATOMIC_COUNTER_BUFFER:
        *n = MAX_ATOMIC_BINDINGS;
        *general = &bs->atomic;
        return ogles2_es31(ctx) ? bs->acbo : 0;
    default:
        return 0;
    }
}

static void bind_range(struct OGLES3Context *ctx, GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                       GLsizeiptr size, BOOL ranged)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer *b = 0, **general = 0;
    struct buf_range *r;
    struct OGLES3XfbBinding *x = 0;
    ULONG n = OGLES3_MAX_XFB_BUFFERS, align;
    if (!ogles2_es3_entry(ctx) || !(bs = bs_get(ctx)))
        return;
    r = ranges(ctx, bs, target, &n, &general);
    if (!r && target != GL_TRANSFORM_FEEDBACK_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (index >= (r ? n : OGLES3_MAX_XFB_BUFFERS)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* transform feedback's are the bound transform feedback object's, and
     * stay while it is active (ES 3.0 2.15.2) */
    if (!r && ogles2_xfb_active(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!r && !(x = ogles2_xfb_bindings(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    /* Mesa's checks (bufferobj.c): a range of a buffer is not empty, and
     * starts at GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT (256), a storage
     * buffer's at GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (4), an atomic
     * counter buffer's at a word; transform feedback's at a word, a whole
     * number of words long. */
    align = target == GL_UNIFORM_BUFFER ? 255 : 3;
    if (ranged && buffer &&
        (size <= 0 || offset < 0 || (offset & align) || (target == GL_TRANSFORM_FEEDBACK_BUFFER && (size & 3)))) {
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
    /* the target's own binding too (ES 3.0 2.9.1) */
    if (r) {
        r += index;
        r->b = b;
        r->offset = ranged && b ? (ULONG)offset : 0;
        r->size = ranged && b ? (ULONG)size : 0;
        *general = b;
    } else {
        struct OGLES3Buffer *old = x[index].b;
        x[index].b = b;
        x[index].offset = ranged && b ? (ULONG)offset : 0;
        x[index].size = ranged && b ? (ULONG)size : 0;
        bs->xfb = b;
        ogles2_buffer_release(ctx, old); /* a deleted buffer only it held */
    }
}

void ogles2_glBindBufferRange(struct OGLES3Context *ctx, GLenum target, GLuint index, GLuint buffer,
                              GLintptr offset, GLsizeiptr size)
{
    bind_range(ctx, target, index, buffer, offset, size, TRUE);
}

void ogles2_glBindBufferBase(struct OGLES3Context *ctx, GLenum target, GLuint index, GLuint buffer)
{
    bind_range(ctx, target, index, buffer, 0, 0, FALSE);
}

/* An indexed buffer binding's query: its target and which of the three
 * values (0 the name, 1 the start, 2 the size); FALSE: none of them. */
static BOOL indexed_pname(GLenum pname, GLenum *target, int *what)
{
    static const GLenum q[4][4] = {
        {GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER_BINDING, GL_UNIFORM_BUFFER_START, GL_UNIFORM_BUFFER_SIZE},
        {GL_TRANSFORM_FEEDBACK_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, GL_TRANSFORM_FEEDBACK_BUFFER_START,
         GL_TRANSFORM_FEEDBACK_BUFFER_SIZE},
        {GL_SHADER_STORAGE_BUFFER, GL_SHADER_STORAGE_BUFFER_BINDING, GL_SHADER_STORAGE_BUFFER_START,
         GL_SHADER_STORAGE_BUFFER_SIZE},
        {GL_ATOMIC_COUNTER_BUFFER, GL_ATOMIC_COUNTER_BUFFER_BINDING, GL_ATOMIC_COUNTER_BUFFER_START,
         GL_ATOMIC_COUNTER_BUFFER_SIZE},
    };
    int t, k;
    for (t = 0; t < 4; t++)
        for (k = 1; k < 4; k++)
            if (q[t][k] == pname) {
                *target = q[t][0];
                *what = k - 1;
                return TRUE;
            }
    return FALSE;
}

/* glGetIntegeri_v and glGetInteger64i_v's buffer bindings: ES 3.0's uniform
 * and transform feedback buffers, ES 3.1's storage and atomic counter ones.
 * -1: not such a query (others answer it: ogles2_indexed_query); 0: an
 * error set; 1: *v. */
static int indexed(struct OGLES3Context *ctx, GLenum pname, GLuint index, GLint64 *v)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer **general;
    const struct buf_range *r = 0;
    const struct OGLES3Buffer *b;
    ULONG offset, size, n = OGLES3_MAX_XFB_BUFFERS;
    GLenum target;
    int what;
    if (!indexed_pname(pname, &target, &what))
        return -1;
    if (!(bs = bs_get(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return 0;
    }
    if (target != GL_TRANSFORM_FEEDBACK_BUFFER && !(r = ranges(ctx, bs, target, &n, &general))) {
        ogles2_error(ctx, GL_INVALID_ENUM); /* ES 3.1's in an ES 3.0 context */
        return 0;
    }
    if (index >= n) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    if (r) {
        b = r[index].b;
        offset = r[index].offset;
        size = r[index].size;
    } else {
        const struct OGLES3XfbBinding *x = ogles2_xfb_bindings(ctx); /* the bound object's */
        b = x ? x[index].b : 0;
        offset = x ? x[index].offset : 0;
        size = x ? x[index].size : 0;
    }
    *v = what == 0 ? (b ? (GLint64)b->name : 0) : what == 1 ? (GLint64)offset : (GLint64)size;
    return 1;
}

void ogles2_glGetIntegeri_v(struct OGLES3Context *ctx, GLenum target, GLuint index, GLint *data)
{
    GLint64 v;
    int r;
    if (!ogles2_es3_entry(ctx) || !data)
        return;
    if ((r = indexed(ctx, target, index, &v)) < 0)
        r = ogles2_indexed_query(ctx, target, index, &v);
    if (r > 0)
        *data = (GLint)v;
}

/* ES 3.1's: the same indexed state as booleans (GL_IMAGE_BINDING_LAYERED's
 * kind; the rest converted as glGetBooleanv converts). */
void ogles2_glGetBooleani_v(struct OGLES3Context *ctx, GLenum target, GLuint index, GLboolean *data)
{
    GLint64 v;
    int r;
    if (!ogles2_es31_entry(ctx) || !data)
        return;
    if ((r = indexed(ctx, target, index, &v)) < 0)
        r = ogles2_indexed_query(ctx, target, index, &v);
    if (r > 0)
        *data = v ? GL_TRUE : GL_FALSE;
}

void ogles2_glGetInteger64i_v(struct OGLES3Context *ctx, GLenum target, GLuint index, GLint64 *data)
{
    GLint64 v;
    int r;
    if (!ogles2_es3_entry(ctx) || !data)
        return;
    if ((r = indexed(ctx, target, index, &v)) < 0)
        r = ogles2_indexed_query(ctx, target, index, &v);
    if (r > 0)
        *data = v;
}

void ogles2_glCopyBufferSubData(struct OGLES3Context *ctx, GLenum readTarget, GLenum writeTarget,
                                GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
    struct OGLES3BufState *bs;
    struct OGLES3Buffer **rs, **ws, *r, *w;
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
    gpu_written(ctx, r);
    gpu_written(ctx, w);
    memmove(w->data + writeOffset, r->data + readOffset, (ULONG)size);
    w->serial++;
    patch_copies(ctx, w, (ULONG)writeOffset, (ULONG)size);
}

struct OGLES3Buffer *ogles2_buffer_element(struct OGLES3Context *ctx)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    return bs ? bs->element : 0;
}

void ogles2_buffer_set_element(struct OGLES3Context *ctx, struct OGLES3Buffer *b)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    if (bs)
        bs->element = b;
}

/* The GL_ARRAY_BUFFER binding (glVertexAttribPointer captures it). */
struct OGLES3Buffer *ogles2_buffer_array(struct OGLES3Context *ctx)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    return bs ? bs->array : 0;
}

GLuint ogles2_buffer_name(const struct OGLES3Buffer *b)
{
    return b ? b->name : 0;
}

ULONG ogles2_buffer_size(const struct OGLES3Buffer *b)
{
    return b ? b->size : 0;
}

/* glDrawElements' indices in the GL_ELEMENT_ARRAY_BUFFER: `offset` bytes
 * into it, `bytes` long; NULL without an element buffer (the indices are
 * then the application's pointer) or with the range outside it (*bad). */
const void *ogles2_buffer_indices(struct OGLES3Context *ctx, ULONG offset, ULONG bytes, BOOL *bad)
{
    struct OGLES3BufState *bs = bs_get(ctx);
    *bad = FALSE;
    if (!bs || !bs->element)
        return 0;
    if (!bs->element->data || offset + bytes > bs->element->size || offset + bytes < offset) {
        *bad = TRUE;
        return 0;
    }
    gpu_written(ctx, bs->element); /* before the draw builds its job */
    return bs->element->data + offset;
}

/* A buffer the application changes as it draws (GL_DYNAMIC_DRAW or
 * GL_STREAM_DRAW, and ES 3.0's _READ and _COPY usages but the static ones): its shadow, which a draw converts the vertices it reads
 * from into the job's state buffer, as from a client array. NULL for a
 * GL_STATIC_DRAW buffer: draws read its kept GPU copies (ogles2_buffer_copy),
 * which would be made again, whole, after every change. */
ULONG ogles2_buffer_serial(const struct OGLES3Buffer *b, ULONG *uid)
{
    *uid = b->uid;
    return b->serial;
}

const UBYTE *ogles2_buffer_stream(const struct OGLES3Buffer *b)
{
    return b->usage != GL_STATIC_DRAW && b->usage != GL_STATIC_READ && b->usage != GL_STATIC_COPY ? b->data : 0;
}

/* A GPU copy of buffer `b` read as an attribute array of `size` components
 * of `type` at `offset` with `stride` (0: packed): converted with
 * `convert` (draw.c's, as for client arrays) into tightly packed elements.
 * Returns its GPU address of vertex 0 and its stride; 0 if out of memory or
 * the buffer holds no complete element at `offset`. *vertices: how many. */
ULONG ogles2_buffer_copy(struct OGLES3Context *ctx, struct OGLES3Buffer *b, ULONG offset, GLint size,
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
    for (k = 0; k < OGLES3_BUFFER_COPIES; k++) {
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
    b->copied = TRUE;
    *dst_stride = c->dst_stride;
    *vertices = n - skip;
    return (ULONG)c->mem.hostptr + skip * c->dst_stride;
}
