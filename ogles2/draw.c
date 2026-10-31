/*
 * The draw path (plan section 7.9): client-side vertex arrays and
 * glDrawArrays with the linked program's image: a .v3dp program binary's
 * compiled variants (M2), or the built-in image for clears.
 *
 * Per draw, into the job's state buffer: the uniform streams of the binning,
 * vertex and fragment shaders (their recipes, plan 8.6), a copy of each
 * enabled array's [first, first+count) byte-swapped for the GPU, a 16-byte
 * constant for each disabled one, then the GL shader state record and one
 * attribute record per vertex input slot, filled from the variants'
 * prog_data as Mesa's v3dx_draw.c does. Into the binning list: the state
 * packets that changed (clip window, CFG_BITS, viewport, blend, colour mask;
 * deduplicated per control list as Mesa and PiStorm3D do), the per-pass
 * residue on a pass's first draw, VCM_CACHE_SIZE, GL_SHADER_STATE and
 * VERTEX_ARRAY_PRIMS, in Mesa's order (v3dx_emit.c, v3dx_draw.c).
 *
 * Every claim is filled before the next one is made: a claim that grows the
 * state buffer flushes the old block and starts a new one (v3d_clbuf.c), so
 * a filled block's addresses stay valid until the frame retires.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <stddef.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "../v3dp/v3dglsl_port.h" /* V3DGLSL_KEY_OUTPUTS_FROM_FS */
#include "variant_key.h"

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a control list restarts */

/* Last packet values emitted into the current binning list (Mesa's dirty
 * flags, PiStorm3D's mglv3d_dedup_cache). Valid while gen matches. */
struct dedup
{
    v3d_u32 gen;
    /* What the last emit_state was made from: the same again skips it. */
    ULONG ver;
    const struct OGLES3Framebuffer *fb;
    UWORD fb_w, fb_h;
    UBYTE fb_depth_bits, fb_stencil_bits;
    const struct OGLES3Variant *fs;
    BOOL cfg, cw, vp, blend, cwm, lw, st;
    UBYTE effp, erfp, cp, edo, dtf, zue, eze, ezue, be, se, wf, pf, pv, rom;
    UBYTE smask;          /* the SAMPLE_STATE mask last sent (4x MSAA) */
    UBYTE stencil[2][5];  /* the STENCIL_CFG bytes last sent, front and back */
    UWORD cw_l, cw_t, cw_w, cw_h;
    UWORD vp_w, vp_h, vp_cx2, vp_cy2;
    UBYTE b_src, b_dst, b_eq, b_asrc, b_adst, b_aeq, b_one;
    UWORD b_color[4];
    ULONG cwm_mask;
    float lw_value, do_factor, do_units;
    float vp_zs, vp_zo;
    BOOL vp_down;
    BOOL do_valid;
    int prim;
    /* The varying flags (flat, no-perspective, centroid: varying_flags):
     * the fragment shader they were sent for this pass, and which kinds the
     * hardware holds set (bit 0 flat, 1 no-perspective, 2 centroid). */
    const struct OGLES3Variant *flags_fs;
    UBYTE flags_set;
};

#define U_KINDS 48 /* the uniform kinds u_kind knows (write_uniforms) */

/* The range of indices from an element buffer, scanned for the buffer's data
 * (uid, serial: ogles2_buffer_serial) at ptr, count of type (draw_elements) */
struct index_range
{
    ULONG uid, serial, lo, hi;
    const void *ptr;
    GLsizei count;
    GLenum type;
    BOOL restart;
};

/* Their copy for the GPU in job `job`'s state buffer at addr, rebased by sub
 * to itype (draw_arrays) */
struct index_copy
{
    ULONG job, uid, serial, sub, addr;
    const void *ptr;
    GLsizei count;
    GLenum type, itype;
    BOOL restart;
};

/* Vertices [first, + count) of a buffer's shadow (uid, at data serial
 * `serial`) read at ptr with stride `stride` in format `key` (an array's
 * fmt_key), copied to addr in job `job`'s state buffer (copy_array): a draw
 * of the same job reading vertices within them from the same unchanged data
 * reuses them. */
struct stream_copy
{
    const void *ptr;
    ULONG job, uid, serial, key, first, count, addr;
    GLsizei stride;
};

/* The slots: the key's bits folded by shifts (8, 9, 13: buffer offsets a
 * few hundred bytes apart spread, tests/perf/drawbench's 320 and 960). No
 * multiplicative hash: Emu68 runs muls.l/mulu.l #imm,Dn then lsr.l Dx,Dn
 * wrongly (the shift sees the product's upper half: (1 * 0x9E3779B1) >> 28
 * gave 0xFFFFFFF9; the A1200, 2026-10-29), and a slot out of the tables
 * wrote over memory. */
#define IC_SLOTS 16
#define SC_SLOTS 32
static inline ULONG slot_fold(ULONG x)
{
    x ^= x >> 8;
    x ^= x >> 9;
    x ^= x >> 13;
    return x;
}
#define IC_SLOT(uid, ptr, count) \
    (slot_fold((ULONG)(ptr) ^ ((ULONG)(uid) << 12) ^ ((ULONG)(count) << 18)) & (IC_SLOTS - 1))
#define SC_SLOT(uid, ptr, stride, key) \
    (slot_fold((ULONG)(ptr) ^ ((ULONG)(uid) << 12) ^ ((ULONG)(stride) << 20) ^ (key)) & (SC_SLOTS - 1))

struct OGLES3Draw
{
    v3d_mem builtin_code;          /* the built-in image's code, uploaded at first use */
    struct OGLES3Program quad;     /* clears drawn as rectangles (built-in image) */
    BOOL quad_ready;
    /* and into several render targets (EXT_draw_buffers): clear_mrt_v3dp.h */
    struct OGLES3Program mrt_quad;
    struct OGLES3Image *mrt_image;
    BOOL mrt_ready, mrt_failed;
    /* and depth and stencil where a target is 32-bit float or integer: clear_zs_v3dp.h */
    struct OGLES3Program zs_quad;
    struct OGLES3Image *zs_image;
    BOOL zs_ready, zs_failed;
    BOOL wrong_width_logged;       /* a draw skipped for its colour writes' width (draw_arrays) */
    /* and glCopyTex* on the GPU (copy.c): copy_tex_v3dp.h */
    struct OGLES3Program copy_prog;
    struct OGLES3Image *copy_image;
    BOOL copy_ready, copy_failed;
    struct dedup dd;
    v3d_u32 defattr_gen;           /* the default attribute block of this control list */
    ULONG defattr;
    const struct OGLES3Image *missed; /* the last image a variant miss was logged for */
    const struct OGLES3Image *missed_rt; /* and a render-target miss (EXT_draw_buffers) */
    v3d_mem spill;                 /* register spilling's scratch area (spill_area) */
    ULONG spill_size;              /* bytes a thread has in it */
    v3d_mem zero_ubo;              /* zeros a uniform block reads while no buffer can be bound (ubo_copies) */
    /* Indices from element buffers (tests/perf/drawbench; a game draws the
     * same meshes several times a frame, Doom 3 once a light): their ranges
     * (ir) and their copies for the GPU in this job (ic); and the copies of
     * vertices from buffers' shadows in this job (sc). Slots by IC_SLOT and
     * SC_SLOT. */
    struct index_range ir[IC_SLOTS];
    struct index_copy ic[IC_SLOTS];
    struct stream_copy sc[SC_SLOTS];
    /* Each disabled attribute's current value as last written into this
     * control list's state buffer (constant_attrib): its words (the GPU's
     * byte order) and kind, at ca_addr */
    struct
    {
        v3d_u32 gen;
        ULONG addr, w[4];
        UBYTE kind;
    } ca[OGLES3_MAX_VERTEX_ATTRIBS];
    /* The uniform streams' state values (state_fx), for the context's
     * ustate_ver, the program's Z clip (fx_z1) and the blend colour */
    ULONG fx[U_KINDS + 2], fx_ver, fx_bc[4];
    BOOL fx_ok, fx_z1;
};

/* ---- vertex arrays ---- */

static int type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_FLOAT: case GL_FIXED:          return 4;
    /* ES 3.0's (docs/GLES3_PLAN.md §5 item 5) */
    case GL_HALF_FLOAT:                    return 2;
    case GL_INT: case GL_UNSIGNED_INT:     return 4;
    case GL_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV:   return 4; /* the four components' one word */
    default:                               return 0;
    }
}

static BOOL packed_type(GLenum type)
{
    return type == GL_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_2_10_10_10_REV;
}

/* An element's bytes as the application stores it. */
static ULONG elem_bytes(const struct OGLES3Array *a)
{
    return packed_type(a->type) ? 4 : (ULONG)(type_size(a->type) * a->size);
}

/* The types the CPU turns into 32-bit floats first (the hardware reads no
 * 32-bit integer as a float, and Mesa never gives it three half floats:
 * u_vbuf converts them), as convert_elems' `type`. */
#define CONV_INT_FLOAT   0x10000UL /* GL_INT, + 1: normalized; + 2, + 3: GL_UNSIGNED_INT */
#define CONV_HALF3_FLOAT 0x10004UL

/* ES 3.1's vertex attribute binding (stage D item 5): every attribute that
 * reads binding j takes its buffer, offset, stride and divisor (the fields
 * the draws read), the attribute's relative offset added. The buffers no
 * longer referenced, and `prev` (the binding's buffer before the caller
 * changed it, or 0), are released once each after every reference changed:
 * a deleted buffer only they held is freed, and releasing it twice (two
 * attributes on the binding, or an attribute's and the binding's
 * reference) freed it twice (dEQP-GLES31 draw_indirect's restarts, exec's
 * AN_FreeTwice). GLES2 and ES 3.0 contexts change formats and bindings
 * together, attribute i on binding i, so the draws see what they always
 * saw. */
static void vab_refresh(struct OGLES3Context *ctx, GLuint j, struct OGLES3Buffer *prev)
{
    struct OGLES3Buffer *old[OGLES3_MAX_VERTEX_ATTRIBS + 1];
    const struct OGLES3Array *bnd = &ctx->array[j];
    int i, k, n = 0;
    if (prev && prev != bnd->bbuffer)
        old[n++] = prev;
    for (i = 0; i < OGLES3_MAX_VERTEX_ATTRIBS; i++) {
        struct OGLES3Array *a = &ctx->array[i];
        if (a->binding != j)
            continue;
        if (a->buffer && a->buffer != bnd->bbuffer) {
            for (k = 0; k < n && old[k] != a->buffer; k++)
                ;
            if (k == n)
                old[n++] = a->buffer;
        }
        a->buffer = bnd->bbuffer;
        a->pointer = (const UBYTE *)bnd->boffset + a->reloffset;
        a->stride = bnd->bstride;
        a->zero_stride = bnd->bstride == 0;
        a->divisor = bnd->bdivisor;
    }
    while (n--)
        ogles2_buffer_release(ctx, old[n]);
}

/* glVertexAttribPointer and glVertexAttribIPointer (`integer`); FALSE: an
 * error. ES 3.1 2.9.1: the attribute's format with relative offset 0, on
 * binding `index`, whose buffer, offset and stride (0: the format's packed
 * size) it sets. Out of line: inlined, glVertexAttribPointer's fast path
 * paid its frame and ten saved registers on every call. */
