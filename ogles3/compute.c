/*
 * Compute shaders (ES 3.1 chapter 17; docs/GLES3_PLAN.md stage D item 1):
 * glDispatchCompute and glDispatchComputeIndirect, glMemoryBarrier.
 *
 * A dispatch runs on V3D's compute shader dispatcher (v3dcore's
 * v3d_csd_dispatch), outside the render jobs, and is waited for: the open
 * job is rendered first (its draws come before the dispatch), and what the
 * shader writes is in memory when the call returns, so every barrier is
 * already kept. The configuration words are Mesa's v3d_launch_grid's (one
 * work group a supergroup); the uniform stream is written as draw.c's are,
 * into GPU memory of the context's own, which the next dispatch reuses.
 * Storage buffers are their words in GPU memory (buffer.c,
 * ogles2_buffer_storage), copied back into their shadows when the CPU or a
 * draw next uses them. Textures, images and atomic counters are read
 * through the uniform stream's records (below; docs/GLES3_PLAN.md stage D
 * items 1-2).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles3_internal.h"
#include "v3d_context.h"
#include "v3d_csd.h"
#include "v3d_gpu_lock.h"
#include "v3d_hw.h"

/* The context's dispatch memory: [constant buffer copy][uniform stream] in
 * `stream`, the zeroed uniform block, the spill area and the shared
 * memory, each grown when a dispatch needs more. */
struct OGLES3Compute
{
    v3d_mem stream, zero, spill, shared;
    ULONG stream_size, zero_size, spill_size, shared_size;
};

#define GPU_LOCK_TIMEOUT_MS 5000
#define ZERO_UBO_BYTES      65536 /* GL_MAX_UNIFORM_BLOCK_SIZE (draw.c's) */
#define MAX_WORK_GROUPS     65535 /* GL_MAX_COMPUTE_WORK_GROUP_COUNT (state.c) */
#define SHARED_BYTES_MAX    (64UL << 20) /* a dispatch's shared variables at most, all groups */

/* Mesa's common/v3d_csd.h fields (V3D 4.2): CFG3 is the group size in bits
 * 7:0, work groups a supergroup in 11:8 (0: 16), batches a supergroup - 1
 * in 19:12. (These two were swapped here at first: every dispatch said 16
 * groups a supergroup of two batches each against CFG4's total, and the
 * A1200 froze minutes later, again after warm resets until switched off.) */
#define CFG012_WG_COUNT_SHIFT     16
#define CFG3_WGS_PER_SG_SHIFT     8
#define CFG3_BATCHES_PER_SG_M1_SHIFT 12
#define CFG3_WG_SIZE_SHIFT        0
#define CFG5_PROPAGATE_NANS       (1UL << 2)
#define CFG5_SINGLE_SEG           (1UL << 1)
#define CFG5_THREADING            (1UL << 0)

void ogles2_compute_free(struct OGLES3Context *ctx)
{
    struct OGLES3Compute *c = ctx->compute;
    if (!c)
        return;
    if (c->stream.hostptr)
        v3d_mem_free(ogles2_device(), &c->stream);
    if (c->zero.hostptr)
        v3d_mem_free(ogles2_device(), &c->zero);
    if (c->spill.hostptr)
        v3d_mem_free(ogles2_device(), &c->spill);
    if (c->shared.hostptr)
        v3d_mem_free(ogles2_device(), &c->shared);
    FreeVec(c);
    ctx->compute = 0;
}

/* At least `need` bytes in *m (its size *have); nothing waits on it (every
 * dispatch is waited for). */
static BOOL grow(v3d_mem *m, ULONG *have, ULONG need)
{
    if (need <= *have)
        return TRUE;
    if (m->hostptr)
        v3d_mem_free(ogles2_device(), m);
    memset(m, 0, sizeof(*m));
    *have = 0;
    if (v3d_mem_alloc(ogles2_device(), m, need) < 0) {
        memset(m, 0, sizeof(*m));
        return FALSE;
    }
    *have = need;
    return TRUE;
}