static __attribute__((noinline)) BOOL set_pointer(struct OGLES3Context *ctx, GLuint index, GLint size, GLenum type,
                                                  GLboolean normalized, GLsizei stride, const void *pointer,
                                                  GLboolean integer)
{
    struct OGLES3Array *a, *bnd;
    struct OGLES3Buffer *old, *nb;
    int ts;
    if (!ogles2_context_valid(ctx))
        return FALSE;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS || size < 1 || size > 4 || stride < 0 ||
        (ogles2_es31(ctx) && stride > 2048)) { /* GL_MAX_VERTEX_ATTRIB_STRIDE */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return FALSE;
    }
    /* OES_vertex_half_float's GL_HALF_FLOAT_OES (every context): ES 3.0's GL_HALF_FLOAT */
    BOOL half_oes = type == GL_HALF_FLOAT_OES && !integer;
    if (half_oes)
        type = GL_HALF_FLOAT;
    if (!(ts = type_size(type)) ||
        (!ogles2_es3(ctx) && !half_oes &&
         (type == GL_HALF_FLOAT || type == GL_INT || type == GL_UNSIGNED_INT || packed_type(type)))) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return FALSE;
    }
    if (packed_type(type) && size != 4) { /* ES 3.0 2.8 */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    /* ES 3.0 2.8: no client arrays in a vertex array object but 0 (the
     * object asked for only for a client array: a Quake III frame sets 250
     * pointers into buffers) */
    nb = ogles2_buffer_array(ctx);
    if (!nb && pointer && ogles2_vao_bound(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    a = &ctx->array[index];
    a->size = size;
    a->type = type;
    a->normalized = normalized ? GL_TRUE : GL_FALSE;
    a->integer = integer;
    a->reloffset = 0;
    a->ustride = stride;
    a->binding = (UBYTE)index;
    bnd = a; /* binding `index`'s state is slot index's */
    old = bnd->bbuffer;
    bnd->bbuffer = nb; /* GLES 2.0 2.9: captured now */
    bnd->boffset = pointer;
    bnd->bstride = stride ? stride : packed_type(type) ? 4 : ts * size; /* elem_bytes */
    if (!ctx->vab_moved) {
        /* Every attribute reads its own binding (no glVertexAttribBinding
         * moved one yet): this one alone takes the binding's state, as
         * vab_refresh would, without its look at all 16 (a Quake III frame
         * sets 250 pointers). */
        a->buffer = bnd->bbuffer;
        a->pointer = pointer;
        a->stride = bnd->bstride;
        a->zero_stride = bnd->bstride == 0;
        a->divisor = bnd->bdivisor;
        if (old != bnd->bbuffer)
            ogles2_buffer_release(ctx, old); /* a vertex array object's deleted buffer */
        return TRUE;
    }
    vab_refresh(ctx, index, old); /* old released there: a vertex array object's deleted buffer */
    return TRUE;
}

/* glVertexAttribPointer with GLES2's types, its stride within ES 3.1's
 * limit, every attribute on its own binding (no glVertexAttribBinding
 * moved one): set_pointer's work without its checks for ES 3's types and
 * bindings, and with no call but to free a deleted buffer the attribute had
 * (a Quake III frame sets 250 pointers; tests/perf/drawbench). A client
 * array is taken here only while no vertex array object can be bound
 * (none was ever made). Anything else, errors included, goes to
 * set_pointer. */
/* set_pointer with glVertexAttribPointer's own arguments: a jump from it,
 * no frame or saved registers there for the call */
static __attribute__((noinline)) void vap_slow(struct OGLES3Context *ctx, GLuint index, GLint size, GLenum type,
                                               GLboolean normalized, GLsizei stride, const void *pointer)
{
    set_pointer(ctx, index, size, type, normalized, stride, pointer, GL_FALSE);
}

/* GLES2's attribute types' sizes from GL_BYTE (0x1400) on: GL_BYTE,
 * GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_FLOAT (0x1406), GL_FIXED
 * (0x140C); 0 for the others (set_pointer's) */
static const UBYTE vap_type_size[16] = { 1, 1, 2, 2, 0, 0, 4, 0, 0, 0, 0, 0, 4, 0, 0, 0 };

void ogles2_glVertexAttribPointer(struct OGLES3Context *ctx, GLuint index, GLint size, GLenum type,
                                  GLboolean normalized, GLsizei stride, const void *pointer)
{
    struct OGLES3Array *a;
    struct OGLES3Buffer *nb, *old;
    /* The checks as one value, without branches: Emu68 pays for each
     * branch about what for the work between two (tests/perf/drawbench's
     * samples fall evenly on the blocks) */
    ULONG t = (ULONG)type - GL_BYTE, ts = vap_type_size[t & 15], st;
    ULONG bad = (ULONG)(t > 15) | (ULONG)(ts == 0) | (ULONG)(index >= OGLES3_MAX_VERTEX_ATTRIBS) |
                (ULONG)((ULONG)(size - 1) > 3) | (ULONG)((ULONG)stride > 2048);
    if (ogles2_entry_valid(ctx) && !(bad | ctx->vab_moved)) {
        a = &ctx->array[index];
        nb = ogles2_buffer_array_inl(ctx); /* GLES 2.0 2.9: captured now */
        if (nb || !pointer || !ctx->vao) {
            st = (ULONG)stride | ((ULONG)-(LONG)(stride == 0) & (ts * (ULONG)size)); /* 0: packed */
            a->size = size;
            a->type = type;
            a->normalized = (GLboolean)(normalized != 0);
            a->integer = GL_FALSE;
            a->reloffset = 0;
            a->ustride = stride;
            a->binding = (UBYTE)index;
            old = a->bbuffer;
            a->bbuffer = a->buffer = nb;
            a->boffset = a->pointer = pointer;
            a->bstride = a->stride = (GLsizei)st;
            a->zero_stride = GL_FALSE;
            a->divisor = a->bdivisor;
            if (old != nb && old && OGLES3_BH(old)->deleted)
                ogles2_buffer_release(ctx, old); /* a vertex array object's deleted buffer */
            return;
        }
    }
    vap_slow(ctx, index, size, type, normalized, stride, pointer);
}

/* ES 3.0's integer arrays: read as integers by an integer input. */
void ogles2_glVertexAttribIPointer(struct OGLES3Context *ctx, GLuint index, GLint size, GLenum type, GLsizei stride,
                                   const void *pointer)
{
    if (!ogles2_es3_entry(ctx))
        return;
    if (type != GL_BYTE && type != GL_UNSIGNED_BYTE && type != GL_SHORT && type != GL_UNSIGNED_SHORT &&
        type != GL_INT && type != GL_UNSIGNED_INT) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    set_pointer(ctx, index, size, type, GL_FALSE, stride, pointer, GL_TRUE);
}

/* ES 3.1 2.9.1: glVertexAttribDivisor is the attribute on binding index,
 * that binding's divisor set. */
static void attrib_divisor(struct OGLES3Context *ctx, GLuint index, GLuint divisor)
{
    GLuint old;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    old = ctx->array[index].binding;
    ctx->array[index].binding = (UBYTE)index;
    ctx->array[index].bdivisor = divisor;
    vab_refresh(ctx, index, 0);
    if (old != index)
        vab_refresh(ctx, old, 0);
}

/* ---- ES 3.1's vertex attribute binding (stage D item 5) ---- */

/* An ES 3.1 context with a vertex array object other than 0 bound: ES 3.1
 * 10.3.1 (and Mesa for ES 3.1, as for core GL) refuses the entries
 * below with object 0 (GL_INVALID_OPERATION). */
static BOOL vab_entry(struct OGLES3Context *ctx)
{
    if (!ogles2_es31_entry(ctx))
        return FALSE;
    if (!ogles2_vao_bound(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    return TRUE;
}

void ogles2_glBindVertexBuffer(struct OGLES3Context *ctx, GLuint bindingindex, GLuint buffer, GLintptr offset,
                               GLsizei stride)
{
    struct OGLES3Array *bnd;
    struct OGLES3Buffer *b = 0, *old;
    if (!vab_entry(ctx))
        return;
    if (bindingindex >= OGLES3_MAX_VERTEX_ATTRIBS || offset < 0 || stride < 0 || stride > 2048) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (buffer && !(b = ogles2_buffer_get(ctx, buffer))) { /* not a name glGenBuffers gave */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    bnd = &ctx->array[bindingindex];
    old = bnd->bbuffer;
    bnd->bbuffer = b;
    bnd->boffset = (const void *)offset;
    bnd->bstride = stride;
    vab_refresh(ctx, bindingindex, old);
}

/* glVertexAttribFormat and glVertexAttribIFormat (`integer`). */
static void attrib_format(struct OGLES3Context *ctx, GLuint index, GLint size, GLenum type, GLboolean normalized,
                          GLuint relativeoffset, GLboolean integer)
{
    struct OGLES3Array *a;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS || size < 1 || size > 4 || relativeoffset > 2047) {
        ogles2_error(ctx, GL_INVALID_VALUE); /* GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET */
        return;
    }
    if (!type_size(type) || (integer && type != GL_BYTE && type != GL_UNSIGNED_BYTE && type != GL_SHORT &&
                             type != GL_UNSIGNED_SHORT && type != GL_INT && type != GL_UNSIGNED_INT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (packed_type(type) && size != 4) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    a = &ctx->array[index];
    a->size = size;
    a->type = type;
    a->normalized = normalized ? GL_TRUE : GL_FALSE;
    a->integer = integer;
    a->reloffset = relativeoffset;
    vab_refresh(ctx, a->binding, 0);
}

void ogles2_glVertexAttribFormat(struct OGLES3Context *ctx, GLuint attribindex, GLint size, GLenum type,
                                 GLboolean normalized, GLuint relativeoffset)
{
    if (vab_entry(ctx))
        attrib_format(ctx, attribindex, size, type, normalized, relativeoffset, GL_FALSE);
}

void ogles2_glVertexAttribIFormat(struct OGLES3Context *ctx, GLuint attribindex, GLint size, GLenum type,
                                  GLuint relativeoffset)
{
    if (vab_entry(ctx))
        attrib_format(ctx, attribindex, size, type, GL_FALSE, relativeoffset, GL_TRUE);
}

void ogles2_glVertexAttribBinding(struct OGLES3Context *ctx, GLuint attribindex, GLuint bindingindex)
{
    GLuint old;
    if (!vab_entry(ctx))
        return;
    if (attribindex >= OGLES3_MAX_VERTEX_ATTRIBS || bindingindex >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    old = ctx->array[attribindex].binding;
    ctx->array[attribindex].binding = (UBYTE)bindingindex;
    if (bindingindex != attribindex)
        ctx->vab_moved = TRUE; /* for good: set_pointer's short way is closed */
    vab_refresh(ctx, bindingindex, 0);
    if (old != bindingindex)
        vab_refresh(ctx, old, 0);
}

void ogles2_glVertexBindingDivisor(struct OGLES3Context *ctx, GLuint bindingindex, GLuint divisor)
{
    if (!vab_entry(ctx))
        return;
    if (bindingindex >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->array[bindingindex].bdivisor = divisor;
    vab_refresh(ctx, bindingindex, 0);
}

/* glGetIntegeri_v's GL_VERTEX_BINDING_* (state.c asks): -1 another query,
 * 0 an error set, 1 *v. */
int ogles2_vab_query(struct OGLES3Context *ctx, GLenum pname, GLuint index, GLint64 *v)
{
    const struct OGLES3Array *bnd;
    if (pname != GL_VERTEX_BINDING_BUFFER && pname != GL_VERTEX_BINDING_OFFSET && pname != GL_VERTEX_BINDING_STRIDE &&
        pname != GL_VERTEX_BINDING_DIVISOR)
        return -1;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    bnd = &ctx->array[index];
    *v = pname == GL_VERTEX_BINDING_BUFFER   ? (GLint64)ogles2_buffer_name(bnd->bbuffer)
       : pname == GL_VERTEX_BINDING_OFFSET   ? (GLint64)(ULONG)bnd->boffset
       : pname == GL_VERTEX_BINDING_STRIDE   ? (GLint64)bnd->bstride
                                             : (GLint64)bnd->bdivisor;
    return 1;
}

void ogles2_glVertexAttribDivisor(struct OGLES3Context *ctx, GLuint index, GLuint divisor)
{
    if (ogles2_es3_entry(ctx))
        attrib_divisor(ctx, index, divisor);
}

/* EXT_instanced_arrays (GLES2 contexts; ES3 ones have both) */
void ogles2_glVertexAttribDivisorEXT(struct OGLES3Context *ctx, GLuint index, GLuint divisor)
{
    if (ogles2_context_valid(ctx))
        attrib_divisor(ctx, index, divisor);
}

static void enable_array(struct OGLES3Context *ctx, GLuint index, GLboolean on)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->array[index].enabled = on;
}

void ogles2_glEnableVertexAttribArray(struct OGLES3Context *ctx, GLuint index)
{
    enable_array(ctx, index, GL_TRUE);
}

void ogles2_glDisableVertexAttribArray(struct OGLES3Context *ctx, GLuint index)
{
    enable_array(ctx, index, GL_FALSE);
}

void ogles2_glGetVertexAttribiv(struct OGLES3Context *ctx, GLuint index, GLenum pname,
                                GLint *params)
{
    const struct OGLES3Array *a;
    int k;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    a = &ctx->array[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED:        *params = a->enabled; break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE:           *params = a->size; break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE:         *params = a->ustride; break; /* as given (glVertexAttribPointer) */
    case GL_VERTEX_ATTRIB_BINDING: /* ES 3.1 */
    case GL_VERTEX_ATTRIB_RELATIVE_OFFSET:
        if (!ogles2_es31(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        *params = pname == GL_VERTEX_ATTRIB_BINDING ? (GLint)a->binding : (GLint)a->reloffset;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE:           *params = (GLint)a->type; break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:     *params = a->normalized; break;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: *params = (GLint)ogles2_buffer_name(a->buffer); break;
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: /* also EXT_instanced_arrays' */
        *params = (GLint)a->divisor;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
        if (!ogles2_es3(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        *params = a->integer;
        break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (k = 0; k < 4; k++)
            params[k] = ctx->state.attrib_kind[index] ? (GLint)ctx->state.attrib_word[index][k]
                                                      : (GLint)ctx->state.attrib[index][k];
        break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

/* ES 3.0: the same, a current value as the integers it was given. */
void ogles2_glGetVertexAttribIiv(struct OGLES3Context *ctx, GLuint index, GLenum pname, GLint *params)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glGetVertexAttribiv(ctx, index, pname, params);
}

void ogles2_glGetVertexAttribIuiv(struct OGLES3Context *ctx, GLuint index, GLenum pname, GLuint *params)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glGetVertexAttribiv(ctx, index, pname, (GLint *)params);
}

void ogles2_glGetVertexAttribPointerv(struct OGLES3Context *ctx, GLuint index, GLenum pname,
                                      void **pointer)
{
    if (!ogles2_context_valid(ctx) || !pointer)
        return;
    if (index >= OGLES3_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    *pointer = (void *)ctx->array[index].pointer;
}

/* ---- per-context draw state ---- */

static struct OGLES3Draw *draw_state(struct OGLES3Context *ctx)
{
    struct OGLES3Draw *d = ctx->draw;
    if (!d) {
        if (!(d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR)))
            return 0;
        d->dd.gen = ~0UL;
        ctx->draw = d;
    }
    return d;
}

/* The GPU address of a variant's code. A loaded image carries its own; the
 * built-in one is uploaded once per context (it is static data). */
static ULONG code_address(struct OGLES3Draw *d, const struct OGLES3Image *im,
                          const struct OGLES3Variant *v)
{
    if (im->code_mem)
        return (ULONG)im->code_mem->hostptr + v->code_offset;
    if (im != ogles2_builtin_image())
        return 0; /* loaded without a GPU: never drawn */
    if (!d->builtin_code.hostptr) {
        ULONG len;
        UWORD i;
        if (v3d_mem_alloc(ogles2_device(), &d->builtin_code, im->code_total) < 0) {
            d->builtin_code.hostptr = 0;
            return 0;
        }
        for (i = 0; i < im->nvariants; i++)
            memcpy((UBYTE *)d->builtin_code.hostptr + im->variants[i].code_offset,
                   im->variants[i].code, im->variants[i].code_bytes);
        len = d->builtin_code.size;
        CachePreDMA(d->builtin_code.hostptr, &len, 0);
    }
    return (ULONG)d->builtin_code.hostptr + v->code_offset;
}

void ogles2_draw_free(struct OGLES3Context *ctx)
{
    struct OGLES3Draw *d = ctx->draw;
    if (!d)
        return;
    if (d->builtin_code.hostptr)
        v3d_mem_free(ogles2_device(), &d->builtin_code);
    if (d->spill.hostptr)
        v3d_mem_free(ogles2_device(), &d->spill);
    if (d->zero_ubo.hostptr)
        v3d_mem_free(ogles2_device(), &d->zero_ubo);
    if (d->quad_ready)
        ogles2_program_builtin_free(&d->quad);
    if (d->mrt_ready)
        ogles2_program_builtin_free(&d->mrt_quad);
    if (d->mrt_image)
        ogles2_image_free(d->mrt_image, ctx);
    if (d->zs_ready)
        ogles2_program_builtin_free(&d->zs_quad);
    if (d->zs_image)
        ogles2_image_free(d->zs_image, ctx);
    if (d->copy_ready)
        ogles2_program_builtin_free(&d->copy_prog);
    if (d->copy_image)
        ogles2_image_free(d->copy_image, ctx);
    FreeVec(d);
    ctx->draw = 0;
}

/* ---- state buffer claims ---- */

/* `size` bytes of the job's state buffer at `align` (a power of two; the
 * blocks themselves are 64-byte aligned). NULL when memory ran out. */
static void *claim(V3DContext *v, ULONG size, ULONG align)
{
    v3d_u8 s = v->build_slot;
    v3d_static_buffer *sb = &v->state_buf[s];
    sb->used = (sb->used + (int)align - 1) & ~((int)align - 1);
    return v3d_cl_claim_fast(ogles2_device(), &v->state_mem[s], sb, size, &v->frame);
}

static ULONG le32(ULONG x)
{
    return (x >> 24) | ((x >> 8) & 0xff00UL) | ((x << 8) & 0xff0000UL) | (x << 24);
}

static ULONG float_bits(GLfloat f)
{
    union { GLfloat f; ULONG u; } c;
    c.f = f;
    return c.u;
}

/* EXT_clip_control's ZERO_TO_ONE (docs/M7_EXTENSIONS_PLAN.md step 4). V3D
 * 4.2 clips clip-space z to -w..w only: its window-space Z planes clamp
 * (the m7-3 probe). A program compiled with Mesa patch 0013 turns z into
 * 2z - w itself (OGLES3_U_CLIP_Z_COEFF -1), which the hardware clips and
 * maps as -w..w: the viewport stays GL's. An older binary gets the 0..w
 * mapping from the viewport instead: its depth is right, but what lies
 * before the near plane is drawn at depth n. */
static BOOL zero_to_one_in_viewport(const struct OGLES3Context *ctx)
{
    return ctx->state.clip_depth == GL_ZERO_TO_ONE_EXT &&
           !(ctx->program && ctx->program->image && ctx->program->image->clip_z_coeff);
}

/* The viewport's Z scale and offset: clip-space z from -w..w to the depth
 * range, or from 0..w (Mesa's _mesa_get_viewport_xform). */
static void viewport_z(const struct OGLES3Context *ctx, float *zs, float *zo)
{
    const struct OGLES3State *s = &ctx->state;
    if (zero_to_one_in_viewport(ctx)) {
        *zs = s->depth_range[1] - s->depth_range[0];
        *zo = s->depth_range[0];
    } else {
        *zs = (s->depth_range[1] - s->depth_range[0]) * 0.5f;
        *zo = (s->depth_range[1] + s->depth_range[0]) * 0.5f;
    }
}

/* The picture is drawn top-down in the framebuffer's rows: the default
 * framebuffer (plan 7.11), unless EXT_clip_control's upper-left origin turns
 * it over again; a framebuffer object only with that origin. A negative Y
 * scale, and the winding of front faces reversed (Mesa's state tracker). */
static BOOL y_down(const struct OGLES3Context *ctx)
{
    return ctx->fb_flip ^ (ctx->state.clip_origin == GL_UPPER_LEFT_EXT);
}

/* The program's state variables (FORMAT.md STAGE_PARAMS kind 2), computed
 * into its constant buffers before a draw, as Mesa's prog_statevars.c does
 * for a framebuffer drawn upside down (FlipY: the default framebuffer). */
static void update_state_vars(struct OGLES3Context *ctx, struct OGLES3Program *p)
{
    const struct OGLES3Image *im = p->image;
    const GLfloat *dr = ctx->state.depth_range;
    UWORD i;
    for (i = 0; i < im->nstatevars; i++) {
        const struct OGLES3StateVar *sv = &im->statevars[i];
        GLfloat v[4];
        int k;
        switch (sv->state) {
        case OGLES3_STATE_DEPTH_RANGE:
            v[0] = dr[0];
            v[1] = dr[1];
            v[2] = dr[1] - dr[0];
            v[3] = 1.0f;
            break;
        case OGLES3_STATE_FB_WPOS_Y_TRANSFORM:
            /* Mesa's prog_statevars.c: flipped (-1, height, 1, 0), else
             * (1, 0, -1, height) */
            v[0] = ctx->fb_flip ? -1.0f : 1.0f;
            v[1] = ctx->fb_flip ? (GLfloat)ctx->fb_h : 0.0f;
            v[2] = ctx->fb_flip ? 1.0f : -1.0f;
            v[3] = ctx->fb_flip ? 0.0f : (GLfloat)ctx->fb_h;
            break;
        case OGLES3_STATE_FB_PNTC_Y_TRANSFORM:
            /* upper-left sprite origin: inverted on a flipped buffer only */
            v[0] = ctx->fb_flip ? -1.0f : 1.0f;
            v[1] = ctx->fb_flip ? 1.0f : 0.0f;
            v[2] = 0.0f;
            v[3] = 0.0f;
            break;
        default:
            continue;
        }
        for (k = 0; k < 4; k++)
            if (p->cbuf[sv->stage][sv->offset + k] != float_bits(v[k]))
                ogles2_cbuf_set(p, sv->stage, sv->offset + k, float_bits(v[k]));
    }
}

/* Texture and sampler state per texture index of each stage (the TMU
 * recipes' unit field), [0] vertex, [1] fragment; 0 where unused. */
struct tex_states
{
    ULONG tex[2][OGLES3_MAX_STAGE_SAMPLERS], smp[2][OGLES3_MAX_STAGE_SAMPLERS];
    ULONG ubo[2];   /* little-endian copy of each stage's constant buffer, if read */
    ULONG zero_ubo; /* what a uniform block without a buffer reads */
    UBYTE unit[2][OGLES3_MAX_STAGE_SAMPLERS], kind[2][OGLES3_MAX_STAGE_SAMPLERS]; /* textureSize's */
    /* Not cleared at a draw (TEX_STATES_CLEAR): only image_states writes
     * them and only image recipes, of variants it ran for, read them. */
    ULONG img[2][OGLES3_MAX_STAGE_IMAGES]; /* ES 3.1: each stage's images' texture states */
    UBYTE iunit[2][OGLES3_MAX_STAGE_IMAGES]; /* and their image units */
};

/* The part of struct tex_states a draw starts with zeroed: 2/3 of it is
 * the images', which most draws never read. */
#define TEX_STATES_CLEAR offsetof(struct tex_states, img)

/* The largest uniform block (GL_MAX_UNIFORM_BLOCK_SIZE). */
#define ZERO_UBO_BYTES 65536

/* QUNIFORM_UBO_ADDR (indirectly indexed uniform arrays, plan 7.8): the
 * shader reads its constant buffer through the TMU, so a stage whose recipe
 * asks for it gets a little-endian copy in the state buffer. Made before any
 * uniform stream (a claim must not happen while another is half filled). */
static BOOL ubo_copies(struct OGLES3Context *ctx, const struct OGLES3Program *p,
                       const struct OGLES3Variant *const v[3], struct tex_states *ts)
{
    int k, si;
    for (k = 0; k < 3; k++) {
        si = v[k]->stage == OGLES3_VARIANT_FS ? 1 : 0;
        if (ts->ubo[si])
            continue;
        if (v[k]->uses_ubo) {
            UWORD n = p->image->cbuf_words[si];
            ULONG *c = claim(ctx->v3d, n * 4UL + 16, 16);
            if (!c)
                return FALSE;
            memcpy(c, p->cbuf_le[si], n * 4UL);
            ts->ubo[si] = (ULONG)c;
        }
        /* A uniform block (GLSL ES 3.00) with no buffer bound reads zeros:
         * one block of the context's, made once. Only the GPU reads it; the
         * cache clean before every submission covers the zeros written
         * here. */
        if ((v[k]->uses_blocks || v[k]->uses_storage) && !ts->zero_ubo) {
            struct OGLES3Draw *d = ctx->draw;
            if (!d->zero_ubo.hostptr) {
                if (v3d_mem_alloc(ogles2_device(), &d->zero_ubo, ZERO_UBO_BYTES + 256) < 0) {
                    memset(&d->zero_ubo, 0, sizeof(d->zero_ubo));
                    return FALSE;
                }
                memset(d->zero_ubo.hostptr, 0, ZERO_UBO_BYTES + 256);
                ogles2_log("draw: a uniform block without a buffer bound reads zeros (logged once)");
            }
            ts->zero_ubo = (ULONG)V3D_ALIGN_UP((v3d_uintptr)d->zero_ubo.hostptr, 256);
        }
    }
    return TRUE;
}

/* A fragment shader's storage buffer k (ES 3.1; Mesa's SSBO k of the
 * stage, FORMAT.md's map: a storage block, or 0x8000 | an atomic counter
 * buffer binding): the range bound there, which the job then writes
 * (job_storage: its caches cleaned after the render). Without one, size 0
 * at the zeroed block: the bounds checks every such program has (Mesa
 * patch 0017) drop its accesses there. *size: the range's bytes. */
static ULONG draw_storage(struct OGLES3Context *ctx, const struct OGLES3Program *p, int si, ULONG k,
                          const struct tex_states *tst, ULONG *size)
{
    const struct OGLES3Image *im = p->image;
    ULONG a = 0;
    UWORD e;
    *size = 0;
    if (k < im->stage_nsblocks[si]) {
        e = im->stage_sblock[si][k];
        a = ogles2_buffer_storage_draw(ctx, (e & 0x8000) != 0, (e & 0x8000) ? e & 0x7fff : im->sblocks[e].binding,
                                       size);
    }
    if (a) {
        ctx->job_storage = TRUE;
        return a;
    }
    *size = 0;
    return tst->zero_ubo;
}

/* The buffer a stage's uniform block k (its block map's: Mesa's UBO k + 1)
 * reads: the range bound at the block's binding (§5 item 6), else the
 * zeroed block (ubo_copies). *size (if asked): the range's bytes, or the
 * block's own size without one. */
static ULONG block_buffer(struct OGLES3Context *ctx, const struct OGLES3Program *p, int si, ULONG k,
                          const struct tex_states *tst, ULONG *size)
{
    const struct OGLES3Image *im = p->image;
    ULONG a, n = 0;
    UWORD b;
    if (size)
        *size = 0;
    if (k >= im->stage_nblocks[si])
        return tst->zero_ubo;
    b = im->stage_block[si][k];
    if (p->block_binding && (a = ogles2_buffer_uniform(ctx, p->block_binding[b], &n)) != 0) {
        if (size)
            *size = n;
        return a;
    }
    if (size)
        *size = im->ublocks[b].size;
    return tst->zero_ubo;
}

/* A buffer transform feedback wrote earlier in the open job, read by the
 * draw's vertex stage as a uniform block (its GPU words, which feedback
 * writes): the feedback flushed and waited for first, Mesa's
 * v3d_emit_wait_for_tf_if_needed (v3dx_draw.c). The fragment stage needs no
 * wait: it runs after binning, feedback's included. Vertex and index reads
 * of such a buffer go through the CPU, which renders the job first
 * (gpu_written). One wait covers every feedback draw before it. */
static void xfb_wait(struct OGLES3Context *ctx, const struct OGLES3Program *p)
{
    const struct OGLES3Image *im = p->image;
    UBYTE *b;
    UBYTE k;
    if (!ctx->draw_fed) { /* else vertices, indices or a command fed back: read by the binner */
        if (!p->block_binding)
            return;
        for (k = 0; k < im->stage_nblocks[0]; k++)
            if (ogles2_buffer_uniform_fed_back(ctx, p->block_binding[im->stage_block[0][k]]))
                break;
        if (k == im->stage_nblocks[0])
            return;
    }
    if (!(b = v3d_buffer_claim_memory(ctx->v3d->current_buf, 3)))
        return;
    b[0] = v3d_OP_FLUSH_TRANSFORM_FEEDBACK_DATA;
    b[1] = v3d_OP_WAIT_FOR_TRANSFORM_FEEDBACK;
    b[2] = 255; /* every outstanding block (Mesa's) */
    ctx->xfb_waited = ctx->xfb_draws;
}

/* Register spilling (Mesa's QUNIFORM_SPILL_*): a shader that ran out of
 * registers keeps values in a scratch area through the TMU (dEQP-GLES2's
 * dynamically indexed arrays), a slice per QPU thread. The context's area,
 * as Mesa's v3d_update_compiled_*: a shader picks its slice by TIDX,
 * (core << 6) | (qpu << 2) | thread, so QPUs x 4 slices of the largest spill
 * size drawn with yet. Only the GPU touches it. A smaller old area goes when
 * the open job has rendered (its draws may use it). */
static BOOL spill_area(struct OGLES3Context *ctx, const struct OGLES3Variant *const v[3])
{
    struct OGLES3Draw *d = ctx->draw;
    ULONG need = 0;
    int k;
    for (k = 0; k < 3; k++)
        if (v[k]->spill_size > need)
            need = v[k]->spill_size;
    if (need <= d->spill_size)
        return TRUE;
    if (d->spill.hostptr) {
        v3d_frame_defer_free(&ctx->v3d->frame, &d->spill);
        memset(&d->spill, 0, sizeof(d->spill));
    }
    d->spill_size = 0;
    if (v3d_mem_alloc(ogles2_device(), &d->spill,
                      (ULONG)ogles2_device()->deviceInfo.qpu_count * 4 * need + 256) < 0) {
        memset(&d->spill, 0, sizeof(d->spill));
        return FALSE;
    }
    d->spill_size = need;
    ogles2_log("draw: a shader spills registers: a %ld-byte area (%ld bytes a thread)",
               (LONG)d->spill.size, (LONG)need);
    return TRUE;
}

/* The image's sampler uniforms, listed at the program's first draw (most of
 * a fixed-function emulation's uniforms are not samplers): how many to look
 * at, p->samplers[k] or (0xfe, too many to list) every uniform. */
static UWORD list_samplers(struct OGLES3Program *p)
{
    const struct OGLES3Image *im = p->image;
    UWORD u, n;
    if (p->nsamplers == 0xff) {
        for (u = 0, n = 0; u < im->nuniforms && n <= 16; u++)
            if (im->uniforms[u].flags & OGLES3_UNIFORM_SAMPLER) {
                if (n < 16)
                    p->samplers[n] = u;
                n++;
            }
        p->nsamplers = n <= 16 ? (UBYTE)n : 0xfe;
    }
    return p->nsamplers == 0xfe ? im->nuniforms : p->nsamplers;
}

/* The texture unit of element e of sampler uniform u (glUniform1i). */
static UBYTE unit_of(const struct OGLES3Program *p, UWORD u, UWORD e)
{
    return p->uniform_loc[u] == 0xFFFF ? 0 : p->sampler_unit[p->uniform_loc[u] + e];
}

/* A fragment-shader sampler of the program reads a texture the TMU returns
 * 32 bits for (a float or depth texture): the draw needs the 32-bit
 * variants (Mesa's sampler_is_32b key). */
static BOOL wants_wide(struct OGLES3Context *ctx, struct OGLES3Program *p)
{
    const struct OGLES3Image *im = p->image;
    UWORD k, n = list_samplers(p);
    for (k = 0; k < n; k++) {
        UWORD u = p->nsamplers == 0xfe ? k : p->samplers[k];
        const struct OGLES3ImageUniform *un = &im->uniforms[u];
        UWORD e, ne = un->array ? un->array : 1;
        if (!(un->flags & OGLES3_UNIFORM_SAMPLER) || un->sampler[1] >= OGLES3_MAX_STAGE_SAMPLERS)
            continue;
        for (e = 0; e < ne; e++)
            if (ogles2_texture_wide(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type)))
                return TRUE;
    }
    return FALSE;
}

/* Every sampler uniform of the program: its texture index in each stage
 * that uses it reads the texture unit glUniform1i gave it. */
static BOOL texture_states(struct OGLES3Context *ctx, struct OGLES3Program *p,
                           struct tex_states *ts)
{
    const struct OGLES3Image *im = p->image;
    UWORD u, k, n;
    int st;
    n = list_samplers(p);
    for (k = 0; k < n; k++) {
        const struct OGLES3ImageUniform *un;
        u = p->nsamplers == 0xfe ? k : p->samplers[k];
        un = &im->uniforms[u];
        if (!(un->flags & OGLES3_UNIFORM_SAMPLER))
            continue;
        for (st = 0; st < 2; st++) {
            UWORD e, ne = un->array ? un->array : 1;
            for (e = 0; e < ne; e++) { /* an array: an element a texture index */
                ULONG idx = un->sampler[st] + e;
                if (un->sampler[st] >= OGLES3_MAX_STAGE_SAMPLERS || idx >= OGLES3_MAX_STAGE_SAMPLERS)
                    break;
                ts->unit[st][idx] = unit_of(p, u, e);
                ts->kind[st][idx] = (UBYTE)ogles2_sampler_kind(un->type);
                if (!ogles2_texture_state(ctx, ts->unit[st][idx], ts->kind[st][idx], &ts->tex[st][idx],
                                          &ts->smp[st][idx]))
                    return FALSE;
            }
        }
    }
    return TRUE;
}

/* ES 3.1: every image uniform of the program, when a variant of the draw
 * reads images: its image index in each stage that uses it reads the image
 * unit its layout(binding) gave it, which the draw may store into (the job
 * then cleans its caches after the render, job_storage). */
static BOOL image_states(struct OGLES3Context *ctx, struct OGLES3Program *p, const struct OGLES3Variant *const v[3],
                         struct tex_states *ts)
{
    const struct OGLES3Image *im = p->image;
    UWORD u, e;
    int st;
    if (!v[0]->uses_images && !v[1]->uses_images && !v[2]->uses_images)
        return TRUE;
    for (u = 0; u < im->nuniforms; u++) {
        const struct OGLES3ImageUniform *un = &im->uniforms[u];
        UWORD ne = un->array ? un->array : 1;
        if (!(un->flags & OGLES3_UNIFORM_IMAGE))
            continue;
        for (st = 0; st < 2; st++)
            for (e = 0; e < ne && un->sampler[st] + e < OGLES3_MAX_STAGE_IMAGES; e++) {
                ULONG idx = un->sampler[st] + e;
                UBYTE unit = unit_of(p, u, e);
                ts->iunit[st][idx] = unit;
                if (!(ts->img[st][idx] = ogles2_image_state(ctx, unit, 0)))
                    return FALSE;
                ogles2_image_written(ctx, unit, ctx->job_serial);
            }
    }
    ctx->job_storage = TRUE;
    return TRUE;
}

/* One shader's uniform stream from its recipe (plan section 8.6, Mesa's
 * v3d_write_uniforms); its address. One word more than the recipe: the
 * hardware prefetches the next uniform. */
/* The uniforms of write_uniforms' recipes most shaders never have (ES 3's
 * images, uniform blocks, sizes and storage; spilling): a function of
 * their own, so its loop stays as small as 1.x's. GCC compiled that loop
 * worse with each case added (DEVELOPMENT_PLAN.md section 13, bb8665d: a
 * value in the frame instead of a register, 20-40% slower). */
static __attribute__((noinline)) ULONG rare_uniform(struct OGLES3Context *ctx, const struct OGLES3Program *p, int si,
                                                     ULONG type, ULONG d, const struct tex_states *tst)
{
    ULONG w = 0;
    switch (type) {
    case OGLES3_U_IMAGE_TMU_CONFIG_P0: /* Mesa's write_image_tmu_p0 (the loader checked the index) */
        return tst->img[si][d >> 24] | (d & 0xffffff);
    case OGLES3_U_IMAGE_WIDTH:
    case OGLES3_U_IMAGE_HEIGHT:
    case OGLES3_U_IMAGE_DEPTH:
    case OGLES3_U_IMAGE_ARRAY_SIZE:
        return ogles2_image_size(ctx, tst->iunit[si][d], (int)(type - OGLES3_U_IMAGE_WIDTH));
    case OGLES3_U_UBO_ADDR: /* a uniform block (unit 0, the constant buffer, is write_uniforms') */
        return block_buffer(ctx, p, si, (d >> 24) - 1, tst, 0) + (d & 0xffffff);
    case OGLES3_U_GET_UBO_SIZE: /* Mesa's cb[data].buffer_size, cb[0] the constant buffer */
        w = p->image->cbuf_words[si] * 4UL;
        if (d)
            block_buffer(ctx, p, si, d - 1, tst, &w);
        return w;
    case OGLES3_U_TEXTURE_WIDTH:
    case OGLES3_U_TEXTURE_HEIGHT:
    case OGLES3_U_TEXTURE_DEPTH:
    case OGLES3_U_TEXTURE_ARRAY_SIZE:
        d &= OGLES3_MAX_STAGE_SAMPLERS - 1;
        return ogles2_texture_size(ctx, tst->unit[si][d], tst->kind[si][d], (int)(type - OGLES3_U_TEXTURE_WIDTH));
    case OGLES3_U_SPILL_OFFSET:
        return (ULONG)V3D_ALIGN_UP((v3d_uintptr)ctx->draw->spill.hostptr, 256);
    case OGLES3_U_SPILL_SIZE_PER_THREAD:
        return ctx->draw->spill_size;
    case OGLES3_U_PISTORM_VP_WIDTH:
        return float_bits((GLfloat)ctx->viewport[2]);
    case OGLES3_U_PISTORM_VP_HEIGHT:
        return float_bits((GLfloat)ctx->viewport[3]);
    case OGLES3_U_SSBO_OFFSET: { /* a fragment shader's storage buffer (ES 3.1) */
        ULONG bytes;
        return draw_storage(ctx, p, si, d, tst, &bytes);
    }
    case OGLES3_U_GET_SSBO_SIZE:
        draw_storage(ctx, p, si, d, tst, &w);
        return w;
    default:
        return 0; /* the loader refuses the rest */
    }
}

/* write_uniforms' kinds of recipe entry, by type (below 48): 0 the rare
 * ones (rare_uniform), 1 a constant, 2 a uniform, 3 a value of the draw's
 * state (made before the loop), 4 and 5 a texture's two TMU words, 6 a
 * constant buffer address. A table and a small switch keep the loop's
 * values in registers (tests/perf/drawbench: GCC kept the type and data
 * pointers in the frame with the switch of every case). */
static const UBYTE u_kind[U_KINDS] = {
    [OGLES3_U_CONSTANT] = 1,          [OGLES3_U_UNIFORM] = 2,
    [OGLES3_U_VIEWPORT_X_SCALE] = 3,  [OGLES3_U_VIEWPORT_Y_SCALE] = 3,
    [OGLES3_U_VIEWPORT_Z_OFFSET] = 3, [OGLES3_U_VIEWPORT_Z_SCALE] = 3,
    [OGLES3_U_CLIP_Z_COEFF] = 3,      [OGLES3_U_LINE_WIDTH] = 3,
    [OGLES3_U_AA_LINE_WIDTH] = 3,     [OGLES3_U_BLEND_CONSTANT_R] = 3,
    [OGLES3_U_BLEND_CONSTANT_R + 1] = 3, [OGLES3_U_BLEND_CONSTANT_R + 2] = 3,
    [OGLES3_U_BLEND_CONSTANT_R + 3] = 3,
    [OGLES3_U_TMU_CONFIG_P0] = 4,     [OGLES3_U_TMU_CONFIG_P1] = 5,
    [OGLES3_U_UBO_ADDR] = 6,
};

/* The draw's state values, kind 3 (and PiStorm3D's viewport sizes after
 * them): made again only when that state changes (uniform_state's version,
 * the blend colour, the program's Z clip), not for each stream (their
 * float work was most of write_uniforms' time: tests/perf/drawbench). The
 * default framebuffer is drawn upside down (plan 7.11), a negative Y scale,
 * as Mesa's state tracker gives a window; a framebuffer object is not
 * (y_down). The draw has its OGLES3Draw (draw_arrays). */
static __attribute__((noinline)) const ULONG *state_fx(struct OGLES3Context *ctx)
{
    struct OGLES3Draw *d = ctx->draw;
    const struct OGLES3State *s = &ctx->state;
    const ULONG *bc = (const ULONG *)s->blend_color;
    BOOL z1 = zero_to_one_in_viewport(ctx);
    ULONG *fx = d->fx;
    float zs, zo;
    int k;
    if (d->fx_ok && d->fx_ver == ctx->ustate_ver && d->fx_z1 == z1 && d->fx_bc[0] == bc[0] &&
        d->fx_bc[1] == bc[1] && d->fx_bc[2] == bc[2] && d->fx_bc[3] == bc[3])
        return fx;
    viewport_z(ctx, &zs, &zo);
    fx[OGLES3_U_VIEWPORT_X_SCALE] = float_bits(ctx->viewport[2] * 0.5f * 256.0f);
    fx[OGLES3_U_VIEWPORT_Y_SCALE] = float_bits(ctx->viewport[3] * (y_down(ctx) ? -0.5f : 0.5f) * 256.0f);
    fx[OGLES3_U_VIEWPORT_Z_SCALE] = float_bits(zs);
    fx[OGLES3_U_VIEWPORT_Z_OFFSET] = float_bits(zo);
    fx[OGLES3_U_CLIP_Z_COEFF] = float_bits(s->clip_depth == GL_ZERO_TO_ONE_EXT ? -1.0f : 0.0f);
    fx[OGLES3_U_LINE_WIDTH] = fx[OGLES3_U_AA_LINE_WIDTH] = float_bits(s->line_width);
    for (k = 0; k < 4; k++) {
        fx[OGLES3_U_BLEND_CONSTANT_R + k] = float_bits(s->blend_color[k]);
        d->fx_bc[k] = bc[k];
    }
    /* PiStorm3D's scale_p, the built-in image's (clears drawn as rectangles) */
    fx[U_KINDS] = float_bits((GLfloat)ctx->viewport[2]);
    fx[U_KINDS + 1] = float_bits((GLfloat)ctx->viewport[3]);
    d->fx_ver = ctx->ustate_ver;
    d->fx_z1 = z1;
    d->fx_ok = TRUE;
    return fx;
}

static ULONG write_uniforms(struct OGLES3Context *ctx, const struct OGLES3Program *p,
                            const struct OGLES3Variant *var, const struct tex_states *tst)
{
    int si = var->stage == OGLES3_VARIANT_FS ? 1 : 0;
    const ULONG *rtype = var->rtype, *rdata = var->rdata, *cbuf = p->cbuf[si];
    const ULONG *tex = tst->tex[si], *smp = tst->smp[si];
    const UWORD *special = var->uidx ? var->special : 0;
    const ULONG *fx = 0; /* state_fx's, at the first value that needs them */
    ULONG ubo = tst->ubo[si];
    ULONG *u;
    int i, k, n;
    if (!(u = claim(ctx->v3d, (var->nrecipe + 1) * 4UL, 4)))
        return 0;
    ctx->n_uwords += var->nrecipe;
    /* Most of a stream is constant buffer words, already little-endian:
     * every entry is copied as one (the others from word 0), then the
     * others are written over. The built-in image has no such list. */
    if (var->uidx) {
        const ULONG *cle = p->cbuf_le[si];
        const UWORD *ui = var->uidx;
        ULONG *dst = u;
        for (k = var->nrecipe; k > 0; k--)
            *dst++ = cle[*ui++];
        n = var->nspecial;
    } else {
        n = var->nrecipe;
    }
    for (k = 0; k < n; k++) {
        ULONG d, w, t;
        i = special ? special[k] : k;
        d = rdata[i];
        t = rtype[i];
        switch (t < U_KINDS ? u_kind[t] : t == OGLES3_U_PISTORM_VP_WIDTH ? 7 : t == OGLES3_U_PISTORM_VP_HEIGHT ? 8 : 0) {
        case 7: w = (fx ? fx : (fx = state_fx(ctx)))[U_KINDS]; break;
        case 8: w = (fx ? fx : (fx = state_fx(ctx)))[U_KINDS + 1]; break;
        case 1: w = d; break;
        case 2: w = cbuf[d]; break;
        case 3: w = (fx ? fx : (fx = state_fx(ctx)))[t]; break;
        /* Mesa's write_tmu_p0/p1: the state's address with the
         * compiler's configuration bits in its low bits. */
        case 4: w = tex[(d >> 24) & (OGLES3_MAX_STAGE_SAMPLERS - 1)] | (d & 0xffffff); break;
        case 5: w = smp[(d >> 24) & (OGLES3_MAX_STAGE_SAMPLERS - 1)] | (d & 0xffffff); break;
        case 6:
            if (!(d >> 24)) { /* unit 0 the constant buffer; the others uniform blocks (rare_uniform) */
                w = ubo + (d & 0xffffff);
                break;
            }
            /* fall through */
        default: w = rare_uniform(ctx, p, si, t, d, tst); break;
        }
        u[i] = le32(w);
    }
    u[var->nrecipe] = 0;
    return (ULONG)u;
}

/* The coordinate or vertex shader's stream (k 0 or 1): the one its last
 * draw in this job wrote, when nothing it reads has changed since. The
 * fragment shader's samples the draw's textures: always written. */
static ULONG vertex_uniforms(struct OGLES3Context *ctx, struct OGLES3Program *p, int k,
                             const struct OGLES3Variant *var, const struct tex_states *tst)
{
    ULONG addr;
    if (p->us_var[k] == var && p->us_gen[k] == g_v3d_cl_generation && p->us_ctx[k] == ctx &&
        p->us_cbuf_ver[k] == p->cbuf_ver[0] && p->us_state_ver[k] == ctx->ustate_ver)
        return p->us_addr[k];
    addr = write_uniforms(ctx, p, var, tst);
    if (addr && var->no_textures && var->uidx) {
        p->us_var[k] = var;
        p->us_gen[k] = g_v3d_cl_generation;
        p->us_ctx[k] = ctx;
        p->us_cbuf_ver[k] = p->cbuf_ver[0];
        p->us_state_ver[k] = ctx->ustate_ver;
        p->us_addr[k] = addr;
    }
    return addr;
}

/* The state write_uniforms reads besides the constant buffers and the
 * textures (the blend colour only in a fragment shader's): a new version
 * when it changes. */
static void uniform_state(struct OGLES3Context *ctx)
{
    const struct OGLES3State *s = &ctx->state;
    ULONG u0 = (ULONG)ctx->viewport[2], u1 = (ULONG)ctx->viewport[3];
    ULONG u2 = float_bits(s->depth_range[0]), u3 = float_bits(s->depth_range[1]);
    ULONG u4 = float_bits(s->line_width), u5 = ctx->fb_flip;
    ULONG u6 = (s->clip_origin == GL_UPPER_LEFT_EXT) | (s->clip_depth == GL_ZERO_TO_ONE_EXT) << 1;
    ULONG *c = ctx->ustate;
    if (c[0] != u0 || c[1] != u1 || c[2] != u2 || c[3] != u3 || c[4] != u4 || c[5] != u5 || c[6] != u6) {
        c[0] = u0; c[1] = u1; c[2] = u2; c[3] = u3; c[4] = u4; c[5] = u5; c[6] = u6;
        ctx->ustate_ver++;
    }
}

/* The 16 default attribute values (0, 0, 0, 1), once per control list. */
static ULONG default_attribs(struct OGLES3Context *ctx, struct OGLES3Draw *dd)
{
    ULONG *p;
    int i;
    if (dd->defattr_gen == g_v3d_cl_generation)
        return dd->defattr;
    if (!(p = claim(ctx->v3d, 16 * 16, 16)))
        return 0;
    for (i = 0; i < 16; i++) {
        p[i * 4 + 0] = p[i * 4 + 1] = p[i * 4 + 2] = 0;
        p[i * 4 + 3] = le32(float_bits(1.0f));
    }
    dd->defattr_gen = g_v3d_cl_generation;
    dd->defattr = (ULONG)p;
    return dd->defattr;
}

struct attr_src
{
    ULONG address, stride;
    UBYTE vsize, vtype, normalized, is_signed;
    UWORD divisor;       /* the record's instance divisor (0: per vertex) */
    UBYTE read_int;      /* "read as int/uint" (an integer array or current value) */
};

/* Emu68 runs a memory access as dearly as several register operations:
 * movem moves four longs with one (tests/perf/emuspeed: 100 words swapped
 * 1.1 us a long at a time, 0.23 us with movem). Byte order swapped in
 * registers in between. */
#define SWAP4 "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t" \
              "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t" \
              "ror.w #8,%%d2\n\tswap %%d2\n\tror.w #8,%%d2\n\t" \
              "ror.w #8,%%d3\n\tswap %%d3\n\tror.w #8,%%d3\n\t"

/* n longs from s to d in the other byte order. */
static void swap_longs(ULONG *d, const ULONG *s, ULONG n)
{
    ULONG q;
    for (q = n >> 2; q; q--)
        __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t" SWAP4
                         "movem.l %%d0-%%d3,(%1)\n\t"
                         "lea (16,%1),%1"
                         : "+a"(s), "+a"(d) : : "d0", "d1", "d2", "d3", "memory");
    for (n &= 3; n; n--)
        *d++ = le32(*s++);
}

/* n 32-bit indices less `base`, in the other byte order. */
static void swap_indices(ULONG *d, const ULONG *s, ULONG n, ULONG base)
{
    ULONG q;
    for (q = n >> 2; q; q--)
        __asm__ volatile("movem.l (%0)+,%%d0-%%d3\n\t"
                         "sub.l %2,%%d0\n\tsub.l %2,%%d1\n\tsub.l %2,%%d2\n\tsub.l %2,%%d3\n\t" SWAP4
                         "movem.l %%d0-%%d3,(%1)\n\t"
                         "lea (16,%1),%1"
                         : "+a"(s), "+a"(d) : "d"(base) : "d0", "d1", "d2", "d3", "memory");
    for (n &= 3; n; n--)
        *d++ = le32(*s++ - base);
}

/* `count` elements of `n` components of `type` into the GPU's byte order. */
/* An IEEE half float (the 68k's order) as a float. */
static GLfloat half_float(UWORD h)
{
    ULONG s = (ULONG)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023;
    union { ULONG u; GLfloat f; } v;
    if (!e) { /* zero, denormals: m / 2^24 */
        v.f = (GLfloat)m * (1.0f / 16777216.0f);
        v.u |= s;
        return v.f;
    }
    v.u = s | (e == 31 ? (0xffUL << 23) : ((e + 112) << 23)) | (m << 13);
    return v.f;
}

/* convert_elems' conversions to 32-bit floats (ES 3.0 2.1.6's): a function
 * of their own, so the copies GLES2's types need keep 1.x's code (with
 * them inline GCC saved the FPU's registers on every call and kept less in
 * registers in the loops; tests/perf/drawbench). */
static __attribute__((noinline)) void convert_to_float(UBYTE *dst, ULONG dst_stride, const UBYTE *src,
                                                       ULONG src_stride, ULONG count, GLint n, GLenum type)
{
    ULONG i;
    int k;
    {
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++) {
                GLfloat f;
                if (type == CONV_HALF3_FLOAT)
                    f = half_float(*(const UWORD *)(src + k * 2));
                else {
                    ULONG w = *(const ULONG *)(src + k * 4);
                    switch (type - CONV_INT_FLOAT) {
                    case 0:  f = (GLfloat)(LONG)w; break;
                    case 1:  f = (GLfloat)((double)(LONG)w / 2147483647.0); f = f < -1.0f ? -1.0f : f; break;
                    case 2:  f = (GLfloat)w; break;
                    default: f = (GLfloat)((double)w / 4294967295.0); break;
                    }
                }
                ((ULONG *)dst)[k] = le32(float_bits(f));
            }
    }
}

static void convert_elems(UBYTE *dst, ULONG dst_stride, const UBYTE *src, ULONG src_stride,
                          ULONG count, GLint n, GLenum type)
{
    ULONG i;
    int k;
    if (type >= CONV_INT_FLOAT) {
        convert_to_float(dst, dst_stride, src, src_stride, count, n, type);
        return;
    }
    switch (type) {
    case GL_INT: /* ES 3.0's 32-bit words: integers read as such, the packed 2_10_10_10 */
    case GL_UNSIGNED_INT:
    case GL_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_FLOAT:
    case GL_FIXED:
        /* Word loads straight from the array (the 68020 reads any address;
         * -fno-strict-aliasing): the copy is most of a draw's CPU time. */
        if (src_stride == (ULONG)n * 4 && dst_stride == src_stride) {
            swap_longs((ULONG *)dst, (const ULONG *)src, count * (ULONG)n);
            break;
        }
        /* Strided (a position in a 4-float slot, interleaved arrays): one
         * movem each way a vertex. */
        if (n == 4) {
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                __asm__ volatile("movem.l (%0),%%d0-%%d3\n\t" SWAP4 "movem.l %%d0-%%d3,(%1)"
                                 : : "a"(src), "a"(dst) : "d0", "d1", "d2", "d3", "memory");
            break;
        }
        if (n == 3) {
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                __asm__ volatile("movem.l (%0),%%d0-%%d2\n\t"
                                 "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t"
                                 "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t"
                                 "ror.w #8,%%d2\n\tswap %%d2\n\tror.w #8,%%d2\n\t"
                                 "movem.l %%d0-%%d2,(%1)"
                                 : : "a"(src), "a"(dst) : "d0", "d1", "d2", "memory");
            break;
        }
        if (n == 2) {
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                __asm__ volatile("movem.l (%0),%%d0-%%d1\n\t"
                                 "ror.w #8,%%d0\n\tswap %%d0\n\tror.w #8,%%d0\n\t"
                                 "ror.w #8,%%d1\n\tswap %%d1\n\tror.w #8,%%d1\n\t"
                                 "movem.l %%d0-%%d1,(%1)"
                                 : : "a"(src), "a"(dst) : "d0", "d1", "memory");
            break;
        }
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride) {
            const ULONG *s = (const ULONG *)src;
            ULONG *d = (ULONG *)dst;
            for (k = 0; k < n; k++)
                d[k] = le32(s[k]);
        }
        break;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            for (k = 0; k < n; k++) {
                dst[k * 2] = src[k * 2 + 1];
                dst[k * 2 + 1] = src[k * 2];
            }
        break;
    default: /* bytes: no swap */
        if (n == 4 && src_stride == 4 && dst_stride == 4) { /* packed colours: movem (memcpy) */
            memcpy(dst, src, count * 4);
            break;
        }
        if (n == 4) { /* colours: one word each, no memcpy call */
            for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
                *(ULONG *)dst = *(const ULONG *)src;
            break;
        }
        for (i = 0; i < count; i++, src += src_stride, dst += dst_stride)
            memcpy(dst, src, (ULONG)n);
        break;
    }
}

/* Copies [first, first+count) of an enabled array in the GPU's byte order,
 * tightly packed at a 4-byte stride: a client array into the state buffer
 * per draw (plan 7.5), a buffer's through its kept GPU copy (buffer.c). */
/* An array's format as a draw uses it, worked out once a format (a
 * Quake III frame draws 70 times with the same few): what convert_elems
 * makes of an element (its type, one of the CPU's conversions to float;
 * its components, a packed one one word; its bytes) and the attribute
 * record's fields. */
static void array_fmt(struct OGLES3Array *a, ULONG key)
{
    int es = type_size(a->type), n = a->size;
    ULONG in_bytes = elem_bytes(a), ctype = a->type;
    if (!a->integer && (a->type == GL_INT || a->type == GL_UNSIGNED_INT))
        ctype = CONV_INT_FLOAT + (a->type == GL_UNSIGNED_INT ? 2 : 0) + (a->normalized ? 1 : 0);
    else if (a->type == GL_HALF_FLOAT && n == 3)
        ctype = CONV_HALF3_FLOAT;
    a->fmt_ctype = ctype;
    a->fmt_in_bytes = (UWORD)in_bytes;
    a->fmt_dst_stride = (UWORD)(((ctype >= CONV_INT_FLOAT ? (ULONG)n * 4 : in_bytes) + 3) & ~3UL);
    a->fmt_nconv = (UBYTE)(packed_type(a->type) ? 1 : n);
    a->fmt_vsize = n == 4 ? v3d_VEC_4 : (UBYTE)n;
    a->fmt_normalized = a->normalized && !a->integer && ctype < CONV_INT_FLOAT && a->type != GL_FLOAT &&
                        a->type != GL_FIXED && a->type != GL_HALF_FLOAT;
    a->fmt_is_signed = ctype < CONV_INT_FLOAT && (a->type == GL_BYTE || a->type == GL_SHORT || a->type == GL_INT ||
                                                  a->type == GL_INT_2_10_10_10_REV);
    /* GL_FIXED is read by the attribute record's fixed type, which is GL's
     * 16.16 (M1 probe F on the A1200, plan 7.5): a byte swap only. */
    a->fmt_vtype = ctype >= CONV_INT_FLOAT ? v3d_ATTRIBUTE_FLOAT
                 : a->type == GL_FIXED ? v3d_ATTRIBUTE_FIXED
                 : a->type == GL_FLOAT ? v3d_ATTRIBUTE_FLOAT
                 : a->type == GL_HALF_FLOAT ? v3d_ATTRIBUTE_HALF_FLOAT
                 : packed_type(a->type) ? v3d_ATTRIBUTE_INT2101010
                 : es == 4 ? v3d_ATTRIBUTE_INT
                 : es == 2 ? v3d_ATTRIBUTE_SHORT : v3d_ATTRIBUTE_BYTE;
    a->fmt_key = key;
}

/* The fmt_ fields of `from` (valid for its fmt_key) into `to`. */
static void fmt_keep(struct OGLES3Array *to, const struct OGLES3Array *from)
{
    to->fmt_key = from->fmt_key;
    to->fmt_ctype = from->fmt_ctype;
    to->fmt_in_bytes = from->fmt_in_bytes;
    to->fmt_dst_stride = from->fmt_dst_stride;
    to->fmt_nconv = from->fmt_nconv;
    to->fmt_vsize = from->fmt_vsize;
    to->fmt_vtype = from->fmt_vtype;
    to->fmt_normalized = from->fmt_normalized;
    to->fmt_is_signed = from->fmt_is_signed;
}

/* The fmt_ fields for the array's format as it is now. */
static inline __attribute__((always_inline)) void fmt_current(struct OGLES3Array *a)
{
    ULONG key = ((ULONG)a->type << 16) | ((ULONG)a->size << 8) | (a->normalized ? 2 : 0) | (a->integer ? 1 : 0);
    if (a->fmt_key != key)
        array_fmt(a, key);
}

/* An array the GPU wrote (transform feedback, a compute shader: buffer.c,
 * ogles2_buffer_gpu_words) read by the GPU where it lies, no wait and no
 * copy back (docs/GLES3_PLAN.md, the remaining gaps, item 5): its elements
 * whole 32-bit words the attribute record reads as they are (floats, fixed,
 * integers read as integers, the packed 10-10-10-2), at word boundaries.
 * Smaller components lie byte-swapped in the words (the shadow's order is
 * the program's), and integers read as floats are converted on the CPU:
 * those keep the CPU path (gpu_written). The fmt_ fields current. *addr:
 * vertex 0's address, *stride, *verts: the buffer's whole vertices. */
static BOOL gpu_logged[3]; /* gpu_array's, gpu_elements' and gpu_indirect's first use, logged */

static __attribute__((noinline)) BOOL gpu_array(struct OGLES3Context *ctx, const struct OGLES3Array *a, ULONG *addr,
                                                 ULONG *stride, ULONG *verts)
{
    ULONG w, size, off = (ULONG)a->pointer, es = a->fmt_in_bytes, st;
    BOOL fed;
    if (a->fmt_ctype >= CONV_INT_FLOAT || (es & 3) || (off & 3) || ((ULONG)a->stride & 3) ||
        (a->type != GL_FLOAT && a->type != GL_FIXED && a->type != GL_INT && a->type != GL_UNSIGNED_INT &&
         !packed_type(a->type)) ||
        !(w = ogles2_buffer_gpu_words(ctx, a->buffer, &fed, &size)))
        return FALSE;
    st = a->stride ? (ULONG)a->stride : es;
    if (!gpu_logged[0]) {
        gpu_logged[0] = TRUE;
        ogles2_log("draw: vertices the GPU wrote read where they lie (logged once)");
    }
    *addr = w + off;
    *stride = st;
    *verts = off + es <= size ? (size - off - es) / st + 1 : 0;
    if (fed)
        ctx->draw_fed = TRUE;
    return TRUE;
}

static inline __attribute__((always_inline)) BOOL copy_array(struct OGLES3Context *ctx, struct OGLES3Array *a,
                                                              GLint first, GLsizei count, struct attr_src *out,
                                                              BOOL probe_addr)
{
    ULONG in_bytes, src_stride, ctype, dst_stride;
    int nconv;
    const UBYTE *src = (const UBYTE *)a->pointer;
    UBYTE *dst;

    fmt_current(a);
    in_bytes = a->fmt_in_bytes;
    src_stride = a->stride ? (ULONG)a->stride : in_bytes;
    ctype = a->fmt_ctype;
    nconv = a->fmt_nconv;
    dst_stride = a->fmt_dst_stride;
    out->divisor = 0;
    out->read_int = a->integer;
    out->vsize = a->fmt_vsize;
    out->normalized = a->fmt_normalized;
    out->is_signed = a->fmt_is_signed;
    out->vtype = a->fmt_vtype;
    /* A buffer the program changes as it draws: the vertices this draw reads,
     * from its shadow (the draw checked the range against the buffer). The
     * copy an earlier draw of this job made, when it holds them and the
     * data has not changed since (Doom 3 draws a surface once a light;
     * tests/perf/drawbench): the copies stay until the job is rendered. */
    if (a->buffer && (src = ogles2_buffer_stream_inl(a->buffer)) != 0) {
        ULONG uid = OGLES3_BH(a->buffer)->uid, serial = OGLES3_BH(a->buffer)->serial;
        struct stream_copy *sc = &ctx->draw->sc[SC_SLOT(uid, a->pointer, a->stride, a->fmt_key)];
        if (sc->job == ctx->job_serial && sc->uid == uid && sc->serial == serial && sc->ptr == a->pointer &&
            sc->stride == a->stride && sc->key == a->fmt_key && !probe_addr && (ULONG)first >= sc->first &&
            (ULONG)first + (ULONG)count <= sc->first + sc->count) {
            out->address = sc->addr + ((ULONG)first - sc->first) * dst_stride;
            out->stride = dst_stride;
            ctx->n_a_hit++;
            return TRUE;
        }
        src += (ULONG)a->pointer;
        if (!probe_addr && (dst = claim(ctx->v3d, dst_stride * (ULONG)count, 4))) {
            out->address = (ULONG)dst;
            out->stride = dst_stride;
            ctx->n_vbytes += dst_stride * (ULONG)count;
            convert_elems(dst, dst_stride, src + (ULONG)first * src_stride, src_stride, (ULONG)count, nconv,
                          (GLenum)ctype);
            sc->job = ctx->job_serial;
            sc->uid = uid;
            sc->serial = serial;
            sc->ptr = a->pointer;
            sc->stride = a->stride;
            sc->key = a->fmt_key;
            sc->first = (ULONG)first;
            sc->count = (ULONG)count;
            sc->addr = (ULONG)dst;
            ctx->n_a_stream++;
            return TRUE;
        }
        if (!probe_addr)
            return FALSE;
    } else if (a->buffer) {
        ULONG vertices, base, uid = OGLES3_BH(a->buffer)->uid, serial = OGLES3_BH(a->buffer)->serial;
        ctx->n_a_static++;
        if (a->bc_job == ctx->job_serial && a->bc_uid == uid && a->bc_serial == serial && a->bc_ptr == a->pointer &&
            a->bc_stride == a->stride && a->bc_key == a->fmt_key) {
            /* this job's lookup of the same copy (buffer.c's loop over the
             * copies and its call, once a job an array) */
            base = a->bc_base;
            vertices = a->bc_vertices;
            out->stride = a->bc_dst_stride;
        } else {
            base = ogles2_buffer_copy(ctx, a->buffer, (ULONG)a->pointer, nconv, (GLenum)ctype,
                                      (GLboolean)(out->normalized | (out->read_int ? 2 : 0)), a->stride, in_bytes,
                                      dst_stride, &out->stride, &vertices, convert_elems);
            if (base) {
                a->bc_job = ctx->job_serial;
                a->bc_uid = uid;
                a->bc_serial = serial;
                a->bc_ptr = a->pointer;
                a->bc_stride = a->stride;
                a->bc_key = a->fmt_key;
                a->bc_base = base;
                a->bc_dst_stride = out->stride;
                a->bc_vertices = vertices;
            }
        }
        /* The draw checked the range against the buffer (arrays_ok). */
        if (!base || (ULONG)first + (ULONG)count > vertices)
            return FALSE;
        out->address = base + (ULONG)first * out->stride;
        return TRUE;
    }
    ctx->n_a_client++;
    if (probe_addr) {
        /* Probe A: the test's buffer, wherever it lies (4 KB). */
        if (dst_stride * (ULONG)count > 4096)
            return FALSE;
        dst = (UBYTE *)ctx->probe_addr;
    } else if (!(dst = claim(ctx->v3d, dst_stride * (ULONG)count, 4)))
        return FALSE;
    out->address = (ULONG)dst;
    out->stride = dst_stride;
    ctx->n_vbytes += dst_stride * (ULONG)count;
    convert_elems(dst, dst_stride, src + (ULONG)first * src_stride, src_stride, (ULONG)count, nconv, (GLenum)ctype);
    if (probe_addr) {
        ULONG len = dst_stride * (ULONG)count;
        CachePreDMA((APTR)out->address, &len, 0);
    }
    return TRUE;
}

/* gl_VertexID's copy from element 0 (draw_arrays): out of line, so the
 * draw's loop holds the only inlined copy_array. */
static __attribute__((noinline)) BOOL copy_array_again(struct OGLES3Context *ctx, struct OGLES3Array *a,
                                                       GLsizei count, struct attr_src *out)
{
    return copy_array(ctx, a, 0, count, out, FALSE);
}

/* copy_array for an array the GPU wrote, read where it lies (gpu_array):
 * FALSE when it cannot be, and copy_array then takes it. Called from the
 * draw's vertex loop only once a buffer was ever given to the GPU to write
 * (ogles2_gpu_writes_seen), so copy_array stays as it was (inline, the
 * draws' loop: tests/perf/drawbench). */
static __attribute__((noinline)) BOOL gpu_source(struct OGLES3Context *ctx, struct OGLES3Array *a, GLint first,
                                                  struct attr_src *out)
{
    ULONG ga, gs, gv;
    fmt_current(a);
    if (!gpu_array(ctx, a, &ga, &gs, &gv))
        return FALSE;
    out->divisor = 0;
    out->read_int = a->integer;
    out->vsize = a->fmt_vsize;
    out->normalized = a->fmt_normalized;
    out->is_signed = a->fmt_is_signed;
    out->vtype = a->fmt_vtype;
    out->address = ga + (ULONG)first * gs;
    out->stride = gs;
    return TRUE;
}

/* A disabled array reads the current generic attribute: 16 bytes, stride 0. */
static BOOL constant_attrib(struct OGLES3Context *ctx, GLuint index, struct attr_src *out)
{
    ULONG *p;
    int k;
    UBYTE kind = ctx->state.attrib_kind[index];
    const ULONG *w = kind ? ctx->state.attrib_word[index] : (const ULONG *)ctx->state.attrib[index];
    typeof(ctx->draw->ca[0]) *ca = &ctx->draw->ca[index];
    ctx->n_a_const++;
    /* The same value again in this control list: its block (a Quake III
     * draw has one or two disabled attributes) */
    if (ca->gen == g_v3d_cl_generation && ca->kind == kind && ca->w[0] == w[0] && ca->w[1] == w[1] &&
        ca->w[2] == w[2] && ca->w[3] == w[3]) {
        p = (ULONG *)ca->addr;
    } else {
        if (!(p = claim(ctx->v3d, 16, 16)))
            return FALSE;
        for (k = 0; k < 4; k++) {
            p[k] = le32(w[k]);
            ca->w[k] = w[k];
        }
        ca->gen = g_v3d_cl_generation;
        ca->kind = kind;
        ca->addr = (ULONG)p;
    }
    out->address = (ULONG)p;
    out->stride = 0;
    out->vsize = v3d_VEC_4;
    out->normalized = 0;
    out->divisor = 0;
    /* ES 3.0's integer current values: their words, read as integers */
    out->read_int = ctx->state.attrib_kind[index] != 0;
    out->vtype = out->read_int ? v3d_ATTRIBUTE_INT : v3d_ATTRIBUTE_FLOAT;
    out->is_signed = ctx->state.attrib_kind[index] == 1;
    return TRUE;
}

/* One GL_SHADER_STATE_ATTRIBUTE_RECORD (16 bytes): v3dcore's
 * glShaderStateAttributeRecord's bytes, written directly (packets_ok checks
 * them once; it is used while they agree). */
static void put_le32(UBYTE *b, ULONG x);
static BOOL packets_ok(V3DContext *v);
static int column_slot(const struct OGLES3ImageAttrib *at, int k);

static void attr_record(UBYTE *b, ULONG address, UBYTE normalized, UBYTE is_signed, UBYTE vsize,
                        UBYTE vtype, UBYTE vs_reads, UBYTE cs_reads, UWORD divisor, ULONG stride, ULONG maxindex)
{
    put_le32(b, address);
    b[4] = (UBYTE)((normalized & 0x80) | (normalized & 1 ? 0x40 : 0) | (is_signed ? 0x20 : 0) | ((vtype & 7) << 2) |
                   (vsize & 3)); /* normalized's bit 7: read as int/uint */
    b[5] = (UBYTE)((vs_reads << 4) | (cs_reads & 15));
    b[6] = (UBYTE)divisor; /* instance divisor, 16 bits */
    b[7] = (UBYTE)(divisor >> 8);
    put_le32(b + 8, stride);
    put_le32(b + 12, maxindex);
}

/* The GL shader state record and its attribute records, one per vertex
 * input slot (Mesa's emit_shader_state_record and v3d_emit_gl_shader_state,
 * V3D 4.2, no geometry shader); the record's address. */
static ULONG shader_record(struct OGLES3Context *ctx, struct OGLES3Program *p, ULONG defattr,
                           const ULONG code[3], const ULONG unif[3], const struct OGLES3Variant *cs,
                           const struct OGLES3Variant *vs, const struct OGLES3Variant *fs,
                           GLenum mode, const struct attr_src *src, int nslots)
{
    V3DContext *v = ctx->v3d;
    int nrec = nslots ? nslots : 1;
    ULONG size = sizeof(v3d_gl_shader_state_record) +
                 nrec * sizeof(v3d_gl_shader_state_attribute_record);
    v3d_gl_shader_state_record *r = claim(v, size, 32); /* GL_SHADER_STATE holds address >> 5 */
    v3d_static_buffer attrs, *saved;
    ULONG *swivel;
    BOOL direct = packets_ok(v);
    BOOL cs_loaded_any = FALSE;
    BOOL cs_builtins = cs->uses_iid || cs->uses_vid;
    int i;

    BOOL points = mode == GL_POINTS && vs->per_vertex_point_size;

    if (!r)
        return 0;
    /* The program's template when it was made for these variants: only
     * the four addresses below differ from draw to draw. */
    if (p->rec_tmpl && p->rec_var[0] == cs && p->rec_var[1] == vs && p->rec_var[2] == fs &&
        p->rec_points == points && p->rec_code[0] == code[0] && p->rec_code[1] == code[1] &&
        p->rec_code[2] == code[2]) {
        memcpy(r, p->rec_tmpl, sizeof(*r));
        goto addresses;
    }
    memset(r, 0, size);
    r->enable_clipping = TRUE;
    r->point_size_in_shaded_vertex_data = points;
    r->fragment_shader_does_z_writes = fs->writes_z;
    r->turn_off_early_z_test = fs->disable_ez;
    r->fragment_shader_uses_real_pixel_centre_w_in_addition_to_centroid_w2 = fs->uses_center_w;
    r->any_shader_reads_hardware_written_primitive_id = fs->uses_pid;
    r->insert_primitive_id_as_first_varying_to_fragment_shader = fs->uses_pid;
    r->do_scoreboard_wait_on_first_thread_switch = fs->lock_scoreboard_on_first_thrsw;
    r->disable_implicit_point_line_varyings = !fs->uses_implicit_point_line_varyings;
    r->number_of_varyings_in_fragment_shader = fs->num_inputs;
    r->coordinate_shader_has_separate_input_and_output_vpm_blocks = cs->separate_segments;
    r->vertex_shader_has_separate_input_and_output_vpm_blocks = vs->separate_segments;
    r->coordinate_shader_input_vpm_segment_size = cs->separate_segments ? cs->vpm_input_size : 1;
    r->vertex_shader_input_vpm_segment_size = vs->separate_segments ? vs->vpm_input_size : 1;
    r->coordinate_shader_output_vpm_segment_size = cs->vpm_output_size;
    r->vertex_shader_output_vpm_segment_size = vs->vpm_output_size;
    /* v3d_compute_vpm_config without a geometry shader: As = 1 (stored
     * minus one), Ve = 0, and Vc from VCM_CACHE_SIZE: the zeroes stay. */
    r->vertex_id_read_by_coordinate_shader = cs->uses_vid;
    r->instance_id_read_by_coordinate_shader = cs->uses_iid;
    r->vertex_id_read_by_vertex_shader = vs->uses_vid;
    r->instance_id_read_by_vertex_shader = vs->uses_iid;

    r->fragment_shader_code_address_rshift_3 = code[2] >> 3;
    r->fragment_shader_4_way_threadable = fs->threads == 4;
    r->fragment_shader_start_in_final_thread_section = fs->single_seg;
    r->fragment_shader_propagate_nans = TRUE;

    r->vertex_shader_code_address_rshift_3 = code[1] >> 3;
    r->vertex_shader_4_way_threadable = vs->threads == 4;
    r->vertex_shader_start_in_final_thread_section = vs->single_seg;
    r->vertex_shader_propagate_nans = TRUE;

    r->coordinate_shader_code_address_rshift_3 = code[0] >> 3;
    r->coordinate_shader_4_way_threadable = cs->threads == 4;
    r->coordinate_shader_start_in_final_thread_section = cs->single_seg;
    r->coordinate_shader_propagate_nans = TRUE;

    /* The bitfield words holding the code addresses, as PiStorm3D swaps them. */
    swivel = (ULONG *)r;
    swivel[3] = le32(swivel[3]);
    swivel[5] = le32(swivel[5]);
    swivel[7] = le32(swivel[7]);
    if (!p->rec_tmpl)
        p->rec_tmpl = AllocVec(sizeof(*r), MEMF_ANY);
    if (p->rec_tmpl) {
        memcpy(p->rec_tmpl, r, sizeof(*r));
        p->rec_var[0] = cs;
        p->rec_var[1] = vs;
        p->rec_var[2] = fs;
        p->rec_points = points;
        p->rec_code[0] = code[0];
        p->rec_code[1] = code[1];
        p->rec_code[2] = code[2];
    }

addresses:
    /* Whole little-endian words, outside the swapped ones above. */
    r->address_of_default_attribute_values = le32(defattr);
    r->fragment_shader_uniforms_address = le32(unif[2]);
    r->vertex_shader_uniforms_address = le32(unif[1]);
    r->coordinate_shader_uniforms_address = le32(unif[0]);

    /* The attribute records follow the record directly, in input slot order. */
    attrs.start = (v3d_u8 *)(r + 1);
    attrs.used = 0;
    attrs.capacity = nrec * sizeof(v3d_gl_shader_state_attribute_record);
    attrs.overflowed = 0;
    saved = v->current_buf;
    v->current_buf = &attrs;
    if (!nslots) {
        /* GFXH-930: at least one attribute read by both shaders: a dummy. */
        glShaderStateAttributeRecord(v, (APTR)defattr, FALSE, FALSE, FALSE, v3d_VEC_1,
                                     v3d_ATTRIBUTE_FLOAT, 1, 1, 0, 0, 0xFFFFFF);
    }
    for (i = 0; i < nslots; i++) {
        UBYTE cs_n = cs->vattr_sizes[i];
        /* GFXH-930 and GFXH-1602 (Mesa): the binning shader loads at least
         * one attribute, the first when it reads the vertex/instance id. */
        if (cs_n)
            cs_loaded_any = TRUE;
        if (i == 0 && cs_builtins && !cs_loaded_any) {
            cs_n = 1;
            cs_loaded_any = TRUE;
        } else if (i == nslots - 1 && !cs_loaded_any) {
            cs_n = 1;
            cs_loaded_any = TRUE;
        }
        if (direct)
            attr_record((UBYTE *)(r + 1) + i * sizeof(v3d_gl_shader_state_attribute_record), src[i].address,
                        (UBYTE)(src[i].normalized | (src[i].read_int ? 0x80 : 0)), src[i].is_signed, src[i].vsize,
                        src[i].vtype, vs->vattr_sizes[i], cs_n, src[i].divisor, src[i].stride, 0xFFFFFF);
        else
            glShaderStateAttributeRecord(v, (APTR)src[i].address, src[i].read_int, src[i].normalized,
                                         src[i].is_signed, src[i].vsize, src[i].vtype,
                                         vs->vattr_sizes[i], cs_n, src[i].divisor, src[i].stride, 0xFFFFFF);
    }
    v->current_buf = saved;
    return (ULONG)r;
}

/* ---- state packets ---- */

static UBYTE blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO:                     return V3D_BLEND_FACTOR_ZERO;
    case GL_ONE:                      return V3D_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR:                return V3D_BLEND_FACTOR_SRCCOLOR;
    case GL_ONE_MINUS_SRC_COLOR:      return V3D_BLEND_FACTOR_INVSRCCOLOR;
    case GL_DST_COLOR:                return V3D_BLEND_FACTOR_DSTCOLOR;
    case GL_ONE_MINUS_DST_COLOR:      return V3D_BLEND_FACTOR_INVDSTCOLOR;
    case GL_SRC_ALPHA:                return V3D_BLEND_FACTOR_SRCALPHA;
    case GL_ONE_MINUS_SRC_ALPHA:      return V3D_BLEND_FACTOR_INVSRCALPHA;
    case GL_DST_ALPHA:                return V3D_BLEND_FACTOR_DSTALPHA;
    case GL_ONE_MINUS_DST_ALPHA:      return V3D_BLEND_FACTOR_INVDSTALPHA;
    case GL_CONSTANT_COLOR:           return V3D_BLEND_FACTOR_CONSTCOLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return V3D_BLEND_FACTOR_INVCONSTCOLOR;
    case GL_CONSTANT_ALPHA:           return V3D_BLEND_FACTOR_CONSTALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return V3D_BLEND_FACTOR_INVCONSTALPHA;
    default:                          return V3D_BLEND_FACTOR_SRCALPHASATURATE;
    }
}

static UBYTE blend_mode(GLenum e)
{
    return e == GL_FUNC_SUBTRACT ? V3D_BLEND_MODE_SUB
         : e == GL_FUNC_REVERSE_SUBTRACT ? V3D_BLEND_MODE_RSUB
         : e == GL_MIN_EXT ? V3D_BLEND_MODE_MIN
         : e == GL_MAX_EXT ? V3D_BLEND_MODE_MAX : V3D_BLEND_MODE_ADD;
}

/* EXT_blend_minmax ignores the factors; Mesa's state tracker
 * (st_atom_blend.c) sets them to ONE for MIN and MAX, as here. */
static UBYTE blend_factor_for(GLenum f, GLenum e)
{
    return e == GL_MIN_EXT || e == GL_MAX_EXT ? V3D_BLEND_FACTOR_ONE : blend_factor(f);
}

/* A blend factor for a target whose destination alpha is 1 (Mesa's
 * v3d_factor with dst_alpha_one). */
static UBYTE alpha_one(UBYTE f)
{
    return f == V3D_BLEND_FACTOR_DSTALPHA                                         ? V3D_BLEND_FACTOR_ONE
         : f == V3D_BLEND_FACTOR_INVDSTALPHA || f == V3D_BLEND_FACTOR_SRCALPHASATURATE ? V3D_BLEND_FACTOR_ZERO
                                                                                   : f;
}

/* IEEE half from a float in [0, 1] (blend constant colour). */
static UWORD half(GLfloat f)
{
    ULONG u = float_bits(f), mant;
    LONG e;
    if (f <= 0.0f)
        return 0;
    e = (LONG)((u >> 23) & 0xff) - 127 + 15;
    mant = u & 0x7fffff;
    if (e <= 0) /* denormal */
        return (UWORD)((mant | 0x800000) >> (14 - e));
    return (UWORD)((e << 10) | (mant >> 13));
}

static UBYTE stencil_op(GLenum op)
{
    switch (op) {
    case GL_ZERO:      return V3D_STENCIL_OP_ZERO;
    case GL_REPLACE:   return V3D_STENCIL_OP_REPLACE;
    case GL_INCR:      return V3D_STENCIL_OP_INCR;
    case GL_DECR:      return V3D_STENCIL_OP_DECR;
    case GL_INVERT:    return V3D_STENCIL_OP_INVERT;
    case GL_INCR_WRAP: return V3D_STENCIL_OP_INCWRAP;
    case GL_DECR_WRAP: return V3D_STENCIL_OP_DECWRAP;
    default:           return V3D_STENCIL_OP_KEEP;
    }
}

/* STENCIL_CFG (opcode 80) for one face, packed byte by byte from Mesa's
 * v3d_packet.xml (plan 4.3): ref bits 0-7, test mask 8-15, function 16-18,
 * stencil fail op 19-21, depth fail op 22-24, pass op 25-27, front 28,
 * back 29, write mask 32-39. */
static void stencil_bytes(const struct OGLES3State *s, int face, UBYTE *b)
{
    GLint ref = s->stencil_ref[face];
    ULONG w = (ULONG)(s->stencil_func[face] - GL_NEVER) |
              ((ULONG)stencil_op(s->stencil_fail[face]) << 3) |
              ((ULONG)stencil_op(s->stencil_zfail[face]) << 6) |
              ((ULONG)stencil_op(s->stencil_zpass[face]) << 9) |
              (face ? 1UL << 13 : 1UL << 12);
    b[0] = (UBYTE)(ref < 0 ? 0 : ref > 255 ? 255 : ref); /* clamped to the 8 bits (GLES 2.0 4.1.4) */
    b[1] = (UBYTE)s->stencil_value_mask[face];
    b[2] = (UBYTE)w;
    b[3] = (UBYTE)(w >> 8);
    b[4] = (UBYTE)s->stencil_write_mask[face];
}

/* One kind of varying flags of a fragment shader (GLSL ES 3.00's flat and
 * centroid; Mesa's emit_varying_flags): a packet per word of 24 varyings
 * that has any, the first zeroing the varyings around it; FALSE if none. */
static BOOL varying_flags(V3DContext *v, const ULONG *flags, UBYTE op)
{
    BOOL any = FALSE;
    int i;
    for (i = 0; i < 3; i++) {
        ULONG lower, higher, w;
        UBYTE *b;
        if (!flags[i])
            continue;
        if (any)
            lower = higher = V3D_VARYING_FLAGS_ACTION_UNCHANGED;
        else if (i == 0)
            lower = V3D_VARYING_FLAGS_ACTION_UNCHANGED, higher = V3D_VARYING_FLAGS_ACTION_ZEROED;
        else
            lower = higher = V3D_VARYING_FLAGS_ACTION_ZEROED;
        w = (ULONG)i | lower << 4 | higher << 6 | (flags[i] & 0xffffff) << 8;
        if ((b = v3d_buffer_claim_memory(v->current_buf, 5))) {
            b[0] = op;
            b[1] = (UBYTE)w;
            b[2] = (UBYTE)(w >> 8);
            b[3] = (UBYTE)(w >> 16);
            b[4] = (UBYTE)(w >> 24);
        }
        any = TRUE;
    }
    return any;
}

static void emit_stencil(V3DContext *v, const UBYTE *b)
{
    UBYTE *p = v3d_buffer_claim_memory(v->current_buf, 6);
    if (p) {
        p[0] = v3d_OP_STENCIL_CFG;
        memcpy(p + 1, b, 5);
    }
}

/* Mesa's per-frame early-Z direction (v3d_update_job_ez), from this draw's
 * depth and stencil state; the frame keeps one direction or gives early Z up. */
static void update_ez(V3DContext *v, BOOL depth_on, GLenum func, BOOL stencil_on,
                      const struct OGLES3State *s)
{
    UBYTE draw_ez = V3D_EZ_UNDECIDED;
    if (depth_on) {
        switch (func) {
        case GL_LESS: case GL_LEQUAL:    draw_ez = V3D_EZ_LT_LE; break;
        case GL_GREATER: case GL_GEQUAL: draw_ez = V3D_EZ_GT_GE; break;
        case GL_NEVER: case GL_EQUAL:    draw_ez = V3D_EZ_UNDECIDED; break;
        default:                         draw_ez = V3D_EZ_DISABLED; break;
        }
        /* Mesa (v3dx_state.c): a stencil test that can reject, or that
         * changes values when depth fails, breaks early-Z updates. */
        if (stencil_on && (s->stencil_zfail[0] != GL_KEEP || s->stencil_func[0] != GL_ALWAYS ||
                           s->stencil_zfail[1] != GL_KEEP || s->stencil_func[1] != GL_ALWAYS))
            draw_ez = V3D_EZ_DISABLED;
    }
    if (draw_ez == V3D_EZ_DISABLED)
        v->ez_state = V3D_EZ_DISABLED;
    else if (draw_ez != V3D_EZ_UNDECIDED) {
        if (v->ez_state == V3D_EZ_UNDECIDED)
            v->ez_state = draw_ez;
        else if (v->ez_state != draw_ez)
            v->ez_state = V3D_EZ_DISABLED;
    }
    /* The render config takes the first decided direction; a job whose first
     * draw disables early Z keeps it off (Mesa: draw_calls_queued == 0). */
    if (v->first_ez_state == V3D_EZ_UNDECIDED &&
        (v->ez_state != V3D_EZ_DISABLED || !v->draw_state_configured))
        v->first_ez_state = v->ez_state;
}

static __attribute__((noinline)) void emit_state_all(struct OGLES3Context *ctx, struct dedup *dd, int prim,
                                                     const struct OGLES3Variant *fs);

/* The draw's state packets, unless nothing they read changed since the
 * last draw of this control list: that test inline, the rest in
 * emit_state_all (whose prologue and epilogue, ten registers each way, a
 * draw like the last paid for nothing; tests/perf/drawbench). */
static inline void emit_state(struct OGLES3Context *ctx, struct dedup *dd, int prim, const struct OGLES3Variant *fs)
{
    if (dd->gen == g_v3d_cl_generation && dd->ver == ctx->state_ver && dd->prim == prim && dd->fs == fs &&
        dd->fb == ctx->draw_fb && dd->fb_w == ctx->fb_w && dd->fb_h == ctx->fb_h &&
        dd->fb_depth_bits == ctx->fb_depth_bits && dd->fb_stencil_bits == ctx->fb_stencil_bits &&
        ctx->v3d->draw_state_configured)
        return;
    emit_state_all(ctx, dd, prim, fs);
}

static void emit_state_all(struct OGLES3Context *ctx, struct dedup *dd, int prim, const struct OGLES3Variant *fs)
{
    V3DContext *v = ctx->v3d;
    const struct OGLES3State *s = &ctx->state;
    BOOL depth_on = (s->caps & OGLES3_CAP_DEPTH_TEST) && ctx->fb_depth_bits;
    BOOL stencil_on = (s->caps & OGLES3_CAP_STENCIL_TEST) && ctx->fb_stencil_bits;
    BOOL flip = ctx->fb_flip;
    UBYTE effp, erfp, cp, edo, dtf, zue, eze, ezue, be, se, wf, pf, pv, rom, smask;
    LONG fh = (LONG)ctx->fb_h;
    LONG vx = ctx->viewport[0], vy = ctx->viewport[1], vw = ctx->viewport[2], vh = ctx->viewport[3];
    LONG l, t, r, b;
    UWORD vp_cx2, vp_cy2;
    ULONG cwm, g;
    float zs, zo;
    int k;

    ctx->n_emits++;
    /* g: the parts to compute again (OGLES3_SG_*): those the setters marked
     * since the last time, all of them for a new control list, pass,
     * framebuffer or primitive; a new fragment shader, the configuration
     * bits too (its own Z) and its varying flags (below). */
    if (dd->gen != g_v3d_cl_generation) { /* a new control list: emit everything */
        memset(dd, 0, sizeof(*dd));
        dd->gen = g_v3d_cl_generation;
        dd->prim = -1;
        g = OGLES3_SG_ALL;
    } else if (dd->ver == ctx->state_ver && dd->prim == prim && dd->fs == fs && dd->fb == ctx->draw_fb &&
               dd->fb_w == ctx->fb_w && dd->fb_h == ctx->fb_h && dd->fb_depth_bits == ctx->fb_depth_bits &&
               dd->fb_stencil_bits == ctx->fb_stencil_bits && v->draw_state_configured) {
        return; /* nothing it reads has changed since the last draw of this list */
    } else if (dd->prim != prim || dd->fb != ctx->draw_fb || dd->fb_w != ctx->fb_w || dd->fb_h != ctx->fb_h ||
               dd->fb_depth_bits != ctx->fb_depth_bits || dd->fb_stencil_bits != ctx->fb_stencil_bits ||
               !v->draw_state_configured) {
        g = OGLES3_SG_ALL;
    } else {
        g = ctx->state_dirty | (dd->fs != fs ? OGLES3_SG_CFG : 0);
    }
    ctx->state_dirty = 0;
    dd->ver = ctx->state_ver;
    dd->fs = fs;
    dd->fb = ctx->draw_fb;
    dd->fb_w = ctx->fb_w;
    dd->fb_h = ctx->fb_h;
    dd->fb_depth_bits = (UBYTE)ctx->fb_depth_bits;
    dd->fb_stencil_bits = (UBYTE)ctx->fb_stencil_bits;

    /* CLIP_WINDOW: viewport, and scissor box if enabled, in the hardware's
     * top-down frame (the default framebuffer's GL y is from the bottom; a
     * framebuffer object's row 0 is GL's), clamped to the framebuffer. */
    if (g & OGLES3_SG_WIN) {
    l = vx;
    r = vx + vw;
    t = flip ? fh - (vy + vh) : vy;
    b = flip ? fh - vy : vy + vh;
    if (s->caps & OGLES3_CAP_SCISSOR_TEST) {
        LONG sl = ctx->scissor[0], sr = sl + ctx->scissor[2];
        LONG st = flip ? fh - (ctx->scissor[1] + ctx->scissor[3]) : ctx->scissor[1];
        LONG sb = flip ? fh - ctx->scissor[1] : ctx->scissor[1] + ctx->scissor[3];
        if (sl > l) l = sl;
        if (sr < r) r = sr;
        if (st > t) t = st;
        if (sb < b) b = sb;
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)ctx->fb_w) r = (LONG)ctx->fb_w;
    if (b > fh) b = fh;
    if (r < l) r = l;
    if (b < t) b = t;
    if (!dd->cw || dd->cw_l != l || dd->cw_t != t || dd->cw_w != r - l || dd->cw_h != b - t) {
        ClipWindow(v, (UWORD)l, (UWORD)t, (UWORD)(r - l), (UWORD)(b - t));
        dd->cw = TRUE;
        dd->cw_l = (UWORD)l; dd->cw_t = (UWORD)t; dd->cw_w = (UWORD)(r - l); dd->cw_h = (UWORD)(b - t);
    }
    }

    /* CFG_BITS. Culling as Mesa's v3dx_emit.c; the winding flag set for
     * counter-clockwise as PiStorm3D's on the flipped default framebuffer
     * (its shaders flip y the same way), for clockwise on an object, and
     * the other way round with an upper-left origin (y_down). */
    edo = (s->caps & OGLES3_CAP_POLYGON_OFFSET_FILL) ? TRUE : FALSE;
    /* be: Mesa's v3dx_emit.c, the tile buffer does not blend while the
     * shader does (a 32-bit float or integer target in the framebuffer) */
    be = (s->caps & OGLES3_CAP_BLEND) && !ctx->fb_soft_blend ? TRUE : FALSE;
    /* 4x MSAA (the job's, job.c): the rasterizer samples four
     * points a pixel (Mesa v3dx_emit.c), and glSampleCoverage's mask, as
     * Mesa's state tracker makes one: round(value x 4) samples, inverted on
     * request. */
    rom = ctx->job_msaa ? 1 : 0;
    smask = 0xF;
    if (ctx->job_msaa) {
        if (s->caps & OGLES3_CAP_SAMPLE_COVERAGE) {
            int n = (int)(s->sample_coverage_value * 4.0f + 0.5f);
            smask = (UBYTE)((1U << n) - 1);
            if (s->sample_coverage_invert)
                smask = (UBYTE)(~smask & 0xF);
        }
        if (s->caps & OGLES3_CAP_SAMPLE_MASK) /* ES 3.1's glSampleMaski */
            smask &= (UBYTE)(s->sample_mask & 0xF);
    }
    if (g & OGLES3_SG_CFG) {
    effp = erfp = TRUE;
    if (s->caps & OGLES3_CAP_CULL_FACE) {
        if (s->cull_face_mode != GL_BACK) effp = FALSE;  /* FRONT or FRONT_AND_BACK */
        if (s->cull_face_mode != GL_FRONT) erfp = FALSE; /* BACK or FRONT_AND_BACK */
    }
    if (s->caps & OGLES3_CAP_RASTERIZER_DISCARD) /* Mesa: both facings off (transform feedback still) */
        effp = erfp = FALSE;
    cp = y_down(ctx) ? s->front_face == GL_CCW : s->front_face == GL_CW;
    dtf = depth_on ? (UBYTE)(s->depth_func - GL_NEVER) : V3D_COMPARE_FUNC_ALWAYS;
    zue = depth_on && s->depth_mask;
    update_ez(v, depth_on, s->depth_func, stencil_on, s);
    /* Mesa: a fragment shader that computes its own Z can go against the
     * frame's early-Z direction. */
    if (fs->writes_z && !fs->writes_z_from_fep)
        v->ez_state = V3D_EZ_DISABLED;
    eze = depth_on && v->ez_state != V3D_EZ_DISABLED;
    ezue = eze;
    if (!depth_on && !zue) /* Mesa: updates follow the frame's EZ state outside the depth gate */
        ezue = v->ez_state != V3D_EZ_DISABLED;
    se = stencil_on ? TRUE : FALSE;
    /* glPolygonMode and glProvokingVertex (AmigaOS 4), as Mesa's v3dx_emit.c:
     * the hardware applies the fill modes to triangles only. */
    wf = s->polygon_mode != GL_FILL;
    pf = s->polygon_mode == GL_POINT;
    pv = s->provoking_vertex == GL_FIRST_VERTEX_CONVENTION;
    if (!dd->cfg || dd->effp != effp || dd->erfp != erfp || dd->cp != cp || dd->edo != edo ||
        dd->dtf != dtf || dd->zue != zue || dd->eze != eze || dd->ezue != ezue || dd->be != be ||
        dd->se != se || dd->wf != wf || dd->pf != pf || dd->pv != pv || dd->rom != rom) {
        CfgBits(v, effp, erfp, cp, edo, V3D_LINE_RASTERIZATION_DIAMOND_EXIT, rom, wf, dtf, zue, eze,
                ezue, se, be, pf, pv);
        dd->cfg = TRUE;
        dd->effp = effp; dd->erfp = erfp; dd->cp = cp; dd->edo = edo; dd->dtf = dtf;
        dd->zue = zue; dd->eze = eze; dd->ezue = ezue; dd->be = be; dd->se = se;
        dd->wf = wf; dd->pf = pf; dd->pv = pv; dd->rom = rom;
    }
    }
    /* STENCIL_CFG, front and back, while the test is on (Mesa v3dx_emit.c). */
    if (stencil_on && (g & OGLES3_SG_STENCIL)) {
        UBYTE b[5];
        for (k = 0; k < 2; k++) {
            stencil_bytes(s, k, b);
            if (!dd->st || memcmp(b, dd->stencil[k], 5)) {
                emit_stencil(v, b);
                memcpy(dd->stencil[k], b, 5);
            }
        }
        dd->st = TRUE;
    }
    if (edo && (g & OGLES3_SG_OFFSET)) {
        /* The GPU counts units in a 24-bit depth buffer's steps: x256 for a
         * 16-bit one (Mesa's depth_offset_z16; exact, a power of two). */
        GLfloat units = s->polygon_offset_units * (ctx->fb_depth_bits == 16 ? 256.0f : 1.0f);
        if (!dd->do_valid || dd->do_factor != s->polygon_offset_factor || dd->do_units != units) {
            DepthOffset(v, s->polygon_offset_factor, units);
            dd->do_valid = TRUE;
            dd->do_factor = s->polygon_offset_factor;
            dd->do_units = units;
        }
    }
    /* POINT_SIZE on every draw (Mesa's V3D 4.2 binner erratum workaround):
     * with the draw's other packets (draw_packets). */
    if ((g & OGLES3_SG_LINE) && (!dd->lw || dd->lw_value != s->line_width)) {
        LineWidth(v, s->line_width);
        dd->lw = TRUE;
        dd->lw_value = s->line_width;
    }

    /* The viewport group (CLIPPER_XY_SCALING, the Z packets, VIEWPORT_OFFSET),
     * as Mesa's v3dx_emit.c: the Z scale and offset from the depth range and
     * mode, the clipper's Y scale of the vertex shader's sign. The Z planes
     * are the depth range in window depth, in either mode (Mesa's
     * util_viewport_zmin_zmax). */
    if (g & OGLES3_SG_VIEW) {
    viewport_z(ctx, &zs, &zo);
    vp_cx2 = (UWORD)(2 * vx + vw);              /* twice the centre, from the left */
    vp_cy2 = (UWORD)(flip ? 2 * (fh - vy) - vh  /* twice the centre, from the top */
                          : 2 * vy + vh);
    if (!dd->vp || dd->vp_w != vw || dd->vp_h != vh || dd->vp_cx2 != vp_cx2 || dd->vp_cy2 != vp_cy2 ||
        dd->vp_zs != zs || dd->vp_zo != zo || dd->vp_down != y_down(ctx)) {
        ClipperXYScaling(v, (UWORD)vw, (UWORD)vh, y_down(ctx));
        ClipperZScaleAndOffset(v, zs, zo);
        ClipperZMinMaxClippingPlanes(v, zo - zs < zo + zs ? zo - zs : zo + zs,
                                     zo - zs < zo + zs ? zo + zs : zo - zs);
        ViewportOffset(v, vp_cx2, 0, vp_cy2, 0);
        dd->vp = TRUE;
        dd->vp_w = (UWORD)vw; dd->vp_h = (UWORD)vh; dd->vp_cx2 = vp_cx2; dd->vp_cy2 = vp_cy2;
        dd->vp_zs = zs; dd->vp_zo = zo; dd->vp_down = y_down(ctx);
    }
    }

    /* Blending: BLEND_ENABLES, BLEND_CFG (all four render targets, as Mesa
     * and PiStorm3D), BLEND_CONSTANT_COLOR. Targets whose format has no
     * alpha (R8, RG8, RGB8, RGB565, R16F, ...) read a destination alpha of
     * 1: a BLEND_CFG of their own where the others have alpha (Mesa's
     * blend_dst_alpha_one). */
    if (!(g & OGLES3_SG_BLEND)) {
        /* unchanged */
    } else if (be) {
        UBYTE src = blend_factor_for(s->blend_src_rgb, s->blend_eq_rgb);
        UBYTE dst = blend_factor_for(s->blend_dst_rgb, s->blend_eq_rgb);
        UBYTE asrc = blend_factor_for(s->blend_src_alpha, s->blend_eq_alpha);
        UBYTE adst = blend_factor_for(s->blend_dst_alpha, s->blend_eq_alpha);
        UBYTE eq = blend_mode(s->blend_eq_rgb), aeq = blend_mode(s->blend_eq_alpha);
        UBYTE one = 0;
        UWORD c[4];
        for (k = 0; k < 4; k++)
            c[k] = half(s->blend_color[k]);
        for (k = 0; k < ctx->fb_nrt; k++)
            if ((ctx->fb_draw_mask & (1 << k)) && ctx->fb_rt[k].no_alpha)
                one |= (UBYTE)(1 << k);
        if (!dd->blend || !dd->be || dd->b_src != src || dd->b_dst != dst || dd->b_eq != eq ||
            dd->b_asrc != asrc || dd->b_adst != adst || dd->b_aeq != aeq || dd->b_one != one ||
            dd->b_color[0] != c[0] || dd->b_color[1] != c[1] || dd->b_color[2] != c[2] ||
            dd->b_color[3] != c[3]) {
            BlendEnables(v, 0x0F);
            if (one != 0x0F)
                BlendCfg(v, (UBYTE)(0x0F & ~one), dst, src, eq, adst, asrc, aeq);
            if (one)
                BlendCfg(v, one, alpha_one(dst), alpha_one(src), eq, alpha_one(adst), alpha_one(asrc), aeq);
            BlendConstantColor(v, c[3], c[2], c[1], c[0]);
            dd->b_src = src; dd->b_dst = dst; dd->b_eq = eq;
            dd->b_asrc = asrc; dd->b_adst = adst; dd->b_aeq = aeq; dd->b_one = one;
            for (k = 0; k < 4; k++)
                dd->b_color[k] = c[k];
        }
    } else if (!dd->blend || dd->be) {
        BlendEnables(v, 0x00);
    }
    if (g & OGLES3_SG_BLEND) {
        dd->blend = TRUE;
        dd->be = be;
    }

    /* COLOR_WRITE_MASKS: a set bit disables a channel (R, G, B, A = bits
     * 0-3), four bits a render target. GLES2's one mask for every draw
     * buffer; a target no draw buffer names (EXT_draw_buffers' GL_NONE, a
     * missing attachment) is not written at all. */
    if (g & OGLES3_SG_MASK) {
        ULONG m = (s->color_mask[0] ? 0 : 1) | (s->color_mask[1] ? 0 : 2) |
                  (s->color_mask[2] ? 0 : 4) | (s->color_mask[3] ? 0 : 8);
        cwm = 0;
        for (k = 0; k < OGLES3_MAX_DRAW_BUFFERS; k++)
            cwm |= ((ctx->fb_draw_mask & ctx->draw_rts & (1 << k)) ? m : 0xF) << (4 * k);
        if (!dd->cwm || dd->cwm_mask != cwm) {
            ColorWriteMasks(v, cwm);
            dd->cwm = TRUE;
            dd->cwm_mask = cwm;
        }
    }

    /* The per-pass residue on a pass's first draw (PiStorm3D's
     * gl_EnsureDrawState; Mesa's first draw, v3dx_emit.c). */
    if (!v->draw_state_configured) {
        DoCommand(v, v3d_OP_ZERO_ALL_FLAT_SHADE_FLAGS);
        DoCommand(v, v3d_OP_ZERO_ALL_NON_PERSPECTIVE_FLAGS);
        DoCommand(v, v3d_OP_ZERO_ALL_CENTROID_FLAGS);
        TransformFeedbackSpecs(v, FALSE, 0);
        OcclusionQueryCounter(v, ctx->oq_addr); /* the active occlusion query (query.c), or off */
        SampleState(v, 1, smask); /* coverage 1.0 (the emitter converts it to a float) */
        dd->smask = smask;
        v->draw_state_configured = TRUE;
        dd->prim = prim;
        dd->flags_fs = 0;
        dd->flags_set = 0;
    } else {
        if (dd->prim != prim) { /* Mesa: on every primitive-type change */
            TransformFeedbackSpecs(v, FALSE, 0);
            dd->prim = prim;
        }
        if ((g & (OGLES3_SG_CFG | OGLES3_SG_SAMPLE)) && dd->smask != smask) { /* within the pass */
            SampleState(v, 1, smask);
            dd->smask = smask;
        }
    }
    /* The fragment shader's flat, no-perspective and centroid varyings
     * (Mesa sends them on every fragment shader change); a kind it has none
     * of is zeroed only if an earlier shader of the pass set some, so GLES2
     * programs (none) send nothing here. */
    if (fs && fs != dd->flags_fs) {
        static const UBYTE op[3] = {v3d_OP_FLAT_SHADE_FLAGS, v3d_OP_NON_PERSPECTIVE_FLAGS, v3d_OP_CENTROID_FLAGS};
        static const UBYTE zero[3] = {v3d_OP_ZERO_ALL_FLAT_SHADE_FLAGS, v3d_OP_ZERO_ALL_NON_PERSPECTIVE_FLAGS,
                                      v3d_OP_ZERO_ALL_CENTROID_FLAGS};
        const ULONG *f[3];
        int k;
        f[0] = fs->flat_shade_flags;
        f[1] = fs->noperspective_flags;
        f[2] = fs->centroid_flags;
        for (k = 0; k < 3; k++) {
            if (varying_flags(v, f[k], op[k])) {
                dd->flags_set |= 1 << k;
            } else if (dd->flags_set & (1 << k)) {
                DoCommand(v, zero[k]);
                dd->flags_set &= ~(1 << k);
            }
        }
        dd->flags_fs = fs;
    }
}

/* ---- the packets every draw ends with ---- */

static void put_le32(UBYTE *b, ULONG x)
{
    b[0] = (UBYTE)x;
    b[1] = (UBYTE)(x >> 8);
    b[2] = (UBYTE)(x >> 16);
    b[3] = (UBYTE)(x >> 24);
}

/* POINT_SIZE 1.0, VCM_CACHE_SIZE, GL_SHADER_STATE, then INDEX_BUFFER_SETUP
 * and INDEXED_PRIM_LIST (itype: v3d_INDEX_TYPE_*), or VERTEX_ARRAY_PRIMS
 * when itype < 0: v3dcore's writers' bytes (v3d_commands.c), written
 * directly. Theirs clear each packet a byte at a time and pack bitfields,
 * about a microsecond a draw on Emu68. 31 or 22 bytes; checked against
 * theirs once (packets_ok). */
static inline __attribute__((always_inline)) ULONG draw_packets(UBYTE *b, UBYTE vcm_cs, UBYTE vcm_vs, ULONG rec, int nrec,
                                                                 UBYTE mode,
                          int itype, ULONG idx_addr, ULONG idx_size, ULONG count, ULONG first, ULONG instances,
                          ULONG ioffset)
{
    b[0] = v3d_OP_POINT_SIZE;
    put_le32(b + 1, 0x3F800000UL); /* 1.0f */
    b[5] = v3d_OP_VCM_CACHE_SIZE;
    b[6] = (UBYTE)((vcm_vs << 4) | (vcm_cs & 15)); /* rendering, binning */
    b[7] = v3d_OP_GL_SHADER_STATE;
    put_le32(b + 8, (rec & ~31UL) | ((ULONG)nrec & 31));
    if (itype >= 0) {
        b[12] = v3d_OP_INDEX_BUFFER_SETUP;
        put_le32(b + 13, idx_addr);
        put_le32(b + 17, idx_size);
        b[21] = instances > 1 ? v3d_OP_INDEXED_INSTANCED_PRIM_LIST : v3d_OP_INDEXED_PRIM_LIST;
        b[22] = (UBYTE)((itype << 6) | (mode & 63));
        put_le32(b + 23, count); /* bits 0-30 the length, bit 31 primitive restart (the caller's) */
        if (instances > 1) { /* ES 3.0's instances (Mesa's packet 34): then the index offset */
            put_le32(b + 27, instances);
            put_le32(b + 31, ioffset);
            return 35;
        }
        put_le32(b + 27, ioffset); /* the base vertex of indices read on the GPU (gpu_elements), else 0 */
        return 31;
    }
    if (instances > 1) { /* packet 38: the mode a byte, length, instances, first vertex */
        b[12] = v3d_OP_VERTEX_ARRAY_INSTANCED_PRIMS;
        b[13] = mode;
        put_le32(b + 14, count);
        put_le32(b + 18, instances);
        put_le32(b + 22, first);
        return 26;
    }
    b[12] = v3d_OP_VERTEX_ARRAY_PRIMS;
    b[13] = mode;
    put_le32(b + 14, count);
    put_le32(b + 18, first); /* the index of the first vertex */
    return 22;
}

/* draw_packets' bytes are v3dcore's: made both ways once, into a buffer of
 * the test's own, for both endings (0 not checked yet, 1 the same, 2 not:
 * logged, and v3dcore's writers used). */
static UBYTE packets_state;

static BOOL packets_ok(V3DContext *v)
{
    UBYTE ref[128], mine[128];
    v3d_static_buffer sb, *saved;
    ULONG n;
    if (packets_state)
        return packets_state == 1;
    memset(&sb, 0, sizeof(sb));
    memset(ref, 0, sizeof(ref));
    sb.start = ref;
    sb.capacity = sizeof(ref);
    saved = v->current_buf;
    v->current_buf = &sb;
    PointSize(v, 1.0f);
    VCMCacheSize(v, 3, 5);
    glShaderState(v, 0x12345660UL, 3);
    IndexBufferSetup(v, 0x0ABCDEF4UL, 0x1236);
    IndexedPrimList(v, 4, v3d_INDEX_TYPE_16_BIT, 0x10203, FALSE, 0);
    PointSize(v, 1.0f);
    VCMCacheSize(v, 2, 1);
    glShaderState(v, 0x0FEDCBA0UL, 1);
    VertexArrayPrims(v, 5, 0x30201, 0x4321);
    glShaderStateAttributeRecord(v, (APTR)0x08123456UL, FALSE, TRUE, TRUE, v3d_VEC_3, v3d_ATTRIBUTE_SHORT,
                                 3, 2, 0, 0x0C, 0xFFFFFF);
    glShaderStateAttributeRecord(v, (APTR)0x08654320UL, TRUE, FALSE, FALSE, v3d_VEC_4, v3d_ATTRIBUTE_FLOAT,
                                 4, 1, 0x0203, 0x10, 0xFFFFFF); /* a divisor's bytes and read as int too */
    v->current_buf = saved;
    n = draw_packets(mine, 3, 5, 0x12345660UL, 3, 4, v3d_INDEX_TYPE_16_BIT, 0x0ABCDEF4UL, 0x1236, 0x10203, 0, 1, 0);
    n += draw_packets(mine + n, 2, 1, 0x0FEDCBA0UL, 1, 5, -1, 0, 0, 0x30201, 0x4321, 1, 0);
    attr_record(mine + n, 0x08123456UL, TRUE, TRUE, v3d_VEC_3, v3d_ATTRIBUTE_SHORT, 3, 2, 0, 0x0C, 0xFFFFFF);
    n += 16;
    attr_record(mine + n, 0x08654320UL, 0x80, FALSE, v3d_VEC_4, v3d_ATTRIBUTE_FLOAT, 4, 1, 0x0203, 0x10, 0xFFFFFF);
    n += 16;
    packets_state = (ULONG)sb.used == n && !sb.overflowed && !memcmp(ref, mine, n) ? 1 : 2;
    if (packets_state == 2)
        ogles2_log("draw: the direct packet writer differs from v3dcore's (%ld bytes, %lu): v3dcore's "
                   "used", (LONG)sb.used, n);
    return packets_state == 1;
}

/* ---- glDrawArrays ---- */

/* Probe P: the time since the last mark goes to stage k (frame.c reports
 * them): 0 checks and the index range, 1 shaders, 2 textures, 3 uniforms,
 * 4 vertex data, 5 the record, 6 indices, 7 the binning list. */
static void mark(struct OGLES3Context *ctx, int k)
{
    if (ctx->probe & OGLES3_PROBE_PERIODIC) {
        ULONG t = ogles2_clock_us();
        if (k >= 0)
            ctx->stage[k] += (double)(ULONG)(t - ctx->t_stage);
        ctx->t_stage = t;
    }
}

/* An indexed draw's indices, already rebased to the copied vertex range. */
static ULONG index_size(GLenum type)
{
    return type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
}

struct elements
{
    const void *indices;  /* the application's */
    GLenum type;          /* GL_UNSIGNED_BYTE, _SHORT or _INT (OES_element_index_uint) */
    GLsizei count;
    ULONG min;            /* the smallest index: subtracted from each */
    BOOL restart;         /* GL_PRIMITIVE_RESTART_FIXED_INDEX: the type's largest index restarts */
    ULONG uid, serial;    /* from an element buffer: its data (ogles2_buffer_serial); uid 0: the application's */
};

/* ---- draw-time variants (docs/GLES3_PLAN.md stage A item 5) ----
 * An OpenGL ES 3 program (a format 2 image) draws with the variants whose
 * keys are what this draw's state gives, as Mesa's v3d_update_compiled_fs
 * and _vs compute them; those its file lacks come from its .v3dx images, or
 * are compiled into the cache's. The key a draw needs is the closest
 * variant's (1.x's choice: the fields a draw does not change keep the
 * compiler's values, such as point sprites') with the fields computed here.
 * In stage A every render target is 8-bit or 16F, so no draw blends in the
 * shader: software_blend, the targets' formats and blend state,
 * f32_color_rb, norm_16 and snorm are 0, and the logic op is COPY (12; 0
 * would be CLEAR). The 32-bit samplers (Mesa's sampler_is_32b, from the
 * bound textures) and alpha to coverage (msaa and sample_alpha_to_coverage
 * set, ignore_sample_mask clear: a 4x MSAA default framebuffer with
 * GL_SAMPLE_ALPHA_TO_COVERAGE) are in the key too, so an ES3 program needs
 * no .v3dw or .v3da compiled; shipped or cached ones are still looked in.
 * GLES2 programs keep 1.x's choice (the same variants as in 1.3). Key bytes
 * are FORMAT.md's; the keys' fields are computed and matched in
 * variant_key.c. */
/* Stage st's (0 vertex, 1 fragment) 32-bit samplers: Mesa's sampler_is_32b,
 * a bit per texture index whose bound texture returns 32 bits a channel
 * (v3d_setup_shared_key); 0 while no such texture was ever given an image. */
static ULONG sampler_bits(struct OGLES3Context *ctx, struct OGLES3Program *p, int st)
{
    const struct OGLES3Image *im = p->image;
    UWORD k, n;
    ULONG bits = 0;
    if (!ogles2_wide_seen)
        return 0;
    n = list_samplers(p);
    for (k = 0; k < n; k++) {
        UWORD u = p->nsamplers == 0xfe ? k : p->samplers[k];
        const struct OGLES3ImageUniform *un = &im->uniforms[u];
        UWORD e, ne = un->array ? un->array : 1;
        if (!(un->flags & OGLES3_UNIFORM_SAMPLER))
            continue;
        for (e = 0; e < ne; e++) {
            ULONG idx = un->sampler[st] + e;
            if (un->sampler[st] >= OGLES3_MAX_STAGE_SAMPLERS || idx >= OGLES3_MAX_STAGE_SAMPLERS)
                break;
            if (ogles2_texture_wide(ctx, unit_of(p, u, e), ogles2_sampler_kind(un->type)))
                bits |= 1UL << idx;
        }
    }
    return bits;
}

/* GL's blend equation and factors as Mesa's pipe enums (its state
 * tracker's translation; MIN and MAX take factors ONE). */
static ULONG pipe_func(GLenum e)
{
    return e == GL_FUNC_SUBTRACT ? 1 : e == GL_FUNC_REVERSE_SUBTRACT ? 2 : e == GL_MIN_EXT ? 3
         : e == GL_MAX_EXT ? 4 : 0;
}

static ULONG pipe_factor(GLenum f, GLenum e)
{
    if (e == GL_MIN_EXT || e == GL_MAX_EXT)
        return 1;
    switch (f) {
    case GL_ZERO:                     return 17;
    case GL_ONE:                      return 1;
    case GL_SRC_COLOR:                return 2;
    case GL_ONE_MINUS_SRC_COLOR:      return 18;
    case GL_SRC_ALPHA:                return 3;
    case GL_ONE_MINUS_SRC_ALPHA:      return 19;
    case GL_DST_ALPHA:                return 4;
    case GL_ONE_MINUS_DST_ALPHA:      return 20;
    case GL_DST_COLOR:                return 5;
    case GL_ONE_MINUS_DST_COLOR:      return 21;
    case GL_SRC_ALPHA_SATURATE:       return 6;
    case GL_CONSTANT_COLOR:           return 7;
    case GL_ONE_MINUS_CONSTANT_COLOR: return 23;
    case GL_CONSTANT_ALPHA:           return 8;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return 24;
    default:                          return 1;
    }
}

/* The render targets' key fields for this draw (variant_key.h): the
 * framebuffer's drawn targets (fbo.c, ogles2_fb_update), and with software
 * blending each one's format and the blend state, which integer targets
 * never use (ES 3.0 4.1.7; Mesa's state tracker disables it for them):
 * ADD of ONE and ZERO when off. */
static void vk_targets(struct OGLES3Context *ctx, struct OGLES3VkTargets *t)
{
    const struct OGLES3State *s = &ctx->state;
    int i;
    memset(t, 0, sizeof(*t));
    t->soft = ctx->fb_soft_blend;
    for (i = 0; i < ctx->fb_nrt; i++) {
        const struct OGLES3RtFormat *rf = &ctx->fb_rt[i];
        BOOL integer = rf->kind == OGLES3_RT_INT || rf->kind == OGLES3_RT_UINT;
        if (!(ctx->fb_draw_mask & (1 << i)) || rf->kind == OGLES3_RT_NONE)
            continue;
        if (rf->kind == OGLES3_RT_F32 || integer)
            t->f32 |= (UBYTE)(1 << i);
        if (!t->soft)
            continue;
        t->cbufs |= (UBYTE)(1 << i);
        t->fmt[i] = rf->pipe;
        memcpy(t->swz[i], rf->swz, 4);
        if ((s->caps & OGLES3_CAP_BLEND) && !integer) {
            t->blend[i][0] = pipe_func(s->blend_eq_rgb);
            t->blend[i][1] = pipe_factor(s->blend_src_rgb, s->blend_eq_rgb);
            t->blend[i][2] = pipe_factor(s->blend_dst_rgb, s->blend_eq_rgb);
            t->blend[i][3] = pipe_func(s->blend_eq_alpha);
            t->blend[i][4] = pipe_factor(s->blend_src_alpha, s->blend_eq_alpha);
            t->blend[i][5] = pipe_factor(s->blend_dst_alpha, s->blend_eq_alpha);
        } else {
            t->blend[i][0] = t->blend[i][3] = 0;  /* PIPE_BLEND_ADD */
            t->blend[i][1] = t->blend[i][4] = 1;  /* PIPE_BLENDFACTOR_ONE */
            t->blend[i][2] = t->blend[i][5] = 17; /* PIPE_BLENDFACTOR_ZERO */
        }
    }
}

/* Two draws' render target key fields alike: compared a long word at a
 * time whatever their alignment (a structure on the stack is aligned to 2,
 * where runtime.c's memcmp would take bytes). */
static BOOL same_targets(const struct OGLES3VkTargets *a, const struct OGLES3VkTargets *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    int k;
    for (k = 0; k < (int)(sizeof(*a) / 4); k++)
        if (x[k] != y[k])
            return FALSE;
    return TRUE;
}

/* A variant of `stage` fitting `want` in one image, or 0. */
static const struct OGLES3Variant *find_in(const struct OGLES3Image *im, UBYTE stage, const UBYTE *want)
{
    UWORD i;
    for (i = 0; im && i < im->nvariants; i++) {
        const struct OGLES3Variant *v = &im->variants[i];
        if (v->stage == stage &&
            (stage == OGLES3_VARIANT_FS ? ogles2_vk_fs_fits(v->key, want) : ogles2_vk_vs_fits(v->key, want)))
            return v;
    }
    return 0;
}

/* In the program's file, then (fragment variants) its shipped or cached
 * .v3dw and .v3da where the key has 32-bit samplers or alpha to coverage,
 * then its .v3dx images (shipped, the cache's and its older ones); its
 * image in *owner. */
static const struct OGLES3Variant *find_x(struct OGLES3Image *im, UBYTE stage, const UBYTE *want,
                                          const struct OGLES3Image **owner)
{
    const struct OGLES3Variant *v;
    const struct OGLES3Image *x;
    if ((v = find_in(im, stage, want))) {
        *owner = im;
        return v;
    }
    if (stage == OGLES3_VARIANT_FS && ogles2_vk_bits(want) && (x = ogles2_image_wide_file(im)) &&
        (v = find_in(x, stage, want))) {
        *owner = x;
        return v;
    }
    if (stage == OGLES3_VARIANT_FS && want[OGLES3_VK_FS_A2C] && (x = ogles2_image_a2c_file(im)) &&
        (v = find_in(x, stage, want))) {
        *owner = x;
        return v;
    }
    if ((x = ogles2_image_x(im, FALSE)) && (v = find_in(x, stage, want))) {
        *owner = x;
        return v;
    }
    for (x = ogles2_image_x(im, TRUE); x; x = x->xc)
        if ((v = find_in(x, stage, want))) {
            *owner = x;
            return v;
        }
    return 0;
}

/* One entry of a V3DGLSL_COMPILE_VARIANT request: the u32 stage word (with
 * `flags` in its second byte) and the key; its length. */
static ULONG key_entry(UBYTE *e, UBYTE stage, UBYTE flags, const UBYTE *key)
{
    ULONG n = stage == OGLES3_VARIANT_FS ? OGLES3_FS_KEY_BYTES : OGLES3_VS_KEY_BYTES;
    e[0] = stage;
    e[1] = flags;
    e[2] = e[3] = 0;
    memcpy(e + 4, key, n);
    return 4 + n;
}

/* An ES3 program's variants for this draw (from 1.x's choice, *fs and *vs);
 * FALSE leaves them as they are (logged once). A fragment variant not found
 * is compiled with the vertex variant that goes with it, in one request: the
 * compiler gives the vertex key the fragment variant's inputs as its used
 * outputs (V3DGLSL_KEY_OUTPUTS_FROM_FS), as Mesa's v3d_update_compiled_vs
 * does. A vertex variant alone is asked for when only it is missing. */
static BOOL es3_variants(struct OGLES3Context *ctx, struct OGLES3Program *p, GLenum fsmode, UBYTE cbufs,
                         const ULONG *bits, BOOL a2c, const struct OGLES3VkTargets *tg, const struct OGLES3Variant **vs,
                         const struct OGLES3Variant **fs, const struct OGLES3Image **vown,
                         const struct OGLES3Image **fown)
{
    struct OGLES3Image *im = (struct OGLES3Image *)p->image;
    UBYTE want[OGLES3_FS_KEY_BYTES], vwant[OGLES3_VS_KEY_BYTES];
    UBYTE req[8 + OGLES3_FS_KEY_BYTES + OGLES3_VS_KEY_BYTES];
    const struct OGLES3Variant *f, *v = 0;
    int prim;
    if (!(*fs)->key || !(*vs)->key)
        return FALSE;
    prim = fsmode == GL_POINTS ? OGLES3_VK_POINTS
         : fsmode == GL_LINES || fsmode == GL_LINE_LOOP || fsmode == GL_LINE_STRIP ? OGLES3_VK_LINES
                                                                                   : OGLES3_VK_TRIANGLES;
    ogles2_vk_fs_want(want, (*fs)->key, prim, cbufs, bits[1], a2c);
    ogles2_vk_fs_targets(want, tg);
    f = find_x(im, OGLES3_VARIANT_FS, want, fown);
    if (!f) {
        ULONG n = key_entry(req, OGLES3_VARIANT_FS, 0, want);
        ogles2_vk_vs_want(vwant, (*vs)->key, 0, 0, bits[0]);
        n += key_entry(req + n, OGLES3_VARIANT_VS, V3DGLSL_KEY_OUTPUTS_FROM_FS >> 8, vwant);
        if (ogles2_image_compile_variants(ctx, im, req, n))
            f = find_x(im, OGLES3_VARIANT_FS, want, fown);
    }
    if (f) {
        ogles2_vk_vs_want(vwant, (*vs)->key, f->num_inputs, f->input_slots, bits[0]);
        v = find_x(im, OGLES3_VARIANT_VS, vwant, vown);
        if (!v && ogles2_image_compile_variants(ctx, im, req, key_entry(req, OGLES3_VARIANT_VS, 0, vwant)))
            v = find_x(im, OGLES3_VARIANT_VS, vwant, vown);
        if (v) {
            *fs = f;
            *vs = v;
            return TRUE;
        }
    }
    if (!p->var_logged) {
        static const char digits[] = "0123456789abcdef";
        char hex[25];
        int k;
        for (k = 0; k < 12; k++) {
            hex[2 * k] = digits[im->hash[k] >> 4];
            hex[2 * k + 1] = digits[im->hash[k] & 15];
        }
        hex[24] = 0;
        p->var_logged = TRUE;
        ogles2_log("draw: no variant for program %s in this state and no compiler for it: the closest is used",
                   (ULONG)hex);
    }
    *fown = im;
    *vown = im;
    return FALSE;
}

/* The smallest and largest of `count` 16-bit indices, two a long word when
 * they are long-aligned (Emu68 charges for the memory accesses), without
 * branches (d's sign bit, spread by the shift, picks). A function of its
 * own: in draw_elements GCC kept the minimum in the frame. Faster than a
 * word at a time and than 1.x's loop in draw_elements (tests/perf/drawbench). */
#define MINMAX(l, h, x) (d = (x) - (l), (l) += d & (d >> 31), d = (h) - (x), (h) -= d & (d >> 31))
static __attribute__((noinline)) void scan_u16(const UWORD *in, ULONG count, ULONG *lo_out, ULONG *hi_out)
{
    LONG l = 0x7FFFFFFF, h = 0, d;
    ULONG k = 0;
    if (!((ULONG)in & 3)) {
        const ULONG *in2 = (const ULONG *)in;
        /* Eight a pass, in two independent minimum/maximum pairs: Emu68
         * pays for the loop's branch each pass, and the ARM core runs the
         * two chains side by side (tests/perf/drawbench's client arrays) */
        LONG l2 = 0x7FFFFFFF, h2 = 0;
        for (; k + 7 < count; k += 8) {
            ULONG w0 = in2[0], w1 = in2[1], w2 = in2[2], w3 = in2[3];
            in2 += 4;
            MINMAX(l, h, (LONG)(w0 >> 16));
            MINMAX(l2, h2, (LONG)(w0 & 0xFFFF));
            MINMAX(l, h, (LONG)(w1 >> 16));
            MINMAX(l2, h2, (LONG)(w1 & 0xFFFF));
            MINMAX(l, h, (LONG)(w2 >> 16));
            MINMAX(l2, h2, (LONG)(w2 & 0xFFFF));
            MINMAX(l, h, (LONG)(w3 >> 16));
            MINMAX(l2, h2, (LONG)(w3 & 0xFFFF));
        }
        d = l2 - l; /* the pairs merged: the smaller minimum, the larger maximum */
        l += d & (d >> 31);
        d = h - h2;
        h -= d & (d >> 31);
        for (; k + 1 < count; k += 2) {
            ULONG w = *in2++;
            LONG x = (LONG)(w >> 16), y = (LONG)(w & 0xFFFF);
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
            d = y - l;
            l += d & (d >> 31);
            d = h - y;
            h -= d & (d >> 31);
        }
    }
    for (; k < count; k++) {
        LONG x = in[k];
        d = x - l;
        l += d & (d >> 31);
        d = h - x;
        h -= d & (d >> 31);
    }
    *lo_out = (ULONG)l;
    *hi_out = (ULONG)h;
}

/* An indirect draw's packets: draw_packets' first 12 bytes, then (indexed)
 * INDEX_BUFFER_SETUP and INDIRECT_INDEXED_INSTANCED_PRIM_LIST, or
 * INDIRECT_VERTEX_ARRAY_INSTANCED_PRIMS: the GPU reads one command record
 * at `ind` (Mesa's v3dx_draw.c; the callers checked packets_ok). Out of
 * line: draw_arrays' body as small as it was (tests/perf/drawbench). */
static __attribute__((noinline)) void indirect_packets(V3DContext *v, const struct OGLES3Variant *cs,
                                                       const struct OGLES3Variant *vs, ULONG rec, int nslots,
                                                       GLenum mode, const struct elements *el, GLenum itype_gl,
                                                       ULONG idx_addr, ULONG idx_size, ULONG ind)
{
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, el ? 32 : 23);
    if (!b)
        return;
    draw_packets(b, cs->vcm_cache_size, vs->vcm_cache_size, rec, nslots ? nslots : 1, (UBYTE)mode, -1, 0, 0, 0, 0, 1,
                 0); /* bytes 0-11: point size, VCM cache, shader state */
    if (el) {
        b[12] = v3d_OP_INDEX_BUFFER_SETUP;
        put_le32(b + 13, idx_addr);
        put_le32(b + 17, idx_size);
        b[21] = v3d_OP_INDIRECT_INDEXED_INSTANCED_PRIM_LIST;
        b[22] = (UBYTE)(((itype_gl == GL_UNSIGNED_INT ? v3d_INDEX_TYPE_32_BIT
                          : itype_gl == GL_UNSIGNED_SHORT ? v3d_INDEX_TYPE_16_BIT : v3d_INDEX_TYPE_8_BIT) << 6) |
                        (mode & 63));
        put_le32(b + 23, 1 | (el->restart ? 0x80000000UL : 0)); /* one record, primitive restart */
        put_le32(b + 27, ind);
        b[31] = 5; /* DrawElementsIndirectCommand: 20 bytes */
    } else {
        b[12] = v3d_OP_INDIRECT_VERTEX_ARRAY_INSTANCED_PRIMS;
        b[13] = (UBYTE)mode;
        put_le32(b + 14, 1);
        put_le32(b + 18, ind);
        b[22] = 4; /* DrawArraysIndirectCommand: 16 bytes */
    }
}

/* A draw whose counts or indices the CPU does not read (docs/GLES3_PLAN.md,
 * the remaining gaps, item 5): indices on the GPU (gpu_elements), or an
 * indirect command (the indirect draws). Every array then covers its whole
 * buffer from vertex 0 (`whole`: the CPU does not know which vertices are
 * read), the GPU's words where it wrote them (gpu_array). */
struct gpu_draw
{
    ULONG idx_addr, idx_bytes; /* 32-bit indices read where they lie (0: copied through `el`) */
    ULONG ioffset;             /* the base vertex added to them (INDEXED_PRIM_LIST's index offset) */
    ULONG ind_addr;            /* an indirect command's address (0: none) */
    BOOL fed;                  /* transform feedback wrote one of them in the open job */
};

/* A gpu_draw's array: its buffer's whole vertices from the array's start.
 * Out of line: draw_arrays' vertex loop stays as small as it was (the
 * draws were 10-20% slower with it inline, tests/perf/drawbench). */
static __attribute__((noinline)) GLsizei whole_vertices(struct OGLES3Array *arr)
{
    ULONG es, st, off = (ULONG)arr->pointer, size = OGLES3_BH(arr->buffer)->size;
    fmt_current(arr);
    es = arr->fmt_in_bytes;
    st = arr->stride ? (ULONG)arr->stride : es;
    return off + es <= size ? (GLsizei)((size - off - es) / st + 1) : 0;
}

/* draw_arrays' vertex sources once a buffer was ever given to the GPU to
 * write (ogles2_gpu_writes_seen), or for a gpu_draw (`whole`: every array
 * its whole buffer from vertex 0): the GPU's words read where they lie
 * (gpu_source), else copy_array as in the usual loop. 1: done, 0: an array
 * without a complete vertex (nothing drawn), -1: no memory. */
static __attribute__((noinline)) int gpu_sources(struct OGLES3Context *ctx, struct OGLES3Program *p, int nslots,
                                                 GLint first, GLsizei count, GLsizei instances, BOOL whole,
                                                 struct attr_src *src)
{
    int i;
    for (i = 0; i < nslots; i++) {
        GLuint g = p->slot_attrib[i];
        struct OGLES3Array *arr;
        GLint f0 = first;
        GLsizei n0 = count;
        if (g >= OGLES3_MAX_VERTEX_ATTRIBS || !ctx->array[g].enabled) {
            if (!constant_attrib(ctx, g >= OGLES3_MAX_VERTEX_ATTRIBS ? 0 : g, &src[i]))
                return -1;
            continue;
        }
        arr = &ctx->array[g];
        if (arr->zero_stride) {
            f0 = 0;
            n0 = 1;
        } else if (whole) {
            /* the whole buffer, from vertex 0 (an instanced array's too: the
             * instances may be unknown); the GPU's own words cost nothing */
            f0 = 0;
            if (!(n0 = whole_vertices(arr)))
                return 0;
        } else if (arr->divisor) {
            f0 = 0;
            n0 = (GLsizei)(((ULONG)instances + arr->divisor - 1) / arr->divisor);
        }
        if (!(arr->buffer && gpu_source(ctx, arr, f0, &src[i])) && !copy_array(ctx, arr, f0, n0, &src[i], FALSE))
            return -1;
        if (arr->zero_stride)
            src[i].stride = 0;
        else if (arr->divisor)
            src[i].divisor = (UWORD)(arr->divisor > 0xffff ? 0xffff : arr->divisor);
    }
    return 1;
}

/* Draws with program p and the context's state; arguments are valid. The
 * vertices [first, first+count) are copied; without `el` they are drawn in
 * order, with it through its indices. With `gd` (gpu_draw), every array
 * whole, from vertex 0. */
static void draw_arrays(struct OGLES3Context *ctx, struct OGLES3Program *p, GLenum mode,
                        GLint first, GLsizei count, const struct elements *el, GLsizei instances,
                        const struct gpu_draw *gd)
{
    const struct OGLES3Image *im = p->image;
    const struct OGLES3Variant *cs, *vs, *fs;
    const struct OGLES3Image *fim; /* the fragment shader's image */
    GLenum fsmode;
    struct OGLES3Draw *d;
    V3DContext *v = ctx->v3d;
    struct attr_src src[OGLES3_MAX_VERTEX_ATTRIBS];
    struct tex_states tst;
    BOOL vid;        /* a shader reads gl_VertexID: the real vertex numbers (below) */
    BOOL a2c;        /* alpha to coverage in an ES3 program's key */
    ULONG bits[2];   /* an ES3 program's 32-bit samplers, vertex and fragment */
    struct OGLES3VkTargets tg; /* and its render targets' key fields */
    BOOL tg_made = FALSE;      /* tg holds them (vk_targets) */
    GLenum itype_gl; /* the indices' type as the GPU gets them */
    BOOL xfb;        /* it feeds back (transform feedback, xfb.c) */
    ULONG unif[3], code[3], defattr, rec, idx_addr = 0, idx_size = 0, ioffset;
    int a, nslots = 0, i;

    /* GL_RASTERIZER_DISCARD: no fragments, and without transform feedback a
     * draw then has no effect at all: nothing is drawn. With it, binning
     * still feeds back (CFG_BITS discards every primitive, emit_state). */
    if ((ctx->state.caps & OGLES3_CAP_RASTERIZER_DISCARD) && !ogles2_xfb_drawing(ctx))
        return;
    if (!(d = draw_state(ctx))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    mark(ctx, 0);
    /* The program's variants for this primitive mode, as at its last draw
     * (they and their code stay until it is linked again). The fragment
     * shader's is the class the rasterizer sees: triangles in glPolygonMode's
     * GL_LINE or GL_POINT are lines or points to it (Mesa's
     * v3d_rasterizes_lines/points). */
    fsmode = mode >= GL_TRIANGLES && ctx->state.polygon_mode != GL_FILL
                 ? (ctx->state.polygon_mode == GL_POINT ? GL_POINTS : GL_LINES) : mode;
    /* A 4x MSAA default framebuffer, or multisampled renderbuffers, with
     * GL_SAMPLE_ALPHA_TO_COVERAGE (Mesa turns alpha into the sample mask in
     * the fragment shader). */
    a2c = (ctx->state.caps & OGLES3_CAP_SAMPLE_ALPHA_TO_COVERAGE) != 0 &&
          (ctx->draw_fb ? ogles2_fbo_samples(ctx->draw_fb) != 0 : ctx->msaa);
    fim = im;
    bits[0] = bits[1] = 0;
    if (im->format2) {
        /* An OpenGL ES 3 program: both in the variants' keys (es3_variants),
         * and the render targets'. */
        if (ogles2_wide_seen) { /* sampler_bits' own first test, without its calls */
            bits[0] = sampler_bits(ctx, p, 0);
            bits[1] = sampler_bits(ctx, p, 1);
        }
        /* The targets' key: with software blending all of vk_targets';
         * without, its 32-bit targets alone (fb_wide), compared as that
         * below and made in full only for a new choice of variants. */
        if (ctx->fb_soft_blend) {
            vk_targets(ctx, &tg);
            tg_made = TRUE;
        }
    } else {
        /* A GLES2 one: from the program's 32-bit-sampler variants while a
         * float or depth texture is bound to one of its samplers (program.c,
         * the .v3dw), else its alpha-to-coverage variants (the .v3da). Not
         * both: no file holds both, and the 32-bit ones keep theirs. */
        if (ogles2_wide_seen && wants_wide(ctx, p)) {
            const struct OGLES3Image *w = ogles2_image_wide((struct OGLES3Image *)im);
            if (w)
                fim = w;
        }
        if (a2c && fim == im) {
            const struct OGLES3Image *a = ogles2_image_a2c((struct OGLES3Image *)im);
            if (a)
                fim = a;
        }
        a2c = FALSE;
    }
    if (p->var_ok && p->var_mode == mode && p->var_fsmode == fsmode && p->var_fim == fim &&
        p->var_nrt == ctx->fb_nrt && p->var_cbufs == (ctx->draw_fb ? ctx->fb_draw_mask : 1) &&
        p->var_bits[0] == bits[0] && p->var_bits[1] == bits[1] && p->var_a2c == a2c &&
        (!im->format2 || (tg_made ? same_targets(&p->var_tg, &tg)
                                  : !p->var_tg.soft && p->var_tg.f32 == ctx->fb_wide))) { /* an ES3 program's */
        cs = p->var[0];
        vs = p->var[1];
        fs = p->var[2];
        code[0] = p->var_code[0];
        code[1] = p->var_code[1];
        code[2] = p->var_code[2];
    } else {
        UBYTE cbufs = (UBYTE)((1 << ctx->fb_nrt) - 1);
        cs = ogles2_image_variant(im, OGLES3_VARIANT_CS, 0, mode);
        vs = ogles2_image_variant(im, OGLES3_VARIANT_VS, 0, mode);
        fs = ogles2_image_variant(fim, OGLES3_VARIANT_FS, cbufs, fsmode);
        if (!cs || !vs || !fs) {
            ogles2_log("glDrawArrays: the program has no shader for this primitive; nothing drawn");
            return;
        }
        if (d->missed != im && (fs->is_points != (fsmode == GL_POINTS) ||
                                fs->is_lines != (fsmode == GL_LINES || fsmode == GL_LINE_LOOP ||
                                                 fsmode == GL_LINE_STRIP))) {
            d->missed = im; /* plan 7.8: the closest variant is used; logged once per program */
            ogles2_log("glDrawArrays: no fragment shader variant for primitive %ld; using the closest",
                       (ULONG)mode);
        }
        if (d->missed_rt != im && (fs->cbufs ? fs->cbufs : 1) != cbufs) {
            d->missed_rt = im; /* logged once per program */
            ogles2_log("glDrawArrays: no fragment shader variant for %ld render targets "
                       "(EXT_draw_buffers): the program draws into the first only", (ULONG)ctx->fb_nrt);
        }
        {
            /* An OpenGL ES 3 program: the variants for this state's keys
             * (above); a GLES2 one keeps 1.x's. */
            const struct OGLES3Image *vown = im, *fown = fim;
            if (im->format2 && !tg_made) {
                vk_targets(ctx, &tg);
                tg_made = TRUE;
            }
            if (im->format2)
                es3_variants(ctx, p, fsmode, ctx->draw_fb ? ctx->fb_draw_mask : 1, bits, a2c, &tg, &vs, &fs,
                             &vown, &fown);
            code[0] = code_address(d, im, cs);
            code[1] = code_address(d, vown, vs);
            code[2] = code_address(d, fown, fs);
        }
        if (!code[0] || !code[1] || !code[2]) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        p->var[0] = cs;
        p->var[1] = vs;
        p->var[2] = fs;
        p->var_code[0] = code[0];
        p->var_code[1] = code[1];
        p->var_code[2] = code[2];
        p->var_mode = mode;
        p->var_fsmode = fsmode;
        p->var_fim = fim;
        p->var_nrt = ctx->fb_nrt;
        p->var_cbufs = (UBYTE)(ctx->draw_fb ? ctx->fb_draw_mask : 1);
        p->var_bits[0] = bits[0];
        p->var_bits[1] = bits[1];
        p->var_a2c = a2c;
        if (im->format2)
            p->var_tg = tg;
        p->var_f32 = fs->key ? ogles2_vk_fs_f32(fs->key) : 0;
        p->var_ok = TRUE;
    }

    /* A colour write of the wrong width (16-bit floats into a 32-bit float
     * or integer target, or the reverse) upsets the GPU until a power cycle
     * (the A1200, 2026-10-26: wrong pixels everywhere after, a reboot did not
     * help): a draw whose fragment variant does not write each drawn target
     * at its width (a GLES2 program's, or the closest of an ES3 program's
     * without a compiler) is dropped, logged once. draw_clear's depth and
     * stencil program writes no colour. */
    if (p != &d->zs_quad) {
        UBYTE need = ctx->fb_wide, has = p->var_f32, cb = fs->cbufs ? fs->cbufs : 1;
        if ((need & cb) != (has & cb)) { /* a target the variant does not write gets no write at all */
            if (!d->wrong_width_logged) {
                d->wrong_width_logged = TRUE;
                ogles2_log("draw: the fragment shader would write a 32-bit float or integer target at the wrong "
                           "width (targets 0x%lx, the variant 0x%lx): not drawn (logged once)",
                           (ULONG)need, (ULONG)has);
            }
            return;
        }
    }
    if (v->frame_active && ctx->job_fb == ctx->draw_fb)
        ctx->job_discard = 0; /* ogles2_job_begin's first case, without its call: the job is open */
    else
        ogles2_job_begin(ctx);
    vid = cs->uses_vid || vs->uses_vid;
    if (__builtin_expect(gd != 0 || ogles2_gpu_writes_seen, 0))
        ctx->draw_fed = gd && gd->fed; /* gpu_array adds the arrays' (stale without GPU writes: never read) */
    itype_gl = !el ? 0
             : vid && el->type != GL_UNSIGNED_INT &&
                       (ULONG)first + (ULONG)count - 1 > (el->type == GL_UNSIGNED_SHORT ? 0xffffUL : 0xffUL)
                   ? GL_UNSIGNED_INT : el->type;

    /* State buffer: uniform streams, vertex data, the record. */
    mark(ctx, 1);
    update_state_vars(ctx, p);
    memset(&tst, 0, TEX_STATES_CLEAR);
    {
        const struct OGLES3Variant *vv[3];
        vv[0] = cs; vv[1] = vs; vv[2] = fs;
        /* Their work only for a variant that asks for it (most ask for none) */
        ULONG any = vv[0]->uses_ubo | vv[1]->uses_ubo | vv[2]->uses_ubo | vv[0]->uses_blocks | vv[1]->uses_blocks |
                    vv[2]->uses_blocks | vv[0]->uses_storage | vv[1]->uses_storage | vv[2]->uses_storage |
                    vv[0]->spill_size | vv[1]->spill_size | vv[2]->spill_size | vv[0]->uses_images |
                    vv[1]->uses_images | vv[2]->uses_images;
        if (any && (!ubo_copies(ctx, p, vv, &tst) || !spill_area(ctx, vv) || !image_states(ctx, p, vv, &tst)))
            goto nomem;
    }
    if (!texture_states(ctx, p, &tst))
        goto nomem;
    mark(ctx, 2);
    uniform_state(ctx);
    unif[0] = vertex_uniforms(ctx, p, 0, cs, &tst);
    unif[1] = vertex_uniforms(ctx, p, 1, vs, &tst);
    unif[2] = write_uniforms(ctx, p, fs, &tst);
    defattr = default_attribs(ctx, d);
    if (!unif[0] || !unif[1] || !unif[2] || !defattr)
        goto nomem;
    mark(ctx, 3);
    /* One attribute record per vertex input slot, from the generic attribute
     * bound to it (a matrix: one slot and one generic attribute per column). */
    /* Mapped at the program's first draw (attribute locations are fixed
     * at link). */
    if (p->nslots == 0xff) {
        int k, cs;
        for (a = 0; a < im->nattribs; a++)
            for (k = 0; k < im->attribs[a].columns; k++)
                if ((cs = column_slot(&im->attribs[a], k)) >= 0 && cs + 1 > nslots)
                    nslots = cs + 1;
        if (nslots > 16)
            nslots = 16;
        for (i = 0; i < nslots; i++) {
            GLuint g = 0xff;
            for (a = 0; a < im->nattribs; a++)
                for (k = 0; k < im->attribs[a].columns; k++)
                    if (column_slot(&im->attribs[a], k) == (int)i)
                        g = p->attrib_index[a] + k;
            p->slot_attrib[i] = (UBYTE)(g < OGLES3_MAX_VERTEX_ATTRIBS ? g : 0xff);
        }
        p->nslots = (UBYTE)nslots;
    }
    nslots = p->nslots;
    if (__builtin_expect(gd != 0 || ogles2_gpu_writes_seen, 0)) {
        /* GPU-written buffers, or a gpu_draw's whole arrays: a loop of its
         * own, so the usual one below stays as it was (tests/perf/drawbench) */
        int r = gpu_sources(ctx, p, nslots, first, count, instances, gd != 0, src);
        if (r < 0)
            goto nomem;
        if (!r)
            return; /* an array without a complete vertex: nothing drawn */
    } else {
        for (i = 0; i < nslots; i++) {
            GLuint g = p->slot_attrib[i];
            struct OGLES3Array *arr;
            GLint f0 = first;
            GLsizei n0 = count;
            if (g >= OGLES3_MAX_VERTEX_ATTRIBS || !ctx->array[g].enabled) {
                /* a disabled array, or a slot no attribute claims (current value 0) */
                if (!constant_attrib(ctx, g >= OGLES3_MAX_VERTEX_ATTRIBS ? 0 : g, &src[i]))
                    goto nomem;
                continue;
            }
            /* One call for every kind of array: copy_array inlined here alone,
             * as in 1.x (tests/perf/drawbench). */
            arr = &ctx->array[g];
            if (arr->zero_stride) {
                /* ES 3.1: a binding of stride 0, every vertex its first element:
                 * one element read as a current value is (record stride 0) */
                f0 = 0;
                n0 = 1;
            } else if (arr->divisor) {
                /* ES 3.0's instanced arrays: an element per `divisor` instances,
                 * from element 0, whatever the vertices (the record's divisor,
                 * 16 bits: Mesa clamps, v3dx_state.c; a divisor at or above the
                 * instance count reads element 0 as the clamped one does but
                 * past 65535 instances, a deviation). */
                f0 = 0;
                n0 = (GLsizei)(((ULONG)instances + arr->divisor - 1) / arr->divisor);
            }
            if (!copy_array(ctx, arr, f0, n0, &src[i],
                            i == 0 && !arr->zero_stride && !arr->divisor && (ctx->probe & OGLES3_PROBE_ADDR) &&
                                ctx->probe_addr))
                goto nomem;
            if (arr->zero_stride)
                src[i].stride = 0;
            else if (arr->divisor)
                src[i].divisor = (UWORD)(arr->divisor > 0xffff ? 0xffff : arr->divisor);
        }
    }
    /* An integer array's missing components read the defaults: its w an
     * integer 1 (Mesa's v3d_vertex_state_create), not the shared block's
     * 1.0, in a block of the draw's own. */
    for (i = 0; i < nslots; i++)
        if (src[i].read_int && src[i].stride) {
            ULONG *dv = claim(v, 16 * 16, 16);
            int k;
            if (!dv)
                goto nomem;
            memcpy(dv, (const void *)defattr, 16 * 16);
            for (k = 0; k < nslots; k++)
                if (src[k].read_int && src[k].stride)
                    dv[k * 4 + 3] = le32(1);
            defattr = (ULONG)dv;
            break;
        }
    /* gl_VertexID (GLSL ES 3.00, docs/GLES3_PLAN.md stage A item 4): a
     * shader that reads it sees the real vertex numbers, so the draw numbers
     * from `first` (the indices as given, plus the base vertex) and each
     * array's address moves back by `first` elements: the GPU reads
     * address + index x stride, so the copies (made from `first`) stay as
     * small as for any draw. An address that would leave the memory the GPU
     * reaches gets a copy from element 0 instead. */
    if (vid && first > 0)
        for (i = 0; i < nslots; i++) {
            ULONG back = (ULONG)first * src[i].stride;
            if (!src[i].stride || src[i].divisor)
                continue; /* a current value (every vertex reads the same), an instanced array */
            if (src[i].address >= back && ogles2_device_gpu_reachable(src[i].address - back, 4)) {
                src[i].address -= back;
                continue;
            }
            if (!copy_array_again(ctx, &ctx->array[p->slot_attrib[i]], first + count, &src[i]))
                goto nomem;
        }
    mark(ctx, 4);
    if (!(rec = shader_record(ctx, p, defattr, code, unif, cs, vs, fs, mode, src, nslots)))
        goto nomem;
    mark(ctx, 5);
    if (el) {
        /* The indices, rebased to the copied range: 8-bit as they are,
         * 16- and 32-bit swapped to little-endian (plan 7.4). With
         * gl_VertexID (above) they are the real vertex numbers: as given
         * plus the base vertex (first = the smallest + the base vertex),
         * written 32-bit when that outgrows their type. */
        ULONG sub = vid ? el->min - (ULONG)first : el->min, k;
        ULONG isize = (ULONG)el->count * index_size(itype_gl);
        UBYTE *ib;
        struct index_copy *ic = &d->ic[IC_SLOT(el->uid, el->indices, el->count)];
        if (__builtin_expect(gd != 0, 0) && gd->idx_addr) { /* 32-bit, as they are: nothing rebased (the arrays are whole) */
            ib = (UBYTE *)gd->idx_addr;
            isize = gd->idx_bytes;
            goto indices_done;
        }
        if (el->uid && ic->job == ctx->job_serial && ic->uid == el->uid && ic->serial == el->serial &&
            ic->ptr == el->indices && ic->count == el->count && ic->type == el->type && ic->itype == itype_gl &&
            ic->restart == el->restart && ic->sub == sub) {
            ib = (UBYTE *)ic->addr; /* this job's copy of the same indices, rebased the same */
            goto indices_done;
        }
        if (!(ib = claim(v, isize + 4, 4)))
            goto nomem;
        if (el->uid) {
            ic->job = ctx->job_serial;
            ic->uid = el->uid;
            ic->serial = el->serial;
            ic->ptr = el->indices;
            ic->count = el->count;
            ic->type = el->type;
            ic->itype = itype_gl;
            ic->restart = el->restart;
            ic->sub = sub;
            ic->addr = (ULONG)ib;
        }
        if (el->restart) {
            /* Primitive restart: the restart index stays the largest of the
             * type the GPU gets (the packets' restart bit, below), the
             * others are rebased. */
            ULONG r = el->type == GL_UNSIGNED_INT ? 0xFFFFFFFFUL : el->type == GL_UNSIGNED_SHORT ? 0xFFFFUL : 0xFFUL;
            ULONG ro = itype_gl == GL_UNSIGNED_INT ? 0xFFFFFFFFUL : itype_gl == GL_UNSIGNED_SHORT ? 0xFFFFUL : 0xFFUL;
            for (k = 0; k < (ULONG)el->count; k++) {
                ULONG x = el->type == GL_UNSIGNED_INT ? ((const ULONG *)el->indices)[k]
                        : el->type == GL_UNSIGNED_SHORT ? ((const UWORD *)el->indices)[k]
                                                        : ((const UBYTE *)el->indices)[k];
                x = x == r ? ro : x - sub;
                if (itype_gl == GL_UNSIGNED_INT)
                    ((ULONG *)ib)[k] = le32(x);
                else if (itype_gl == GL_UNSIGNED_SHORT) {
                    ib[2 * k] = (UBYTE)x;
                    ib[2 * k + 1] = (UBYTE)(x >> 8);
                } else
                    ib[k] = (UBYTE)x;
            }
        } else if (itype_gl != el->type) { /* widened: 8- or 16-bit indices to 32 */
            ULONG *out = (ULONG *)ib;
            for (k = 0; k < (ULONG)el->count; k++)
                out[k] = le32((el->type == GL_UNSIGNED_SHORT ? (ULONG)((const UWORD *)el->indices)[k]
                                                              : (ULONG)((const UBYTE *)el->indices)[k]) - sub);
        } else if (el->type == GL_UNSIGNED_INT) {
            swap_indices((ULONG *)ib, el->indices, (ULONG)el->count, sub);
        } else if (el->type == GL_UNSIGNED_SHORT) {
            const UWORD *in = el->indices;
            k = 0;
            if (!((ULONG)in & 3)) {
                /* Two a long word (ib is long-aligned): both rebased by one
                 * subtraction (every index is at least the smallest, so the
                 * low one never borrows from the high), then each half's
                 * bytes swapped. */
                const ULONG *in2 = (const ULONG *)in;
                ULONG *out2 = (ULONG *)ib, sub2 = sub * 0x10001UL;
                for (; k + 1 < (ULONG)el->count; k += 2) {
                    ULONG w = *in2++ - sub2;
                    *out2++ = ((w & 0x00FF00FFUL) << 8) | ((w >> 8) & 0x00FF00FFUL);
                }
            }
            for (; k < (ULONG)el->count; k++) {
                UWORD x = (UWORD)(in[k] - sub);
                ib[2 * k] = (UBYTE)x;
                ib[2 * k + 1] = (UBYTE)(x >> 8);
            }
        } else {
            const UBYTE *in = el->indices;
            for (k = 0; k < (ULONG)el->count; k++)
                ib[k] = (UBYTE)(in[k] - sub);
        }
    indices_done:
        idx_addr = (ULONG)ib;
        idx_size = isize;
        ctx->n_indices += el->count;
    }

    /* Binning list. */
    ioffset = __builtin_expect(gd != 0, 0) ? gd->ioffset : 0;
    mark(ctx, 6);
    v->current_buf = &v->binning_buf[v->build_slot];
    emit_state(ctx, &d->dd, (int)mode, fs);
    /* Transform feedback: its specs and buffers before the primitives, its
     * flush after them (xfb.c; never indexed: draw_elements refuses that). */
    if (__builtin_expect(ctx->job_xfb, 0) && ctx->xfb_draws != ctx->xfb_waited)
        xfb_wait(ctx, p);
    xfb = ctx->xfb_begun && ogles2_xfb_before_draw(ctx, mode, cs->writes_psiz); /* never begun: never used */
    if (__builtin_expect(gd != 0, 0) && gd->ind_addr) {
        indirect_packets(v, cs, vs, rec, nslots, mode, el, itype_gl, idx_addr, idx_size, gd->ind_addr);
        mark(ctx, 7);
        return;
    }
    if (packets_ok(v)) {
        int itype = !el ? -1
                  : itype_gl == GL_UNSIGNED_INT ? v3d_INDEX_TYPE_32_BIT
                  : itype_gl == GL_UNSIGNED_SHORT ? v3d_INDEX_TYPE_16_BIT : v3d_INDEX_TYPE_8_BIT;
        UBYTE *b = v3d_buffer_claim_memory(v->current_buf, (el ? 31 : 22) + (instances > 1 ? 4 : 0));
        if (b)
            draw_packets(b, cs->vcm_cache_size, vs->vcm_cache_size, rec, nslots ? nslots : 1, (UBYTE)mode,
                         itype, idx_addr, idx_size,
                         el ? ((ULONG)el->count & 0x7FFFFFFFUL) | (el->restart ? 0x80000000UL : 0) : (ULONG)count,
                         vid ? (ULONG)first : 0, (ULONG)instances, ioffset);
        if (xfb)
            ogles2_xfb_after_draw(ctx, mode, count, instances);
        mark(ctx, 7);
        return;
    }
    if (instances > 1) { /* v3dcore has no writers for the instanced packets: ours */
        UBYTE *b = v3d_buffer_claim_memory(v->current_buf, el ? 35 : 26);
        if (b)
            draw_packets(b, cs->vcm_cache_size, vs->vcm_cache_size, rec, nslots ? nslots : 1, (UBYTE)mode,
                         el ? (itype_gl == GL_UNSIGNED_INT ? v3d_INDEX_TYPE_32_BIT
                               : itype_gl == GL_UNSIGNED_SHORT ? v3d_INDEX_TYPE_16_BIT : v3d_INDEX_TYPE_8_BIT) : -1,
                         idx_addr, idx_size,
                         el ? ((ULONG)el->count & 0x7FFFFFFFUL) | (el->restart ? 0x80000000UL : 0) : (ULONG)count,
                         vid ? (ULONG)first : 0, (ULONG)instances, ioffset);
        if (xfb)
            ogles2_xfb_after_draw(ctx, mode, count, instances);
        mark(ctx, 7);
        return;
    }
    PointSize(v, 1.0f);
    VCMCacheSize(v, cs->vcm_cache_size, vs->vcm_cache_size); /* Mesa: before every shader state */
    glShaderState(v, rec, nslots ? nslots : 1);
    if (el) {
        /* Mesa's order: INDEX_BUFFER_SETUP, then INDEXED_PRIM_LIST.
         * Primitive restart: ES 3.0's fixed index only (none in GLES2). */
        IndexBufferSetup(v, idx_addr, idx_size);
        IndexedPrimList(v, (UBYTE)mode, itype_gl == GL_UNSIGNED_INT     ? v3d_INDEX_TYPE_32_BIT
                                        : itype_gl == GL_UNSIGNED_SHORT ? v3d_INDEX_TYPE_16_BIT
                                                                        : v3d_INDEX_TYPE_8_BIT,
                        (ULONG)el->count, el->restart, 0);
    } else {
        /* The copies start at `first`: vertex 0 is their first, except for
         * gl_VertexID (the addresses moved back, above). */
        VertexArrayPrims(v, (UBYTE)mode, (ULONG)count, vid ? (ULONG)first : 0);
    }
    if (xfb)
        ogles2_xfb_after_draw(ctx, mode, count, instances);
    mark(ctx, 7);
    return;

nomem:
    ogles2_log("glDrawArrays: out of memory for the draw's GPU data; frame dropped");
    v->frame_corrupted = TRUE;
    ogles2_error(ctx, GL_OUT_OF_MEMORY);
}

/* The vertex input slot of column k of an attribute (a matrix's columns are
 * generic attributes of their own), -1 if the shader does not read it. */
static int column_slot(const struct OGLES3ImageAttrib *at, int k)
{
    int j, n = 0;
    if (at->slot == 0xff)
        return -1;
    if (!at->colmask) /* every column, from the slot on */
        return at->slot + k;
    if (!(at->colmask & (1 << k)))
        return -1;
    for (j = 0; j < k; j++)
        if (at->colmask & (1 << j))
            n++;
    return at->slot + n;
}

/* Every enabled array the program reads has data, and a buffer's holds
 * vertices [0, end) (GLES2 leaves reading past it undefined: not drawn). */
static BOOL arrays_ok(struct OGLES3Context *ctx, const struct OGLES3Program *p, ULONG end,
                      GLsizei instances, const char *fn)
{
    int a;
    for (a = 0; a < p->image->nattribs; a++) {
        struct OGLES3Array *arr = &ctx->array[p->attrib_index[a]];
        ULONG end_a = end;
        if (p->image->attribs[a].slot == 0xff || !arr->enabled)
            continue;
        if (arr->zero_stride) /* ES 3.1: a binding of stride 0, its first element */
            end_a = 1;
        else if (arr->divisor) /* an instanced array's elements */
            end_a = ((ULONG)instances + arr->divisor - 1) / arr->divisor;
        if (arr->buffer) {
            ULONG es, st, off = (ULONG)arr->pointer, size = OGLES3_BH(arr->buffer)->size;
            fmt_current(arr);
            es = arr->fmt_in_bytes;
            st = arr->stride ? (ULONG)arr->stride : es;
            if (ogles2_gpu_writes_seen) {
                ULONG ga, gs, gv;
                if (!gpu_array(ctx, arr, &ga, &gs, &gv))
                    ogles2_buffer_cpu(ctx, arr->buffer); /* transform feedback's results, before the job is built */
            }
            if (off + es > size || (size - off - es) / st + 1 < end_a) {
                if (fn)
                    ogles2_log("%s: attribute %ld reads past the end of buffer %ld; nothing drawn",
                               (ULONG)fn, (ULONG)p->attrib_index[a], (ULONG)ogles2_buffer_name(arr->buffer));
                return FALSE;
            }
        } else if (!arr->pointer) {
            if (fn)
                ogles2_log("%s: attribute %ld is enabled without an array; nothing drawn", (ULONG)fn,
                           (ULONG)p->attrib_index[a]);
            return FALSE;
        }
    }
    return TRUE;
}

static void draw_instances(struct OGLES3Context *ctx, GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    ULONG t0 = ogles2_clock_us();
    struct OGLES3Program *p;

    if (!ogles2_entry_valid(ctx))
        return;
    mark(ctx, -1);
    if (mode > GL_TRIANGLE_FAN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (first < 0 || count < 0 || instances < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ctx->xfb_begun && !ogles2_xfb_draw_ok(ctx, mode, count, instances)) /* GL_INVALID_OPERATION (ES 3.0 2.15.2) */
        return;
    if (!ogles2_fb_ready(ctx)) /* GL_INVALID_FRAMEBUFFER_OPERATION */
        return;
    p = ctx->program;
    /* ES 3.1 11.1.3.11: no vertex or fragment shader (a compute program, a
     * separable one of one stage) */
    if (p && p->image && (p->image->compute || p->image->stages != 0x11)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!p && ctx->pipe && !(p = ogles2_pipeline_program(ctx, FALSE))) { /* ES 3.1: a program pipeline's */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!p || !ctx->v3d || count == 0 || instances == 0) /* no program: undefined; nothing is drawn */
        return;
    if (!arrays_ok(ctx, p, (ULONG)first + (ULONG)count, instances, "glDrawArrays"))
        return;
    draw_arrays(ctx, p, mode, first, count, 0, instances, 0);
    ctx->n_draws++;
    ctx->sum_draw += (double)(ULONG)(ogles2_clock_us() - t0);
}

void ogles2_glDrawArrays(struct OGLES3Context *ctx, GLenum mode, GLint first, GLsizei count)
{
    draw_instances(ctx, mode, first, count, 1);
}

void ogles2_glDrawArraysInstanced(struct OGLES3Context *ctx, GLenum mode, GLint first, GLsizei count,
                                  GLsizei instancecount)
{
    if (ogles2_es3_entry(ctx))
        draw_instances(ctx, mode, first, count, instancecount);
}

void ogles2_glDrawArraysInstancedEXT(struct OGLES3Context *ctx, GLenum mode, GLint first, GLsizei count,
                                     GLsizei instancecount)
{
    draw_instances(ctx, mode, first, count, instancecount);
}

/* A gpu_draw's arrays (draw_arrays' `g`): every one the program reads from
 * a buffer, none from client memory (whose vertices the CPU must know).
 * A GPU-written one the GPU cannot read where it lies has its shadow made
 * current first (the CPU path's wait). FALSE: one is in client memory. */
static BOOL arrays_whole(struct OGLES3Context *ctx, const struct OGLES3Program *p)
{
    int a;
    for (a = 0; a < p->image->nattribs; a++) {
        struct OGLES3Array *arr = &ctx->array[p->attrib_index[a]];
        ULONG ga, gs, gv;
        int k;
        if (p->image->attribs[a].slot == 0xff)
            continue;
        for (k = 0; k < p->image->attribs[a].columns && p->attrib_index[a] + k < OGLES3_MAX_VERTEX_ATTRIBS; k++) {
            arr = &ctx->array[p->attrib_index[a] + k]; /* a matrix's columns, generic attributes of their own */
            if (!arr->enabled)
                continue;
            if (!arr->buffer)
                return FALSE;
            fmt_current(arr);
            if (ogles2_gpu_writes_seen && !gpu_array(ctx, arr, &ga, &gs, &gv))
                ogles2_buffer_cpu(ctx, arr->buffer);
        }
    }
    return TRUE;
}

/* glDrawElements with 32-bit indices the GPU wrote (a compute shader,
 * transform feedback) in the element array buffer: read where they lie,
 * nothing rebased, the base vertex the packet's index offset, every array
 * whole (gpu_draw); no wait for the render and no copy back
 * (docs/GLES3_PLAN.md, the remaining gaps, item 5). FALSE: not so (the CPU
 * path); TRUE: drawn, or nothing to draw (logged). The caller checked the
 * arguments, the framebuffer and the program. */
static BOOL gpu_elements(struct OGLES3Context *ctx, struct OGLES3Program *p, GLenum mode, GLsizei count,
                         const void *indices, GLint basevertex, GLsizei instances, BOOL restart, const char *fn)
{
    struct gpu_draw g;
    struct elements el;
    ULONG w, size, off = (ULONG)indices;
    BOOL fed;
    if ((off & 3) || !packets_ok(ctx->v3d) || !(w = ogles2_buffer_element_words(ctx, FALSE, &fed, &size)) ||
        !arrays_whole(ctx, p))
        return FALSE;
    if (off > size || (size - off) / 4 < (ULONG)count) {
        ogles2_log("%s: the indices lie outside the element buffer; nothing drawn", (ULONG)fn);
        return TRUE;
    }
    memset(&g, 0, sizeof(g));
    memset(&el, 0, sizeof(el));
    if (!gpu_logged[1]) {
        gpu_logged[1] = TRUE;
        ogles2_log("%s: 32-bit indices the GPU wrote read where they lie (logged once)", (ULONG)fn);
    }
    g.idx_addr = w + off;
    g.idx_bytes = size - off;
    g.ioffset = (ULONG)basevertex;
    g.fed = fed;
    el.type = GL_UNSIGNED_INT;
    el.count = count;
    el.restart = restart;
    draw_arrays(ctx, p, mode, 0, 1, &el, instances, &g);
    ctx->n_draws++;
    return TRUE;
}

/* glDrawElements, and with a base vertex added to every index
 * (OES_draw_elements_base_vertex): the copied range moves by basevertex,
 * the rebased indices stay as they are. With `range` (glDrawRangeElements)
 * the program has said which indices it uses, range[0] to range[1]: they
 * are not scanned. */
static void draw_elements(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLenum type,
                          const void *indices, GLint basevertex, const ULONG *range, GLsizei instances,
                          const char *fn)
{
    struct OGLES3Program *p;
    struct elements el;
    struct OGLES3Draw *d;
    struct index_range *ir;
    ULONG lo = ~0UL, hi = 0, k, t0 = ogles2_clock_us(), uid = 0, serial = 0;
    BOOL restart;

    if (!ogles2_entry_valid(ctx))
        return;
    mark(ctx, -1);
    restart = (ctx->state.caps & OGLES3_CAP_PRIMITIVE_RESTART) != 0;
    if (mode > GL_TRIANGLE_FAN ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ctx->xfb_begun && ogles2_xfb_drawing(ctx)) { /* ES 3.0 2.15.2: no indexed draw feeds back */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!ogles2_fb_ready(ctx))
        return;
    p = ctx->program;
    if (p && p->image && (p->image->compute || p->image->stages != 0x11)) { /* ES 3.1 11.1.3.11 */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!p && ctx->pipe && !(p = ogles2_pipeline_program(ctx, FALSE))) { /* ES 3.1: a program pipeline's */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!p || !ctx->v3d || count == 0)
        return;
    if (ogles2_gpu_writes_seen && type == GL_UNSIGNED_INT &&
        gpu_elements(ctx, p, mode, count, indices, basevertex, instances, restart, fn))
        return;
    {
        /* With an element buffer bound, `indices` is the offset into it. */
        BOOL bad;
        const void *in = ogles2_buffer_indices(ctx, (ULONG)indices,
                                               (ULONG)count * index_size(type), &bad);
        if (bad) {
            ogles2_log("%s: the indices lie outside the element buffer; nothing drawn", (ULONG)fn);
            return;
        }
        if (in) {
            indices = in;
            const struct OGLES3BufferHead *eb = OGLES3_BH(ogles2_buffer_element(ctx));
            serial = eb->serial;
            uid = eb->uid;
        } else if (!indices)
            return;
    }
    /* The same indices of the same element buffer data as the last draw
     * from a buffer: their range again, unscanned. */
    d = uid ? draw_state(ctx) : 0;
    ir = d ? &d->ir[IC_SLOT(uid, indices, count)] : 0;
    if (ir && !range && ir->uid == uid && ir->serial == serial && ir->ptr == indices && ir->count == count &&
        ir->type == type && ir->restart == restart) {
        lo = ir->lo;
        hi = ir->hi;
        goto range_known;
    }
scan:
    ctx->n_scans++;
    /* Mesa's state tracker: copy only the range the indices use. Without
     * branches (Emu68 runs a loop with two of them a pass at a third of the
     * speed): d's sign bit, spread by the arithmetic shift, picks it. Indices
     * past 2^31 would not fit the vertex arrays anyway. */
    if (range) {
        /* About 25 ns an index saved (Quake III through gl4es: 3.5 us a
         * draw). An index outside the range is undefined (GLES 3.0 2.9.5):
         * the GPU reads other vertices. */
        if (range[1] > 0x7FFFFFFFUL) {
            ogles2_log("%s: an end of 2^31 or more; nothing drawn", (ULONG)fn);
            return;
        }
        lo = range[0];
        hi = range[1];
    } else if (restart) {
        /* GL_PRIMITIVE_RESTART_FIXED_INDEX (ES 3.0 2.9.4): the type's
         * largest index restarts the primitive and is no vertex; a loop of
         * its own (rare), the fast ones below stay branchless. */
        ULONG r = type == GL_UNSIGNED_INT ? 0xFFFFFFFFUL : type == GL_UNSIGNED_SHORT ? 0xFFFFUL : 0xFFUL, x;
        lo = 0xFFFFFFFFUL;
        hi = 0;
        for (k = 0; k < (ULONG)count; k++) {
            x = type == GL_UNSIGNED_INT ? ((const ULONG *)indices)[k]
              : type == GL_UNSIGNED_SHORT ? ((const UWORD *)indices)[k] : ((const UBYTE *)indices)[k];
            if (x == r)
                continue;
            if (x & 0x80000000UL) {
                ogles2_log("%s: an index of 2^31 or more; nothing drawn", (ULONG)fn);
                return;
            }
            if (x < lo)
                lo = x;
            if (x > hi)
                hi = x;
        }
        if (lo > hi)
            return; /* restarts only: nothing to draw */
    } else if (type == GL_UNSIGNED_INT) {
        const ULONG *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        ULONG any = 0;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = (LONG)(in[k] & 0x7FFFFFFFUL);
            any |= in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        if (any & 0x80000000UL) {
            ogles2_log("%s: an index of 2^31 or more; nothing drawn", (ULONG)fn);
            return;
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    } else if (type == GL_UNSIGNED_SHORT) {
        scan_u16(indices, (ULONG)count, &lo, &hi);
    } else {
        const UBYTE *in = indices;
        LONG l = 0x7FFFFFFF, h = 0, d;
        for (k = 0; k < (ULONG)count; k++) {
            LONG x = in[k];
            d = x - l;
            l += d & (d >> 31);
            d = h - x;
            h -= d & (d >> 31);
        }
        lo = (ULONG)l;
        hi = (ULONG)h;
    }
    if (ir && !range) {
        ir->uid = uid;
        ir->serial = serial;
        ir->ptr = indices;
        ir->count = count;
        ir->type = type;
        ir->restart = restart;
        ir->lo = lo;
        ir->hi = hi;
    }
range_known:
    if ((LONG)lo + basevertex < 0) {
        ogles2_log("%s: index %ld + base vertex %ld is negative; nothing drawn", (ULONG)fn, (LONG)lo,
                   (LONG)basevertex);
        return;
    }
    lo += (ULONG)basevertex;
    hi += (ULONG)basevertex;
    if (!arrays_ok(ctx, p, hi + 1, instances, range ? 0 : fn)) { /* 0: quiet */
        if (!range)
            return;
        /* A range wider than the arrays (it need only hold the indices,
         * ES 3.2 10.4; dEQP's draw_range_elements_base_vertex): the
         * indices' own. */
        range = 0;
        goto scan;
    }
    el.indices = indices;
    el.type = type;
    el.count = count;
    el.min = lo - (ULONG)basevertex; /* the indices' own smallest */
    el.restart = restart;
    el.uid = uid;
    el.serial = serial;
    draw_arrays(ctx, p, mode, (GLint)lo, (GLsizei)(hi - lo + 1), &el, instances, 0);
    ctx->n_draws++;
    ctx->sum_draw += (double)(ULONG)(ogles2_clock_us() - t0);
}

void ogles2_glDrawElements(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLenum type,
                           const void *indices)
{
    draw_elements(ctx, mode, count, type, indices, 0, 0, 1, "glDrawElements");
}

static void elements_instanced(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLenum type,
                               const void *indices, GLsizei instancecount)
{
    if (instancecount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (instancecount)
        draw_elements(ctx, mode, count, type, indices, 0, 0, instancecount, "glDrawElementsInstanced");
}

void ogles2_glDrawElementsInstanced(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLenum type,
                                    const void *indices, GLsizei instancecount)
{
    if (ogles2_es3_entry(ctx))
        elements_instanced(ctx, mode, count, type, indices, instancecount);
}

void ogles2_glDrawElementsInstancedEXT(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLenum type,
                                       const void *indices, GLsizei instancecount)
{
    if (ogles2_context_valid(ctx))
        elements_instanced(ctx, mode, count, type, indices, instancecount);
}

void ogles2_glDrawElementsBaseVertexOES(struct OGLES3Context *ctx, GLenum mode, GLsizei count,
                                        GLenum type, const void *indices, GLint basevertex)
{
    draw_elements(ctx, mode, count, type, indices, basevertex, 0, 1, "glDrawElementsBaseVertexOES");
}

/* OES_ and EXT_draw_elements_base_vertex's other entries (the EXT names
 * the same commands): the range is of the indices, before basevertex is
 * added (as glDrawRangeElements'). */
void ogles2_glDrawRangeElementsBaseVertexOES(struct OGLES3Context *ctx, GLenum mode, GLuint start, GLuint end,
                                             GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
    ULONG range[2];
    if (!ogles2_context_valid(ctx))
        return;
    if (end < start) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    range[0] = start;
    range[1] = end;
    draw_elements(ctx, mode, count, type, indices, basevertex, range, 1, "glDrawRangeElementsBaseVertex");
}

void ogles2_glDrawElementsInstancedBaseVertexOES(struct OGLES3Context *ctx, GLenum mode, GLsizei count,
                                                 GLenum type, const void *indices, GLsizei instancecount,
                                                 GLint basevertex)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (instancecount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (instancecount)
        draw_elements(ctx, mode, count, type, indices, basevertex, 0, instancecount,
                      "glDrawElementsInstancedBaseVertex");
}

void ogles2_glDrawElementsBaseVertexEXT(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLenum type,
                                        const void *indices, GLint basevertex)
{
    draw_elements(ctx, mode, count, type, indices, basevertex, 0, 1, "glDrawElementsBaseVertexEXT");
}

void ogles2_glDrawRangeElementsBaseVertexEXT(struct OGLES3Context *ctx, GLenum mode, GLuint start, GLuint end,
                                             GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
    ogles2_glDrawRangeElementsBaseVertexOES(ctx, mode, start, end, count, type, indices, basevertex);
}

void ogles2_glDrawElementsInstancedBaseVertexEXT(struct OGLES3Context *ctx, GLenum mode, GLsizei count,
                                                 GLenum type, const void *indices, GLsizei instancecount,
                                                 GLint basevertex)
{
    ogles2_glDrawElementsInstancedBaseVertexOES(ctx, mode, count, type, indices, instancecount, basevertex);
}

/* With EXT_multi_draw_arrays (offered): one draw after another. */
void ogles2_glMultiDrawElementsBaseVertexEXT(struct OGLES3Context *ctx, GLenum mode, const GLsizei *count,
                                             GLenum type, const void *const *indices, GLsizei drawcount,
                                             const GLint *basevertex)
{
    GLsizei k;
    if (!ogles2_context_valid(ctx))
        return;
    if (drawcount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < drawcount; k++)
        draw_elements(ctx, mode, count[k], type, indices[k], basevertex[k], 0, 1,
                      "glMultiDrawElementsBaseVertexEXT");
}

/* GLES 3.0's glDrawRangeElements, offered to GLES2 programs through
 * aglGetProcAddress (ports/gl4es passes a locked array range): the indices
 * lie in [start, end], so ogles2 does not scan them for their range. */
void ogles2_glDrawRangeElements(struct OGLES3Context *ctx, GLenum mode, GLuint start, GLuint end,
                                GLsizei count, GLenum type, const void *indices)
{
    ULONG range[2];
    if (!ogles2_context_valid(ctx))
        return;
    if (end < start) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    range[0] = start;
    range[1] = end;
    draw_elements(ctx, mode, count, type, indices, 0, range, 1, "glDrawRangeElements");
}

/* ---- ES 3.1's indirect draws (docs/GLES3_PLAN.md stage D item 3): the
 * command read on the CPU, as glDispatchComputeIndirect's (the draws copy
 * and convert their attributes and indices: the counts are needed here),
 * then the direct draw it describes. ---- */

/* ES 3.1 10.5's checks after the mode's, in Mesa's order: a vertex array
 * object other than 0, no enabled array in client memory, no transform
 * feedback drawing; then the command (buffer.c). FALSE: the error set. */
static BOOL indirect_command(struct OGLES3Context *ctx, const void *indirect, ULONG bytes, ULONG *cmd)
{
    GLenum e;
    int k;
    if (!ogles2_vao_bound(ctx) || ogles2_xfb_drawing(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    for (k = 0; k < OGLES3_MAX_VERTEX_ATTRIBS; k++)
        if (ctx->array[k].enabled && !ctx->array[k].buffer) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return FALSE;
        }
    if ((e = ogles2_buffer_draw_indirect(ctx, (GLintptr)indirect, bytes, cmd)) != 0) {
        ogles2_error(ctx, e);
        return FALSE;
    }
    return TRUE;
}

/* An indirect draw whose command the GPU wrote (a compute shader, transform
 * feedback): read by the GPU's indirect packets, every array whole
 * (gpu_draw); no wait for the render, no copy back (docs/GLES3_PLAN.md, the
 * remaining gaps, item 5). `type` 0: glDrawArraysIndirect. The indices: 32-
 * bit ones where they lie (the GPU's words, or the shadow's in the GPU's
 * order), 8- and 16-bit ones the whole buffer copied (the CPU path's
 * conversion). FALSE: not so, the CPU reads the command; TRUE: done (drawn,
 * or the error set). */
static BOOL gpu_indirect(struct OGLES3Context *ctx, GLenum mode, GLenum type, const void *indirect, ULONG bytes)
{
    struct OGLES3Program *p;
    struct gpu_draw g;
    struct elements el;
    ULONG addr;
    BOOL fed;
    GLenum e;
    int k;
    if (!ogles2_vao_bound(ctx) || ogles2_xfb_drawing(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return TRUE;
    }
    for (k = 0; k < OGLES3_MAX_VERTEX_ATTRIBS; k++)
        if (ctx->array[k].enabled && !ctx->array[k].buffer) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return TRUE;
        }
    if ((e = ogles2_buffer_draw_indirect_gpu(ctx, (GLintptr)indirect, bytes, &addr, &fed)) != 0) {
        ogles2_error(ctx, e);
        return TRUE;
    }
    if (!addr || !ctx->v3d || !packets_ok(ctx->v3d))
        return FALSE;
    /* draw_instances' checks */
    if (!ogles2_fb_ready(ctx))
        return TRUE;
    p = ctx->program;
    if (p && p->image && (p->image->compute || p->image->stages != 0x11)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return TRUE;
    }
    if (!p && ctx->pipe && !(p = ogles2_pipeline_program(ctx, FALSE))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return TRUE;
    }
    if (!p)
        return TRUE;
    memset(&g, 0, sizeof(g));
    memset(&el, 0, sizeof(el));
    g.ind_addr = addr;
    g.fed = fed;
    if (type) {
        struct OGLES3Buffer *eb = ogles2_buffer_element(ctx);
        ULONG size = ogles2_buffer_size(eb), w;
        BOOL efed = FALSE, bad;
        el.type = type;
        el.restart = (ctx->state.caps & OGLES3_CAP_PRIMITIVE_RESTART) != 0;
        if (type == GL_UNSIGNED_INT) {
            if (!(w = ogles2_buffer_element_words(ctx, TRUE, &efed, &size)))
                return FALSE;
            g.idx_addr = w;
            g.idx_bytes = size;
            g.fed |= efed;
        } else {
            /* the whole buffer converted, as the CPU path converts a draw's
             * indices (made current first if the GPU wrote it) */
            if (!size || !(el.indices = ogles2_buffer_indices(ctx, 0, size, &bad)) || bad)
                return TRUE; /* nothing to draw from */
            el.count = (GLsizei)(size / index_size(type));
            el.uid = OGLES3_BH(eb)->uid;
            el.serial = OGLES3_BH(eb)->serial;
        }
    }
    if (!arrays_whole(ctx, p))
        return FALSE;
    if (!gpu_logged[2]) {
        gpu_logged[2] = TRUE;
        ogles2_log("indirect draw: a command the GPU wrote read by the GPU (logged once)");
    }
    draw_arrays(ctx, p, mode, 0, 1, type ? &el : 0, 1, &g);
    ctx->n_draws++;
    return TRUE;
}

/* DrawArraysIndirectCommand: count, instance count, first, a reserved word
 * (base instance, 0 in ES). Counts past GLsizei's range or a reserved word
 * not 0: undefined, nothing drawn (logged). */
void ogles2_glDrawArraysIndirect(struct OGLES3Context *ctx, GLenum mode, const void *indirect)
{
    ULONG cmd[4];
    if (!ogles2_es31_entry(ctx))
        return;
    if (mode > GL_TRIANGLE_FAN) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ogles2_gpu_writes_seen && gpu_indirect(ctx, mode, 0, indirect, sizeof(cmd)))
        return;
    if (!indirect_command(ctx, indirect, sizeof(cmd), cmd))
        return;
    if ((cmd[0] | cmd[1] | cmd[2]) > 0x7fffffffUL || cmd[3]) {
        ogles2_log("glDrawArraysIndirect: a command with count %lu, %lu instances, first %lu, reserved %lu: "
                   "nothing drawn", cmd[0], cmd[1], cmd[2], cmd[3]);
        return;
    }
    if (cmd[0] && cmd[1])
        draw_instances(ctx, mode, (GLint)cmd[2], (GLsizei)cmd[0], (GLsizei)cmd[1]);
}

/* DrawElementsIndirectCommand: count, instance count, first index, base
 * vertex, a reserved word; the indices from the element array buffer,
 * which must be bound. */
void ogles2_glDrawElementsIndirect(struct OGLES3Context *ctx, GLenum mode, GLenum type, const void *indirect)
{
    ULONG cmd[5], size;
    if (!ogles2_es31_entry(ctx))
        return;
    if (mode > GL_TRIANGLE_FAN ||
        (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!ogles2_buffer_element(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (ogles2_gpu_writes_seen && gpu_indirect(ctx, mode, type, indirect, sizeof(cmd)))
        return;
    if (!indirect_command(ctx, indirect, sizeof(cmd), cmd))
        return;
    size = index_size(type);
    if ((cmd[0] | cmd[1]) > 0x7fffffffUL || cmd[2] > 0xffffffffUL / size || cmd[4]) {
        ogles2_log("glDrawElementsIndirect: a command with count %lu, %lu instances, first index %lu, "
                   "reserved %lu: nothing drawn", cmd[0], cmd[1], cmd[2], cmd[4]);
        return;
    }
    if (cmd[0] && cmd[1])
        draw_elements(ctx, mode, (GLsizei)cmd[0], type, (const void *)(cmd[2] * size), (GLint)cmd[3], 0,
                      (GLsizei)cmd[1], "glDrawElementsIndirect");
}

#include "clear_mrt_v3dp.h"

/* The program a clear draws into several render targets with: the clear
 * colour (the last generic attribute, constant) in every output, position
 * from the last but one; tools/clear_mrt. 0: none (logged once), and the
 * built-in one clears the first target only. */
static struct OGLES3Program *mrt_program(struct OGLES3Draw *d)
{
    struct OGLES3Program *p = &d->mrt_quad;
    UWORD a;
    if (!d->mrt_ready && !d->mrt_failed) {
        char why[96];
        if (!(d->mrt_image = ogles2_v3dp_load(clear_mrt_v3dp, sizeof(clear_mrt_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->mrt_image)) {
            d->mrt_failed = TRUE;
            ogles2_log("glClear: no program for several render targets (%s): the first is cleared only",
                       (ULONG)(d->mrt_image ? "out of memory" : why));
            return 0;
        }
        d->mrt_ready = TRUE;
    }
    if (!d->mrt_ready)
        return 0;
    for (a = 0; a < d->mrt_image->nattribs; a++)
        p->attrib_index[a] = d->mrt_image->attribs[a].name[2] == 'p' /* a_position */
                                 ? OGLES3_MAX_VERTEX_ATTRIBS - 2 : OGLES3_MAX_VERTEX_ATTRIBS - 1;
    return p;
}

#include "clear_zs_v3dp.h"

/* The program draw_clear clears depth and stencil with where a target is
 * 32-bit float or integer: position from the last but one generic
 * attribute, no colour output; tools/clear_zs. 0: none (logged once). */
static struct OGLES3Program *zs_program(struct OGLES3Draw *d)
{
    struct OGLES3Program *p = &d->zs_quad;
    if (!d->zs_ready && !d->zs_failed) {
        char why[96];
        if (!(d->zs_image = ogles2_v3dp_load(clear_zs_v3dp, sizeof(clear_zs_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->zs_image)) {
            d->zs_failed = TRUE;
            ogles2_log("glClear: no program for depth and stencil beside a 32-bit target (%s): not cleared",
                       (ULONG)(d->zs_image ? "out of memory" : why));
            return 0;
        }
        d->zs_ready = TRUE;
    }
    if (!d->zs_ready)
        return 0;
    p->attrib_index[0] = OGLES3_MAX_VERTEX_ATTRIBS - 2;
    return p;
}

#include "copy_tex_v3dp.h"

/* The program glCopyTex* draws with on the GPU: unit 0's 2D texture over a
 * quad, position from the last but one generic attribute, texture
 * coordinates from the last; tools/copy_tex. 0: none (logged once), and the
 * copies go through the CPU. */
static struct OGLES3Program *copy_program(struct OGLES3Draw *d)
{
    struct OGLES3Program *p = &d->copy_prog;
    UWORD a;
    if (!d->copy_ready && !d->copy_failed) {
        char why[96];
        if (!(d->copy_image = ogles2_v3dp_load(copy_tex_v3dp, sizeof(copy_tex_v3dp), why, sizeof(why))) ||
            !ogles2_program_image(p, d->copy_image)) {
            d->copy_failed = TRUE;
            ogles2_log("glCopyTexSubImage2D: no program for copies on the GPU (%s): the CPU copies",
                       (ULONG)(d->copy_image ? "out of memory" : why));
            return 0;
        }
        d->copy_ready = TRUE;
    }
    if (!d->copy_ready)
        return 0;
    for (a = 0; a < d->copy_image->nattribs; a++)
        p->attrib_index[a] = d->copy_image->attribs[a].name[2] == 'p' /* a_position */
                                 ? OGLES3_MAX_VERTEX_ATTRIBS - 2 : OGLES3_MAX_VERTEX_ATTRIBS - 1;
    return p;
}

/* glCopyTexImage2D and glCopyTexSubImage2D on the GPU (copy.c): texture
 * src, bound to unit 0 for the draw, over the window rectangle (x, y, w, h)
 * of the bound framebuffer (the copy's target), texture coordinates tc[0]
 * to tc[2] left to right and tc[1] to tc[3] bottom to top. None of the
 * application's state applies: no test, blending, dithering or mask, as a
 * copy writes. The application's state is restored afterwards. FALSE:
 * nothing drawn (no program). */
BOOL ogles2_draw_copy(struct OGLES3Context *ctx, struct OGLES3Texture *src, const GLfloat *tc, GLint x, GLint y,
                      GLsizei w, GLsizei h)
{
    struct OGLES3State saved = ctx->state, *s = &ctx->state;
    struct OGLES3Array saved_pos = ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2];
    struct OGLES3Array saved_tc = ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 1];
    struct OGLES3Program *saved_prog = ctx->program, *p;
    struct OGLES3Texture *saved_tex;
    struct OGLES3Draw *d;
    static const GLfloat pos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLfloat uv[8];
    GLint saved_vp[4];
    int k;

    if (!(d = draw_state(ctx)) || !(p = copy_program(d)))
        return FALSE;
    for (k = 0; k < 4; k++)
        saved_vp[k] = ctx->viewport[k];
    uv[0] = tc[0]; uv[1] = tc[1]; uv[2] = tc[2]; uv[3] = tc[1];
    uv[4] = tc[0]; uv[5] = tc[3]; uv[6] = tc[2]; uv[7] = tc[3];
    for (k = 0; k < 2; k++) {
        struct OGLES3Array *a = &ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2 + k];
        a->enabled = GL_TRUE;
        a->size = 2;
        a->type = GL_FLOAT;
        a->normalized = GL_FALSE;
        a->stride = 0;
        a->zero_stride = GL_FALSE;
        a->integer = GL_FALSE;
        a->pointer = k ? (const void *)uv : (const void *)pos;
        a->buffer = 0;
        a->divisor = 0; /* the application's, set aside with the rest */
    }
    s->caps = 0;
    s->polygon_mode = GL_FILL;
    s->color_mask[0] = s->color_mask[1] = s->color_mask[2] = s->color_mask[3] = GL_TRUE;
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->clip_origin = GL_LOWER_LEFT_EXT;
    s->clip_depth = GL_NEGATIVE_ONE_TO_ONE_EXT;
    ctx->viewport[0] = x;
    ctx->viewport[1] = y;
    ctx->viewport[2] = w;
    ctx->viewport[3] = h;
    saved_tex = ogles2_texture_unit0(ctx, src);

    ogles2_job_begin(ctx);
    OGLES3_STATE_CHANGED(ctx, OGLES3_SG_ALL); /* the override above */
    mark(ctx, -1);
    ogles2_query_pause(ctx, TRUE); /* a copy counts no samples */
    draw_arrays(ctx, p, GL_TRIANGLE_STRIP, 0, 4, 0, 1, 0);
    ogles2_query_pause(ctx, FALSE);

    ogles2_texture_unit0(ctx, saved_tex);
    ctx->state = saved;
    OGLES3_STATE_CHANGED(ctx, OGLES3_SG_ALL);
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 1] = saved_tc;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
    return TRUE;
}

/* ---- clears that the tile buffer cannot do ----
 * A clear confined by the scissor box or by write masks, or one after draws
 * in the same job (the tile buffer clears before any of them), is drawn as a
 * rectangle (plan 7.7; PiStorm3D's gl_ClearViaQuad, Mesa's blitter clear):
 * the built-in program, an identity matrix, the clear colour as a constant
 * attribute, z at the clear depth, and a state that writes exactly what
 * glClear writes. The application's state is restored afterwards. */
void ogles2_draw_clear(struct OGLES3Context *ctx, GLbitfield mask, UBYTE rts, const GLfloat *color, GLfloat depth,
                       GLint stencil)
{
    static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    struct OGLES3State saved = ctx->state, *s = &ctx->state;
    struct OGLES3Array saved_pos = ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2];
    struct OGLES3Program *saved_prog = ctx->program, *quad;
    struct OGLES3Draw *d;
    GLint saved_vp[4];
    GLfloat pos[12], z;
    int k;

    if ((mask & GL_DEPTH_BUFFER_BIT) && (!ctx->fb_depth_bits || !s->depth_mask))
        mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    if ((mask & GL_STENCIL_BUFFER_BIT) && !ctx->fb_stencil_bits)
        mask &= ~(GLbitfield)GL_STENCIL_BUFFER_BIT;
    if (!mask)
        return;
    for (k = 0; k < 4; k++)
        saved_vp[k] = ctx->viewport[k];

    /* The program: the built-in image, an identity matrix, position from
     * the last-but-one generic attribute, colour from the last (constant). */
    if (!(d = draw_state(ctx)))
        return;
    if (!d->quad_ready) {
        if (!ogles2_program_builtin(&d->quad))
            return;
        d->quad_ready = TRUE;
        for (k = 0; k < 16; k++)
            ogles2_cbuf_set(&d->quad, 0, (ULONG)k, float_bits(identity[k]));
    }
    quad = &d->quad;
    quad->attrib_index[0] = OGLES3_MAX_VERTEX_ATTRIBS - 2;
    quad->attrib_index[1] = OGLES3_MAX_VERTEX_ATTRIBS - 1;
    /* EXT_draw_buffers: GLES2 clears every draw buffer. */
    if (ctx->fb_nrt > 1 && (mask & GL_COLOR_BUFFER_BIT)) {
        struct OGLES3Program *m = mrt_program(d);
        if (m)
            quad = m;
    }
    /* A 32-bit float or integer target: no 16-bit colour write may reach
     * it (frame.c clears their colour on the CPU): depth and stencil with
     * the program that writes no colour. */
    if (ctx->fb_soft_blend && !(mask & GL_COLOR_BUFFER_BIT)) {
        if (!(quad = zs_program(d)))
            return;
    }

    z = depth * 2.0f - 1.0f; /* lands on the depth with the range [0, 1] */
    pos[0] = -1; pos[1] = -1;  pos[3] = 1; pos[4] = -1;
    pos[6] = -1; pos[7] = 1;   pos[9] = 1; pos[10] = 1;
    pos[2] = pos[5] = pos[8] = pos[11] = z;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].enabled = GL_TRUE;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].size = 3;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].type = GL_FLOAT;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].normalized = GL_FALSE;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].stride = 0;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].zero_stride = GL_FALSE;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].integer = GL_FALSE;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].pointer = pos;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].buffer = 0;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2].divisor = 0;
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 1].enabled = GL_FALSE;
    for (k = 0; k < 4; k++)
        s->attrib[OGLES3_MAX_VERTEX_ATTRIBS - 1][k] = color[k];

    /* The state: only the scissor test stays the application's (the
     * rectangle is filled whatever glPolygonMode says). */
    s->caps &= OGLES3_CAP_SCISSOR_TEST;
    s->polygon_mode = GL_FILL;
    if (!(mask & GL_COLOR_BUFFER_BIT))
        s->color_mask[0] = s->color_mask[1] = s->color_mask[2] = s->color_mask[3] = GL_FALSE;
    if (mask & GL_DEPTH_BUFFER_BIT) {
        s->caps |= OGLES3_CAP_DEPTH_TEST;
        s->depth_func = GL_ALWAYS;
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
        s->caps |= OGLES3_CAP_STENCIL_TEST;
        for (k = 0; k < 2; k++) {
            s->stencil_func[k] = GL_ALWAYS;
            s->stencil_ref[k] = stencil & 0xff;
            s->stencil_fail[k] = s->stencil_zfail[k] = s->stencil_zpass[k] = GL_REPLACE;
            s->stencil_write_mask[k] = saved.stencil_write_mask[0]; /* glClear uses the front mask */
        }
    }
    s->depth_range[0] = 0.0f;
    s->depth_range[1] = 1.0f;
    s->clip_origin = GL_LOWER_LEFT_EXT;
    s->clip_depth = GL_NEGATIVE_ONE_TO_ONE_EXT;
    ctx->viewport[0] = ctx->viewport[1] = 0;
    ctx->viewport[2] = (GLint)ctx->fb_w;
    ctx->viewport[3] = (GLint)ctx->fb_h;

    ogles2_job_begin(ctx);
    OGLES3_STATE_CHANGED(ctx, OGLES3_SG_ALL); /* the override above */
    mark(ctx, -1); /* probe P: the time before is not the draw's */
    if (ctx->oq_addr) /* its test, without the call */
        ogles2_query_pause(ctx, TRUE); /* nor a clear */
    ctx->draw_rts = rts; /* glClearBuffer*: one draw buffer */
    draw_arrays(ctx, quad, GL_TRIANGLE_STRIP, 0, 4, 0, 1, 0);
    /* The format the draw worked out stays with the array (its key says
     * which): the next clear's needs no array_fmt again
     * (tests/perf/drawbench; Quake III clears mid-frame). */
    fmt_keep(&saved_pos, &ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2]);
    ctx->draw_rts = 0xFF;
    ogles2_query_pause(ctx, FALSE);

    ctx->state = saved;
    OGLES3_STATE_CHANGED(ctx, OGLES3_SG_ALL);
    ctx->array[OGLES3_MAX_VERTEX_ATTRIBS - 2] = saved_pos;
    ctx->program = saved_prog;
    for (k = 0; k < 4; k++)
        ctx->viewport[k] = saved_vp[k];
}