static ULONG le32(ULONG x)
{
    return (x >> 24) | ((x >> 8) & 0xff00UL) | ((x << 8) & 0xff0000UL) | (x << 24);
}

static const struct OGLES3Variant *compute_variant(const struct OGLES3Image *im)
{
    UWORD i;
    for (i = 0; i < im->nvariants; i++)
        if (im->variants[i].stage == OGLES3_VARIANT_COMPUTE)
            return &im->variants[i];
    return 0;
}

/* The buffer uniform block k of the shader (Mesa's UBO k + 1) reads: the
 * range at its binding, else the zeroed block; *size its bytes. */
static ULONG block_address(struct OGLES3Context *ctx, const struct OGLES3Program *p, ULONG k, ULONG zero,
                           ULONG *size)
{
    const struct OGLES3Image *im = p->image;
    ULONG a, n = 0;
    UWORD b;
    *size = 0;
    if (k >= im->stage_nblocks[0])
        return zero;
    b = im->stage_block[0][k];
    if (p->block_binding && (a = ogles2_buffer_uniform(ctx, p->block_binding[b], &n)) != 0) {
        *size = n;
        return a;
    }
    *size = im->ublocks[b].size;
    return zero;
}

/* Storage buffer k of the shader (Mesa's SSBO k): a storage block's range
 * at its binding, or past them an atomic counter buffer's (FORMAT.md's
 * 0x8000 | binding); 0 without a buffer (or memory). */
static ULONG storage_address(struct OGLES3Context *ctx, const struct OGLES3Program *p, ULONG k, ULONG *size)
{
    const struct OGLES3Image *im = p->image;
    UWORD e;
    *size = 0;
    if (k >= im->stage_nsblocks[0])
        return 0;
    e = im->stage_sblock[0][k];
    if (e & 0x8000)
        return ogles2_buffer_atomic(ctx, e & 0x7fff, size);
    return ogles2_buffer_storage(ctx, im->sblocks[e].binding, size);
}

/* The work groups whose shared variables can be in memory at once: the
 * shader finds its own at the work group's number in memory (the CSD's
 * payload) times its size, a number the compiler masks to 16 bits less
 * the local invocation index's (nir_to_vir.c's compute setup); at most
 * every group of the dispatch (Mesa allocates that many). */
static ULONG shared_groups(const struct OGLES3Variant *v, const ULONG *groups)
{
    ULONG wg = (ULONG)v->local_size[0] * v->local_size[1] * v->local_size[2], cap, n;
    int bits = 0;
    while (bits < 16 && (1UL << bits) < wg)
        bits++;
    cap = 1UL << (16 - bits);
    n = groups[0] < cap ? groups[0] : cap; /* below 2^16 each: no product overflows */
    n = n * groups[1] < cap ? n * groups[1] : cap;
    return n * groups[2] < cap ? n * groups[2] : cap;
}

/* The image unit of the compute shader's image k (ES 3.1: its uniform's
 * layout(binding)), -1 for none. */
static int image_unit(const struct OGLES3Program *p, ULONG k)
{
    const struct OGLES3Image *im = p->image;
    UWORD u;
    for (u = 0; u < im->nuniforms; u++) {
        const struct OGLES3ImageUniform *un = &im->uniforms[u];
        ULONG ne = un->array ? un->array : 1;
        if ((un->flags & OGLES3_UNIFORM_IMAGE) && k >= un->sampler[0] && k - un->sampler[0] < ne)
            return p->uniform_loc[u] == OGLES3_LOC_NONE ? 0 : p->sampler_unit[p->uniform_loc[u] + k - un->sampler[0]];
    }
    return -1;
}

/* The texture unit and sampler kind of the compute shader's texture k (its
 * sampler uniform's glUniform1i or layout(binding)); FALSE for none. */
static BOOL sampler_unit(const struct OGLES3Program *p, ULONG k, int *unit, int *kind)
{
    const struct OGLES3Image *im = p->image;
    UWORD u;
    for (u = 0; u < im->nuniforms; u++) {
        const struct OGLES3ImageUniform *un = &im->uniforms[u];
        ULONG ne = un->array ? un->array : 1;
        if ((un->flags & OGLES3_UNIFORM_SAMPLER) && k >= un->sampler[0] && k - un->sampler[0] < ne) {
            *unit = p->uniform_loc[u] == OGLES3_LOC_NONE ? 0 : p->sampler_unit[p->uniform_loc[u] + k - un->sampler[0]];
            *kind = ogles2_sampler_kind(un->type);
            return TRUE;
        }
    }
    return FALSE;
}

/* A sampler of the program reads a texture the TMU returns 32 bits for:
 * the dispatch takes the .v3dw's variant (every sampler 32 bits). */
static BOOL wants_wide(struct OGLES3Context *ctx, const struct OGLES3Program *p)
{
    ULONG k;
    int unit, kind;
    if (!ogles2_wide_seen)
        return FALSE;
    for (k = 0; k < OGLES3_MAX_STAGE_SAMPLERS; k++)
        if (sampler_unit(p, k, &unit, &kind) && ogles2_texture_wide(ctx, unit, kind))
            return TRUE;
    return FALSE;
}

/* The dispatch's memory and uniform stream (Mesa's v3d_write_uniforms for
 * the compute stage); the stream's address, 0 when something is missing
 * (logged). */
static ULONG write_stream(struct OGLES3Context *ctx, struct OGLES3Compute *c, const struct OGLES3Program *p,
                          const struct OGLES3Variant *v, const ULONG *groups)
{
    const struct OGLES3Image *im = p->image;
    ULONG cwords = im->cbuf_words[0], cbytes = (cwords * 4 + 15) & ~15UL, zero = 0, *u, n;
    ULONG ibytes = v->no_textures ? 0 : (OGLES3_MAX_STAGE_IMAGES + 2 * OGLES3_MAX_STAGE_SAMPLERS) * 32UL + 32;
    ULONG img[OGLES3_MAX_STAGE_IMAGES], tex[OGLES3_MAX_STAGE_SAMPLERS][2];
    UBYTE *tsr, *trec;
    UWORD i;
    if (!grow(&c->stream, &c->stream_size, cbytes + ibytes + (v->nrecipe + 1) * 4UL + 64))
        return 0;
    /* The images' texture states (ES 3.1), each 32 bytes at a 32-byte
     * boundary after the stream's constant buffer, made at their first
     * entry. */
    tsr = (UBYTE *)V3D_ALIGN_UP((v3d_uintptr)c->stream.hostptr + cbytes, 32);
    trec = tsr + OGLES3_MAX_STAGE_IMAGES * 32; /* the textures' state and sampler records, copied */
    memset(img, 0, sizeof(img));
    memset(tex, 0, sizeof(tex));
    if (v->uses_ubo)
        memcpy(c->stream.hostptr, p->cbuf_le[0], cwords * 4UL);
    u = (ULONG *)((UBYTE *)c->stream.hostptr + cbytes + ibytes);
    if (v->uses_blocks) {
        if (!c->zero_size) {
            if (!grow(&c->zero, &c->zero_size, ZERO_UBO_BYTES + 256))
                return 0;
            memset(c->zero.hostptr, 0, ZERO_UBO_BYTES + 256);
        }
        zero = (ULONG)V3D_ALIGN_UP((v3d_uintptr)c->zero.hostptr, 256);
    }
    if (v->spill_size &&
        !grow(&c->spill, &c->spill_size,
              (ULONG)ogles2_device()->deviceInfo.qpu_count * 4 * v->spill_size + 256))
        return 0;
    if (v->shared_size) {
        ULONG need = shared_groups(v, groups);
        if (need > SHARED_BYTES_MAX / v->shared_size ||
            !grow(&c->shared, &c->shared_size, need * v->shared_size + 256)) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            ogles2_log("glDispatchCompute: no memory for %ld work groups' shared variables", (LONG)need);
            return 0;
        }
    }
    for (i = 0; i < v->nrecipe; i++) {
        ULONG d = v->rdata[i], w;
        switch (v->rtype[i]) {
        case OGLES3_U_UNIFORM:        w = p->cbuf[0][d]; break;
        case OGLES3_U_CONSTANT:       w = d; break;
        case OGLES3_U_UBO_ADDR:
            w = ((d >> 24) ? block_address(ctx, p, (d >> 24) - 1, zero, &n) : (ULONG)c->stream.hostptr) +
                (d & 0xffffff);
            break;
        case OGLES3_U_GET_UBO_SIZE:
            w = cwords * 4;
            if (d)
                block_address(ctx, p, d - 1, zero, &w);
            break;
        case OGLES3_U_SSBO_OFFSET:
            /* Not dispatched without the buffer: the shader would write
             * wherever address 0 is. */
            if (!(w = storage_address(ctx, p, d, &n))) {
                ogles2_log("glDispatchCompute: storage block %ld has no buffer (or no memory)", (LONG)d);
                return 0;
            }
            break;
        case OGLES3_U_GET_SSBO_SIZE:  storage_address(ctx, p, d, &w); break;
        case OGLES3_U_NUM_WORK_GROUPS: w = groups[d < 3 ? d : 0]; break;
        case OGLES3_U_SHARED_OFFSET:  w = (ULONG)V3D_ALIGN_UP((v3d_uintptr)c->shared.hostptr, 256); break;
        case OGLES3_U_SPILL_OFFSET:   w = (ULONG)V3D_ALIGN_UP((v3d_uintptr)c->spill.hostptr, 256); break;
        case OGLES3_U_SPILL_SIZE_PER_THREAD: w = v->spill_size; break;
        case OGLES3_U_TMU_CONFIG_P0: /* Mesa's write_tmu_p0/p1: the records with the compiler's bits */
        case OGLES3_U_TMU_CONFIG_P1: {
            ULONG k = (d >> 24) & (OGLES3_MAX_STAGE_SAMPLERS - 1), ts, ss;
            int unit = 0, kind = OGLES3_TEX_NONE;
            if (!tex[k][0]) {
                sampler_unit(p, k, &unit, &kind);
                if (!ogles2_texture_state(ctx, unit, kind, &ts, &ss))
                    return 0;
                /* into the stream's memory: no job's state buffer to outlive */
                memcpy(trec + k * 64, (const void *)ts, 32);
                memcpy(trec + k * 64 + 32, (const void *)ss, 32);
                tex[k][0] = (ULONG)(trec + k * 64);
                tex[k][1] = (ULONG)(trec + k * 64 + 32);
            }
            w = tex[k][v->rtype[i] == OGLES3_U_TMU_CONFIG_P1] | (d & 0xffffff);
            break;
        }
        case OGLES3_U_TEXTURE_WIDTH:
        case OGLES3_U_TEXTURE_HEIGHT:
        case OGLES3_U_TEXTURE_DEPTH:
        case OGLES3_U_TEXTURE_ARRAY_SIZE: {
            int unit = 0, kind = OGLES3_TEX_NONE;
            sampler_unit(p, d & (OGLES3_MAX_STAGE_SAMPLERS - 1), &unit, &kind);
            w = ogles2_texture_size(ctx, unit, kind, (int)(v->rtype[i] - OGLES3_U_TEXTURE_WIDTH));
            break;
        }
        case OGLES3_U_IMAGE_TMU_CONFIG_P0: /* Mesa's write_image_tmu_p0 */
            if (!img[d >> 24] && !(img[d >> 24] = ogles2_image_state(ctx, image_unit(p, d >> 24), tsr + (d >> 24) * 32)))
                return 0;
            w = img[d >> 24] | (d & 0xffffff);
            break;
        case OGLES3_U_IMAGE_WIDTH:
        case OGLES3_U_IMAGE_HEIGHT:
        case OGLES3_U_IMAGE_DEPTH:
        case OGLES3_U_IMAGE_ARRAY_SIZE:
            w = ogles2_image_size(ctx, image_unit(p, d), (int)(v->rtype[i] - OGLES3_U_IMAGE_WIDTH));
            break;
        default:
            ogles2_log("glDispatchCompute: uniform type %ld not implemented", (LONG)v->rtype[i]);
            return 0;
        }
        u[i] = le32(w);
    }
    u[v->nrecipe] = 0; /* the hardware prefetches the next uniform */
    return (ULONG)u;
}

/* The program a dispatch runs: the one in use, else the bound pipeline's
 * compute program (ES 3.1); NULL or another: none. */
static struct OGLES3Program *compute_program(struct OGLES3Context *ctx)
{
    struct OGLES3Program *p = ctx->program;
    if (!p && ctx->pipe)
        p = ogles2_pipeline_program(ctx, TRUE);
    return p && p->image && p->image->compute ? p : 0;
}

static void dispatch(struct OGLES3Context *ctx, const ULONG *groups)
{
    struct OGLES3Program *p = compute_program(ctx);
    const struct OGLES3Variant *v;
    const struct OGLES3Image *own;
    struct OGLES3Compute *c;
    struct SignalSemaphore *gpu = ogles2_device_gpu_lock();
    ULONG cfg[7], wg_size, batches, stream;
    int k, r;

    if (!p || !p->image || !p->image->compute) { /* ES 3.1 17: no program for the compute stage */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!groups[0] || !groups[1] || !groups[2] || !ctx->v3d)
        return;
    /* A float or depth texture bound to a sampler: the 32-bit-sampler
     * variant of the program's .v3dw (the compiler writes one for a
     * program with samplers). */
    own = p->image;
    if (wants_wide(ctx, p)) {
        const struct OGLES3Image *w = ogles2_image_wide_file((struct OGLES3Image *)p->image);
        if (w && compute_variant(w))
            own = w;
        else
            ogles2_log("glDispatchCompute: a sampler reads 32 bits and the program has no .v3dw: 16 bits read");
    }
    if (!(v = compute_variant(own)) || !own->code_mem) {
        ogles2_log("glDispatchCompute: the program has no compute code in GPU memory");
        return;
    }
    if (!ctx->compute && !(ctx->compute = AllocVec(sizeof(struct OGLES3Compute), MEMF_ANY | MEMF_CLEAR))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    c = ctx->compute;
    /* What was drawn before is rendered first: the shader may read it. */
    if (ogles2_job_has_draws(ctx))
        ogles2_job_flush(ctx, FALSE);
    ogles2_job_finish(ctx);
    if (!(stream = write_stream(ctx, c, p, v, groups))) {
        ogles2_log("glDispatchCompute: nothing dispatched");
        return;
    }

    wg_size = (ULONG)v->local_size[0] * v->local_size[1] * v->local_size[2];
    /* Batches of 16 invocations, one work group a supergroup (Mesa's choice
     * when supergroups cannot be used): CFG4's total must fit 32 bits. */
    batches = (wg_size + 15) / 16 * groups[0]; /* below 2^20 */
    if (!wg_size || wg_size > 256 || groups[1] > 0xffffffffUL / batches ||
        groups[2] > 0xffffffffUL / (batches * groups[1])) {
        ogles2_log("glDispatchCompute: %ld x %ld x %ld work groups of %ld: too many batches, not dispatched",
                   (LONG)groups[0], (LONG)groups[1], (LONG)groups[2], (LONG)wg_size);
        return;
    }
    batches *= groups[1] * groups[2];
    for (k = 0; k < 3; k++)
        cfg[k] = groups[k] << CFG012_WG_COUNT_SHIFT;
    cfg[3] = (1UL << CFG3_WGS_PER_SG_SHIFT) | (((wg_size + 15) / 16 - 1) << CFG3_BATCHES_PER_SG_M1_SHIFT) |
             ((wg_size & 0xff) << CFG3_WG_SIZE_SHIFT);
    cfg[4] = batches - 1;
    cfg[5] = ((ULONG)own->code_mem->hostptr + v->code_offset) | CFG5_PROPAGATE_NANS |
             (v->single_seg ? CFG5_SINGLE_SEG : 0) | (v->threads == 4 ? CFG5_THREADING : 0);
    cfg[6] = stream;

    CacheClearE(0, ~0UL, CACRF_ClearD); /* the stream, the buffers' words: in memory */
    if (!v3d_gpu_lock(gpu, GPU_LOCK_TIMEOUT_MS)) {
        ogles2_log("glDispatchCompute: the GPU stayed busy (another program holds the submission lock)");
        return;
    }
    ogles2_job_take_gpu(ctx); /* a render left running finishes first */
    r = v3d_csd_dispatch(cfg, batches);
    if (r < 0)
        ogles2_device_reset("a compute dispatch it did not finish");
    v3d_gpu_unlock(gpu);
    if (r < 0)
        ogles2_log("glDispatchCompute: %ld x %ld x %ld work groups of %ld did not finish", (LONG)groups[0],
                   (LONG)groups[1], (LONG)groups[2], (LONG)wg_size);
    /* What it stored into images is newer than their RAM copies (ES 3.1). */
    for (k = 0; v->uses_images && k < OGLES3_MAX_STAGE_IMAGES; k++)
        if (image_unit(p, k) >= 0)
            ogles2_image_written(ctx, image_unit(p, k), 0);
}

void ogles2_glDispatchCompute(struct OGLES3Context *ctx, GLuint num_groups_x, GLuint num_groups_y,
                              GLuint num_groups_z)
{
    ULONG g[3];
    if (!ogles2_es31_entry(ctx))
        return;
    if (num_groups_x > MAX_WORK_GROUPS || num_groups_y > MAX_WORK_GROUPS || num_groups_z > MAX_WORK_GROUPS) {
        /* after the program's check (ES 3.1 17: both errors; dEQP checks the first) */
        if (!compute_program(ctx))
            ogles2_error(ctx, GL_INVALID_OPERATION);
        else
            ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    g[0] = num_groups_x;
    g[1] = num_groups_y;
    g[2] = num_groups_z;
    dispatch(ctx, g);
}

void ogles2_glDispatchComputeIndirect(struct OGLES3Context *ctx, GLintptr indirect)
{
    ULONG g[3];
    GLenum e;
    if (!ogles2_es31_entry(ctx))
        return;
    if (!compute_program(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if ((e = ogles2_buffer_dispatch_indirect(ctx, indirect, g)) != 0) {
        ogles2_error(ctx, e);
        return;
    }
    /* Counts past the limits: undefined (ES 3.1 17); nothing is dispatched. */
    if (g[0] > MAX_WORK_GROUPS || g[1] > MAX_WORK_GROUPS || g[2] > MAX_WORK_GROUPS) {
        ogles2_log("glDispatchComputeIndirect: %ld x %ld x %ld work groups: past the limit, not dispatched",
                   (LONG)g[0], (LONG)g[1], (LONG)g[2]);
        return;
    }
    dispatch(ctx, g);
}

/* Every dispatch is waited for and its writes are in memory before the
 * call returns (above), and a draw's are before the CPU or a dispatch reads
 * them: the barriers are kept already; only their bits are checked. */
#define ALL_BARRIERS                                                                                        \
    (GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT |           \
     GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_COMMAND_BARRIER_BIT |           \
     GL_PIXEL_BUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT |           \
     GL_FRAMEBUFFER_BARRIER_BIT | GL_TRANSFORM_FEEDBACK_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT |       \
     GL_SHADER_STORAGE_BARRIER_BIT)
#define REGION_BARRIERS                                                                                     \
    (GL_ATOMIC_COUNTER_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |     \
     GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT)

void ogles2_glMemoryBarrier(struct OGLES3Context *ctx, GLbitfield barriers)
{
    if (!ogles2_es31_entry(ctx))
        return;
    if (barriers != GL_ALL_BARRIER_BITS && (barriers & ~(GLbitfield)ALL_BARRIERS))
        ogles2_error(ctx, GL_INVALID_VALUE);
}

void ogles2_glMemoryBarrierByRegion(struct OGLES3Context *ctx, GLbitfield barriers)
{
    if (!ogles2_es31_entry(ctx))
        return;
    if (barriers != GL_ALL_BARRIER_BITS && (barriers & ~(GLbitfield)REGION_BARRIERS))
        ogles2_error(ctx, GL_INVALID_VALUE);
}
