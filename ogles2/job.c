/*
 * Jobs (plan section 7.7), version 1: one job per context, for the default
 * framebuffer. A job is one binning pass plus one render pass. Ported from
 * PiStorm3D's gl_FrameBegin / gl_FramePresent (gl/src/context.c), with these
 * differences:
 *
 *  - The GPU submission lock (section 7.1, phase 1) is held from the binning
 *    submit until the render has finished, and the flush waits for the render
 *    before returning (PiStorm3D defers that wait to the next frame).
 *  - Lock order: GPU lock, then the bitmap lock, so a program waiting for its
 *    turn on the GPU never holds a bitmap lock. While the bitmap is locked
 *    nothing is logged (cybergraphics: no library calls under a bitmap lock;
 *    the exec cache and semaphore calls are unavoidable, as in PiStorm3D).
 *  - Depth and stencil of the default framebuffer are cleared by the hardware
 *    at the start of every tile and neither loaded nor stored (section 7.2:
 *    EGL leaves them undefined after a swap). This replaces PiStorm3D's
 *    depth_persist_seen heuristic and its one possibly corrupted frame.
 *  - No intermediate passes yet: nothing draws in the first M1 step.
 *  - M6 overlap: a flush may return once the render is submitted, so the
 *    CPU builds the next job while the GPU renders (PiStorm3D's deferred
 *    wait): an object's job, and a job on a fullscreen screen of our own,
 *    which shows what the GPU writes. ogles2_job_finish waits for it: at the
 *    next flush, or before the CPU reads what it writes or changes what it
 *    reads (texture.c, buffer.c, fbo.c, frame.c). The submission lock goes
 *    once the render is submitted (plan 7.1 phase 2 within ogles2): the
 *    next submitter, any context of any program, takes the lock and waits
 *    for that render on the GPU first (take_gpu), so programs take turns
 *    and none holds the lock across its CPU work. minigl.library does not
 *    take part (D3).
 *  - A job renders into the default framebuffer or into a framebuffer
 *    object (M5, plan 7.7; fbo.c): ctx->job_fb. Drawing into another
 *    framebuffer renders the open job first (jobs v1). An object's job loads
 *    and stores its colour image in the image's own layout and its depth
 *    and stencil every time, without the Y flip, with tile pools of its own
 *    when it has more tiles than the default framebuffer.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/execbase.h>
#include <proto/exec.h>
#include "ogles2_internal.h"
#include "v3d_context.h"
#include "v3d_commands.h"
#include "v3d_clbuf.h"
#include "v3d_hw.h"
#include "v3d_submit_timeout.h"
#include "v3d_gpu_lock.h"
#include "v3d_regs.h"
#include "v3d_csd.h"

#define GPU_LOCK_TIMEOUT_MS 5000

extern v3d_u32 g_v3d_cl_generation; /* v3d_frame.c: bumped whenever a CL slot is reset */

static void supertiles(V3DContext *v, ULONG w, ULONG h, ULONG sw, ULONG sh, ULONG tw, ULONG th);

/* ---- the binning list grows (plan section 9, M4) ----
 * Its block is fixed (V3D_INITIAL_CL_BUFFER_SIZE, 4 MB), and a frame with
 * enough draws outgrows it. Instead of dropping the frame, a claim that
 * does not fit ends the block with a BRANCH to a new one, as Mesa's
 * v3d_cl_ensure_space does; the list is then submitted from the first
 * block to the end of the last. BRANCH_ROOM bytes past `capacity` are kept
 * free in every block for that packet. The new blocks are freed when the
 * frame's render has finished (v3d_frame_defer_free). */
#define BRANCH_ROOM 8                         /* BRANCH: opcode + 32-bit address */
#define CHAIN_BLOCK (1024UL * 1024)           /* the least for a further block */

static void *grow_binning(v3d_static_buffer *buf, v3d_uintptr size)
{
    struct OGLES3Context *ctx = buf->grow_data;
    V3DContext *v = ctx->v3d;
    V3DDevice *dev = ogles2_device();
    v3d_mem m;
    UBYTE *b;
    /* Each block twice the last: few blocks even for huge frames (the
     * frame's retire list holds V3D_RETIRE_MAX). */
    ULONG addr, len, want = (ULONG)buf->capacity * 2;
    if (want < CHAIN_BLOCK)
        want = CHAIN_BLOCK;
    if (want < (ULONG)size + BRANCH_ROOM + 256)
        want = (ULONG)size + BRANCH_ROOM + 256;
    if (v3d_mem_alloc(dev, &m, want) < 0)
        return 0; /* out of memory: the frame is dropped as before */
    /* BRANCH, little-endian, in the room kept free after `used`. */
    b = buf->start + buf->used;
    addr = (ULONG)m.hostptr;
    b[0] = v3d_OP_BRANCH;
    b[1] = (UBYTE)addr;
    b[2] = (UBYTE)(addr >> 8);
    b[3] = (UBYTE)(addr >> 16);
    b[4] = (UBYTE)(addr >> 24);
    /* The old block is finished: make it visible to the GPU now
     * (v3d_context_flush_for_dma flushes only the current block). */
    len = (ULONG)buf->used + 5;
    CachePreDMA(buf->start, &len, 0);
    if (ctx->chained_blocks++ < 8)
        ogles2_log("job: binning list block full at 0x%08lx + %ld: BRANCH to a new block at 0x%08lx (%ld bytes)",
                   (ULONG)buf->start, (LONG)buf->used, addr, (LONG)m.size);
    v3d_frame_defer_free(&v->frame, &m); /* freed after this frame's render */
    buf->start = (v3d_u8 *)m.hostptr;
    buf->used = 0;
    /* 256 bytes of slack for the control list reader's prefetch. */
    buf->capacity = (int)m.size - BRANCH_ROOM - 256;
    return v3d_buffer_claim_memory(buf, size);
}

/* ---- depth and stencil across a mid-frame flush (plan 7.2) ----
 * A job ends before its frame when the program reads pixels, copies them,
 * or calls glFlush/glFinish. Such a job stores depth and stencil (D24S8 as
 * one buffer) to memory of its own, and the next job of the frame loads
 * them instead of clearing early, as Mesa does for any surface outliving a
 * job. Only the GPU reads this memory, so its layout is the GPU's: UIF
 * without XOR, padded as Mesa pads a UIF level (v3d_setup_slices). At
 * aglSwapBuffers they are not kept (EGL: undefined after a swap). */
struct zs_save
{
    v3d_mem mem;
    ULONG base;            /* 4096-aligned within mem */
    ULONG w, h;            /* the framebuffer it was made for */
    BOOL msaa;             /* all four samples: twice the width and height (Mesa's msaa_scale) */
    ULONG height_in_ub;    /* padded height in UIF blocks */
    UBYTE buffer, format;  /* v3d_ZSTENCIL/v3d_Z, V3D_OUTPUT_IMAGE_FORMAT_* */
};

static struct zs_save *zs_get(struct OGLES3Context *ctx)
{
    struct zs_save *z = ctx->zs_save;
    V3DContext *v = ctx->v3d;
    ULONG cpp, ub_w, ub_h, pw, ph;
    if (!ctx->depth_bits && !ctx->stencil_bits)
        return 0;
    if (z && z->w == v->width && z->h == v->height && z->msaa == ctx->msaa)
        return z;
    if (z) {
        v3d_mem_free(ogles2_device(), &z->mem);
        FreeVec(z);
        ctx->zs_save = 0;
    }
    if (!(z = AllocVec(sizeof(*z), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    cpp = ctx->depth_format == OGLES3_DEPTH_16 ? 2 : 4;
    ub_w = 2 * v3d_utile_width((int)cpp);
    ub_h = 2 * v3d_utile_height((int)cpp);
    pw = ((ULONG)v->width * (ctx->msaa ? 2 : 1) + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ph = ((ULONG)v->height * (ctx->msaa ? 2 : 1) + ub_h - 1) / ub_h * ub_h;
    if (v3d_mem_alloc(ogles2_device(), &z->mem, pw * ph * cpp + 4096) < 0) {
        FreeVec(z);
        return 0;
    }
    z->base = ((ULONG)z->mem.hostptr + 4095) & ~4095UL;
    z->w = v->width;
    z->h = v->height;
    z->msaa = ctx->msaa;
    z->height_in_ub = ph / ub_h;
    z->buffer = ctx->depth_format == OGLES3_DEPTH_24 ? v3d_ZSTENCIL : v3d_Z;
    z->format = ctx->depth_format == OGLES3_DEPTH_16 ? V3D_OUTPUT_IMAGE_FORMAT_D16
              : ctx->depth_format == OGLES3_DEPTH_24 ? V3D_OUTPUT_IMAGE_FORMAT_D24S8
                                                     : V3D_OUTPUT_IMAGE_FORMAT_D32F;
    ctx->zs_save = z;
    return z;
}

/* glBlitFramebuffer (fbo.c): the default framebuffer's depth and stencil
 * as a texture view of their saved image, its size the framebuffer's;
 * FALSE: none saved (nothing drawn or cleared since the frame began), or
 * all four samples of 4x MSAA. */
BOOL ogles2_job_zs_view(struct OGLES3Context *ctx, V3DTexture *t)
{
    struct zs_save *z = ctx->zs_save;
    ULONG cpp, ub_w, ub_h, pw, ph;
    if (!ctx->zs_saved || !z || z->msaa)
        return FALSE;
    cpp = z->format == V3D_OUTPUT_IMAGE_FORMAT_D16 ? 2 : 4;
    ub_w = 2 * v3d_utile_width((int)cpp);
    ub_h = 2 * v3d_utile_height((int)cpp);
    pw = (z->w + 4 * ub_w - 1) / (4 * ub_w) * (4 * ub_w);
    ph = (z->h + ub_h - 1) / ub_h * ub_h;
    memset(t, 0, sizeof(*t));
    t->texture_mem.hostptr = (void *)z->base;
    t->width = (v3d_u16)z->w;
    t->height = (v3d_u16)z->h;
    t->cpp = (v3d_u8)cpp;
    t->num_levels = 1;
    t->levels[0].stride = pw * cpp;
    t->levels[0].padded_w = (v3d_u16)pw;
    t->levels[0].padded_h = (v3d_u16)ph;
    t->levels[0].tiling_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
    t->tiling_format = V3D_MEMORY_FORMAT_UIF_NO_XOR;
    return TRUE;
}

void ogles2_job_free(struct OGLES3Context *ctx)
{
    struct zs_save *z = ctx->zs_save;
    if (z) {
        v3d_mem_free(ogles2_device(), &z->mem);
        FreeVec(z);
        ctx->zs_save = 0;
    }
    ctx->zs_saved = FALSE;
}

/* Starts the job's binning list if none is open. */
void ogles2_job_begin(struct OGLES3Context *ctx)
{
    V3DContext *v = ctx->v3d;
    struct OGLES3Target tg;
    UWORD w = v->width, h = v->height;
    UBYTE nrt = 1, bpp = V3D_INTERNAL_BPP_32, tw, th;
    BOOL bad = FALSE;
    v3d_u8 s;

    if (v->frame_active) {
        if (ctx->job_fb == ctx->draw_fb) {
            /* Drawing after an invalidation (fbo.c discard): what is drawn
             * is kept. */
            ctx->job_discard = 0;
            return;
        }
        /* Drawing into another framebuffer: the open job is rendered first
         * (jobs v1). The default framebuffer's depth and stencil are kept
         * for its next job, as at any mid-frame flush. */
        ogles2_job_flush(ctx, FALSE);
    }
    if (ctx->draw_fb) {
        if (ogles2_fbo_target(ctx, ctx->draw_fb, FALSE, &tg)) {
            w = tg.w;
            h = tg.h;
            nrt = tg.nrt;
            bpp = tg.bpp;
        } else {
            bad = TRUE; /* no memory for its tile pools: the job is dropped at the flush */
        }
    }
    ctx->job_fb = ctx->draw_fb;
    /* 4x MSAA on the default framebuffer (plan 7.2): 32x32 tiles, Mesa's
     * v3d_choose_tile_size for one 32-bpp target; the hardware derives the
     * same from the configuration packets. An object's with multisampled
     * renderbuffers (GLES3 stage C item 2): its tile size ogles2_fbo_target's. */
    ctx->job_msaa = ctx->draw_fb ? !bad && tg.msaa : ctx->msaa;
    ctx->t_job_begin = ogles2_clock_us();
    ctx->job_discard = 0;
    ctx->job_noload = ctx->draw_fb ? ogles2_fbo_take_discarded(ctx->draw_fb) : 0;
    /* An object's job with several render targets (EXT_draw_buffers) or a
     * 64-bit one (EXT_color_buffer_half_float) has smaller tiles as well:
     * ogles2_fbo_target's. */
    if (ctx->draw_fb && !bad) {
        tw = tg.tile_w;
        th = tg.tile_h;
    } else {
        tw = ctx->job_msaa ? 32 : 64;
        th = ctx->job_msaa ? 32 : 64;
    }
    v->frame.tilesX = (w + tw - 1) / tw;
    v->frame.tilesY = (h + th - 1) / th;
    s = v->build_slot;
    v3d_cl_reset(&v->tile_list_mem[s], &v->tile_list_buf[s]);
    v3d_cl_reset(&v->binning_mem[s], &v->binning_buf[s]);
    /* Back to the first block (a chained frame left buf->start in its last). */
    v->binning_buf[s].start = (v3d_u8 *)v->binning_mem[s].hostptr;
    v->binning_buf[s].capacity -= BRANCH_ROOM;
    v->binning_buf[s].grow = grow_binning;
    v->binning_buf[s].grow_data = ctx;
    v3d_cl_reset(&v->render_mem[s], &v->render_buf[s]);
    v3d_cl_reset(&v->state_mem[s], &v->state_buf[s]);
    g_v3d_cl_generation++;

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    /* As Mesa's v3d_start_binning (v3dx_draw.c). */
    NumberOfLayers(v, 1);
    TileBinningModeCfg(v, v3d_TILE_ALLOCATION_INITIAL_BLOCK_SIZE_64B,
                       v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, nrt, bpp, ctx->job_msaa, FALSE, w, h);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);
    OcclusionQueryCounter(v, 0);
    DoCommand(v, v3d_OP_START_TILE_BINNING);
    /* An occlusion query goes on across jobs (query.c): its counter after
     * the prefix, as Mesa's state emission puts it (in the prefix it
     * counts nothing). */
    if (ctx->oq_addr)
        OcclusionQueryCounter(v, ctx->oq_addr);

    v->frame_active = TRUE;
    /* Unique across contexts: a shared object's "last used by job N" names
     * one job of one member (share.c). */
    ObtainSemaphore(&OGLES3LibBase->lock);
    if (!++OGLES3LibBase->serial)
        ++OGLES3LibBase->serial; /* 0: never used */
    ctx->job_serial = OGLES3LibBase->serial;
    ReleaseSemaphore(&OGLES3LibBase->lock);
    v->ez_state = V3D_EZ_UNDECIDED;
    v->first_ez_state = V3D_EZ_UNDECIDED;
    v->pending_clear_color = FALSE;
    v->pending_clear_depth = FALSE;
    ctx->job_clear_targets = 0;
    v->frame_corrupted = bad;
    v->draw_state_configured = FALSE;
}

static v3d_u8 to_byte(GLfloat f)
{
    return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (v3d_u8)(f * 255.0f + 0.5f); /* ES 3.0 keeps it unclamped */
}

static ULONG fbits(GLfloat f)
{
    union { GLfloat f; ULONG u; } x;
    x.f = f;
    return x.u;
}

static GLfloat bitsf(ULONG u)
{
    union { GLfloat f; ULONG u; } x;
    x.u = u;
    return x.f;
}

/* The whole-buffer path of glClear and glClearBuffer*: the tile buffer is
 * cleared when the job renders. The values are those current at the clear
 * (Mesa snapshots them the same way), not at the flush: the colour of the
 * targets `rts` (glClear: every one) from cv (floats' bits, or with is_int
 * integers), depth and stencil. */
void ogles2_job_clear(struct OGLES3Context *ctx, GLbitfield mask, UBYTE rts, const ULONG *cv, BOOL is_int,
                      GLfloat depth, GLint stencil)
{
    V3DContext *v = ctx->v3d;
    int k;

    ogles2_job_begin(ctx);
    if (mask & GL_COLOR_BUFFER_BIT) {
        GLfloat c[4];
        for (k = 0; k < 4; k++)
            c[k] = is_int ? (GLfloat)(LONG)cv[k] : bitsf(cv[k]);
        v->pending_clear_color = TRUE;
        if (rts & 1) /* the default framebuffer's */
            v->clear_color_value = ((v3d_u32)to_byte(c[3]) << 24) | ((v3d_u32)to_byte(c[0]) << 16) |
                                   ((v3d_u32)to_byte(c[1]) << 8) | to_byte(c[2]); /* ARGB */
        for (k = 0; k < 4; k++) /* a framebuffer object's targets' (build_render_fbo) */
            if (rts & (1 << k)) {
                int j;
                for (j = 0; j < 4; j++)
                    ctx->job_clear_rt[k].v[j] = cv[j];
                ctx->job_clear_rt[k].is_int = (UBYTE)is_int;
                ctx->job_clear_targets |= (UBYTE)(1 << k);
            }
    }
    /* The tile buffer clears depth and stencil together; each keeps the
     * value of the last glClear that named it (a stencil clear after a full
     * one must not take the newer glClearDepthf, dEQP depth_stencil_clear).
     * Before the job's first such clear neither holds anything yet. */
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        if (!v->pending_clear_depth) {
            v->clear_depth_value = 1.0f;
            ctx->job_clear_stencil = 0;
        }
        v->pending_clear_depth = TRUE;
        if (mask & GL_DEPTH_BUFFER_BIT)
            v->clear_depth_value = depth;
        if (mask & GL_STENCIL_BUFFER_BIT)
            ctx->job_clear_stencil = (UBYTE)stencil; /* the low 8 bits (GLES 2.0 4.2.3) */
    }
}

/* Builds the tile list and the render list. The display must be locked. */
static void build_render(struct OGLES3Context *ctx, struct zs_save *store_zs)
{
    V3DContext *v = ctx->v3d;
    struct OGLES3Display *d = &ctx->display;
    v3d_u8 s = v->build_slot;
    UWORD width = v->width, height = v->height;
    ULONG tilesX = v->frame.tilesX, tilesY = v->frame.tilesY;
    ULONG start, end, sw = 1, sh = 1, fw, fh;
    UBYTE clear_color = v->pending_clear_color;
    UBYTE ez = v->first_ez_state;
    UBYTE ez_off = (ez == V3D_EZ_DISABLED) || (width & 1) || (height & 1); /* GFXH-1918 */
    UBYTE depth_type = ctx->depth_format == OGLES3_DEPTH_16 ? V3D_INTERNAL_TYPE_DEPTH16
                     : ctx->depth_format == OGLES3_DEPTH_24 ? V3D_INTERNAL_TYPE_DEPTH24
                                                            : V3D_INTERNAL_TYPE_DEPTH32F;
    ULONG argb, abgr;
    UBYTE flip_y = (ctx->probe & OGLES3_PROBE_FLIP_Y) ? TRUE : FALSE;
    /* 4x MSAA: the colour stored resolved (the four samples averaged), the
     * colour loaded into every sample; depth kept across a flush with all
     * four (Mesa v3dx_rcl.c's decimate modes). */
    UBYTE msaa = ctx->msaa;
    UBYTE zs_decimate = msaa ? V3D_DECIMATE_MODE_ALL_SAMPLES : V3D_DECIMATE_MODE_SAMPLE_0;
    /* Depth and stencil from the frame's previous job, unless this one
     * clears them anyway. */
    struct zs_save *load_zs = ctx->zs_saved && !v->pending_clear_depth ? zs_get(ctx) : 0;
    /* Mesa v3dx_rcl.c: the early Z/S clear only without a Z/S load or store;
     * otherwise the clear packets clear Z/S as well. */
    UBYTE early_zs_clear = !(load_zs || store_zs);
    if (store_zs && !ctx->zs_logged) {
        ctx->zs_logged = TRUE;
        ogles2_log("job: depth and stencil kept across a mid-frame flush (logged once)");
    }

    /* Finish binning. Probe S: the binner signals the render thread, which
     * waits for it (VC4's protocol; packets 7 and 8 exist on V3D 4.2 too). */
    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    ogles2_xfb_job_end(ctx); /* after transform feedback: its counts, the specs off */
    if (ctx->probe & OGLES3_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_INCREMENT_SEMAPHORE);
    DoCommand(v, v3d_OP_FLUSH);

    /* Generic tile list: load colour unless it is cleared, store it to the
     * render bitmap. Depth: early clear, no load, no store (see the top). */
    v->current_buf = &v->tile_list_buf[s];
    SetBuffer(v, &v->tile_list_buf[s]);
    start = CurrentBufferAddress(v);
    DoCommand(v, v3d_OP_TILE_COORDINATES_IMPLICIT);
    /* Probe Y: Flip Y on both, with the height it needs (Mesa's XML: "used
     * for y flip"). */
    if (!clear_color)
        LoadTileBufferGeneral(v, v3d_RENDER_TARGET_0, V3D_MEMORY_FORMAT_RASTER, flip_y,
                              V3D_DECIMATE_MODE_SAMPLE_0, V3D_OUTPUT_IMAGE_FORMAT_RGBA8, FALSE, FALSE,
                              d->rb_swap, d->bprow, flip_y ? height : 0, d->base);
    if (load_zs)
        LoadTileBufferGeneral(v, load_zs->buffer, V3D_MEMORY_FORMAT_UIF_NO_XOR, FALSE,
                              zs_decimate, load_zs->format, FALSE, FALSE, FALSE,
                              load_zs->height_in_ub, 0, load_zs->base);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    PrimListFormat(v, v3d_LIST_TRIANGLES, FALSE);
    SetInstanceid(v, 0);
    BranchToImplicitTileList(v, 0);
    /* No dithering into the RGBA8 framebuffer, whatever GL_DITHER says (GLES2
     * leaves dithering to the implementation): the tile buffer holds 8 bits
     * per channel, so there is nothing finer to dither from, and Mesa never
     * sets the store's dither mode. PiStorm3D dithered when GL_DITHER was on;
     * on the A1200 that store turned 0xff into 0x00 and 0xbf into 0xc0 (M1
     * run 2). Probe D (ENV:OGLES3/Probe) brings it back, for that probe only.
     * Dithering matters for 16-bit targets (plan 7.7), which come later. */
    StoreTileBufferGeneral(v, v3d_RENDER_TARGET_0, V3D_MEMORY_FORMAT_RASTER, flip_y,
                           (ctx->probe & OGLES3_PROBE_STORE_DITHER) ? V3D_DITHER_MODE_RGBA : V3D_DITHER_MODE_NONE,
                           msaa ? V3D_DECIMATE_MODE_4X : V3D_DECIMATE_MODE_SAMPLE_0,
                           V3D_OUTPUT_IMAGE_FORMAT_RGBA8, FALSE, FALSE,
                           d->rb_swap, d->bprow, flip_y ? height : 0, d->base);
    if (store_zs)
        StoreTileBufferGeneral(v, store_zs->buffer, V3D_MEMORY_FORMAT_UIF_NO_XOR, FALSE,
                               V3D_DITHER_MODE_NONE, zs_decimate, store_zs->format,
                               FALSE, FALSE, FALSE, store_zs->height_in_ub, 0, store_zs->base);
    /* Mesa v3dx_rcl.c, GFXH-1461/GFXH-1689: on 4.2 the per-store clear bit is
     * broken for Z/S, so clear with this packet, RTs bit set; Z/S bit clear
     * because the early Z/S clear does that. */
    if (clear_color || v->pending_clear_depth)
        ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_RETURN_FROM_SUB_LIST);
    end = CurrentBufferAddress(v);

    /* Render list. */
    v->current_buf = &v->render_buf[s];
    SetBuffer(v, &v->render_buf[s]);
    if (ctx->probe & OGLES3_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_WAIT_ON_SEMAPHORE);
    TileRenderingModeCFGCommon(v, 1, width, height, v3d_RENDER_TARGET_MAXIMUM_32BPP, msaa, FALSE,
                               ez == V3D_EZ_GT_GE ? v3d_EARLY_Z_DIRECTION_GT_GE
                                                  : v3d_EARLY_Z_DIRECTION_LT_LE,
                               ez_off, depth_type,
                               /* Mesa: the early Z/S clear only without a Z/S load or
                                * store (with it, the stored depth was the clear value,
                                * A1200 run ogles2-m4-1); otherwise the dummy tile and
                                * the end-of-tile clear below clear the tile buffer */
                               early_zs_clear);
    argb = v->clear_color_value;
    abgr = (argb & 0xFF00FF00UL) | ((argb & 0x00FF0000UL) >> 16) | ((argb & 0x000000FFUL) << 16);
    TileRenderingModeCFGClearColorsPart1(v, 0, abgr);
    if (ctx->probe & OGLES3_PROBE_LOG_CLEAR)
        ogles2_log("job: frame %lu, clear %s, ARGB %08lx, packet ABGR %08lx", ctx->frames,
                   clear_color ? "colour" : "no colour", argb, abgr);
    TileRenderingModeCFGColor(v, 1, V3D_INTERNAL_BPP_32, V3D_INTERNAL_TYPE_8, 0);
    TileRenderingModeCFGZSClearValues(v, v->pending_clear_depth ? ctx->job_clear_stencil : 0,
                                      v->pending_clear_depth ? v->clear_depth_value : 1.0f);
    TileListInitialBlockSize(v, v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, TRUE);

    /* Supertiles: fewer than 256 in the frame (Mesa v3dx_rcl.c). */
    for (;;) {
        fw = (tilesX + sw - 1) / sw;
        fh = (tilesY + sh - 1) / sh;
        if (fw * fh < 256)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }
    Multicore_Rendering_Tile_List_Set_Base(v, (ULONG)v->tile_alloc_mem.hostptr, 0);
    MulticoreRenderingSupertileCFG(v, sw, sh, fw, fh, 1, FALSE, FALSE, tilesY, tilesX);

    /* Mesa's two dummy tiles before the real ones (v3dx_rcl.c). */
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);

    StartAddressOfGenericTileList(v, start, end);
    supertiles(v, width, height, sw, sh, msaa ? 32 : 64, msaa ? 32 : 64);
    DoCommand(v, v3d_OP_END_OF_RENDERING);
}

/* SUPERTILE_COORDINATES for every supertile of a w x h job, sw x sh tiles
 * of tw x th pixels each, row by row: three bytes each (opcode, column,
 * row), written in one claim (up to 255 packets; v3dcore's writer took one
 * claim and clear loop a packet). */
static void supertiles(V3DContext *v, ULONG w, ULONG h, ULONG sw, ULONG sh, ULONG tw, ULONG th)
{
    ULONG nx = (w - 1) / (tw * sw) + 1, ny = (h - 1) / (th * sh) + 1, sx, sy;
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, 3 * nx * ny);
    if (!b)
        return; /* the list overflowed: the flush drops the job (any_overflow) */
    for (sy = 0; sy < ny; sy++)
        for (sx = 0; sx < nx; sx++, b += 3) {
            b[0] = v3d_OP_SUPERTILE_COORDINATES;
            b[1] = (UBYTE)sx;
            b[2] = (UBYTE)sy;
        }
}

/* TILE_RENDERING_MODE_CFG (Clear Colors Part1, Part2 for a 64- or
 * 128-bit target, Part3 for a 128-bit one or the UIF padding), render
 * target rt, as raw little-endian bytes: c[0..3] the clear colour in the
 * target's internal type (Mesa's v3dx_rcl.c: Part1 the low 32 bits and the
 * next 24, Part2 the 56 after them, Part3 the high 16 and the padding, which
 * Mesa sends when the image's padded height exceeds the job's by 15 UIF
 * blocks or more). */
static void put_bits(UBYTE *b, ULONG v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        b[i] = (UBYTE)(v >> (8 * i));
}

static void clear_colors(V3DContext *v, UBYTE rt, const ULONG *c, UBYTE bpp, ULONG pad)
{
    UBYTE *b = v3d_buffer_claim_memory(v->current_buf, 9 + (bpp >= V3D_INTERNAL_BPP_64 ? 9 : 0) +
                                                           (bpp >= V3D_INTERNAL_BPP_128 || pad ? 9 : 0));
    if (!b)
        return;
    b[0] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1;
    b[1] = (UBYTE)(3 | rt << 4); /* sub-id 3, the render target */
    put_bits(b + 2, c[0], 4);
    put_bits(b + 6, c[1], 3);
    b += 9;
    if (bpp >= V3D_INTERNAL_BPP_64) {
        b[0] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2;
        b[1] = (UBYTE)(4 | rt << 4); /* sub-id 4 */
        put_bits(b + 2, (c[1] >> 24) | (c[2] << 8), 4);
        put_bits(b + 6, (c[2] >> 24) | ((c[3] & 0xffff) << 8), 3);
        b += 9;
    }
    if (bpp >= V3D_INTERNAL_BPP_128 || pad) {
        b[0] = v3d_OP_TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3;
        b[1] = (UBYTE)(5 | rt << 4); /* sub-id 5 */
        put_bits(b + 2, c[3] >> 16, 2);
        b[4] = b[5] = 0;              /* raster stride or image height */
        b[6] = (UBYTE)pad;            /* bits 40..52: the padded height */
        b[7] = (UBYTE)((pad >> 8) & 0x1f);
        b[8] = 0;
    }
}

/* An integer clamped to `bits` (signed or not), as Mesa's util_clamp_color
 * does to a clear value for the target's format. */
static ULONG clamp_int(ULONG v, int bits, BOOL sgn)
{
    if (bits >= 32)
        return v;
    if (sgn) {
        LONG x = (LONG)v, lo = -(1L << (bits - 1)), hi = (1L << (bits - 1)) - 1;
        return (ULONG)(x < lo ? lo : x > hi ? hi : x);
    }
    return v > (1UL << bits) - 1 ? (1UL << bits) - 1 : v;
}

/* Target rt's clear value in its internal type (Mesa's v3d_tlb_clear): the
 * format's clamps first (fixed point to [0, 1], integers to their range),
 * then 8-bit, 16F, 16-bit integer or 32-bit channels. */
static void pack_clear(struct OGLES3Context *ctx, const struct OGLES3RtFormat *rf, int rt, ULONG *c)
{
    const ULONG *v = ctx->job_clear_rt[rt].v;
    BOOL is_int = ctx->job_clear_rt[rt].is_int;
    BOOL integer = rf->kind == OGLES3_RT_INT || rf->kind == OGLES3_RT_UINT;
    ULONG u[4];
    GLfloat f[4];
    int k;
    for (k = 0; k < 4; k++) {
        f[k] = is_int ? (GLfloat)(LONG)v[k] : bitsf(v[k]);
        if (rf->kind == OGLES3_RT_UNORM)
            f[k] = f[k] < 0.0f ? 0.0f : f[k] > 1.0f ? 1.0f : f[k];
        u[k] = is_int ? v[k] : (ULONG)(LONG)f[k];
        if (integer)
            u[k] = clamp_int(u[k], rf->out == V3D_OUTPUT_IMAGE_FORMAT_RGB10A2UI ? (k == 3 ? 2 : 10) : rf->bits,
                             rf->kind == OGLES3_RT_INT);
    }
    c[0] = c[1] = c[2] = c[3] = 0;
    switch (rf->type) {
    case V3D_INTERNAL_TYPE_8:
        c[0] = (ULONG)to_byte(f[0]) | (ULONG)to_byte(f[1]) << 8 | (ULONG)to_byte(f[2]) << 16 |
               (ULONG)to_byte(f[3]) << 24;
        break;
    case V3D_INTERNAL_TYPE_8I:
    case V3D_INTERNAL_TYPE_8UI:
        c[0] = (u[0] & 0xff) | (u[1] & 0xff) << 8 | (u[2] & 0xff) << 16 | (u[3] & 0xff) << 24;
        break;
    case V3D_INTERNAL_TYPE_16F:
        c[0] = ogles2_float_to_half(f[0]) | (ULONG)ogles2_float_to_half(f[1]) << 16;
        c[1] = ogles2_float_to_half(f[2]) | (ULONG)ogles2_float_to_half(f[3]) << 16;
        break;
    case V3D_INTERNAL_TYPE_16I:
    case V3D_INTERNAL_TYPE_16UI:
        c[0] = (u[0] & 0xffff) | u[1] << 16;
        c[1] = (u[2] & 0xffff) | u[3] << 16;
        break;
    case V3D_INTERNAL_TYPE_32F:
        for (k = 0; k < 4; k++)
            c[k] = fbits(f[k]);
        break;
    default: /* 32-bit integers */
        for (k = 0; k < 4; k++)
            c[k] = u[k];
        break;
    }
}

/* The render list of a framebuffer object's job (Mesa's v3dx_rcl.c): colour
 * loaded unless cleared, depth and stencil loaded unless cleared, all
 * stored, in the images' own layouts; no Y flip. Each colour image drawn
 * into is a render target of its own (EXT_draw_buffers: output i, target
 * i), all cleared alike. Multisampled renderbuffers: 4x mode, every image
 * loaded and stored with all four samples (Mesa's decimate mode for a
 * surface of more than one sample). */
static void build_render_fbo(struct OGLES3Context *ctx, const struct OGLES3Target *tg, ULONG ta)
{
    V3DContext *v = ctx->v3d;
    v3d_u8 s = v->build_slot;
    UWORD width = tg->w, height = tg->h;
    ULONG tilesX = (width + tg->tile_w - 1) / tg->tile_w, tilesY = (height + tg->tile_h - 1) / tg->tile_h;
    ULONG start, end, sw = 1, sh = 1, fw, fh;
    UBYTE rt_bpp[4], rt_type[4], rt_clamp[4];
    UBYTE rt;
    UBYTE clear_color = v->pending_clear_color, clear_zs = v->pending_clear_depth;
    UBYTE ez = v->first_ez_state;
    UBYTE ez_off = (ez == V3D_EZ_DISABLED) || (width & 1) || (height & 1); /* GFXH-1918 */
    /* EXT_discard_framebuffer: not stored by this job, not loaded from before it. */
    UBYTE no_store = ctx->job_discard, no_load = ctx->job_noload;
    BOOL store_c = tg->colors && !(no_store & OGLES3_DISCARD_COLOR);
    BOOL store_z = tg->has_z && !(no_store & OGLES3_DISCARD_DEPTH &&
                                  (!tg->packed_zs || (no_store & OGLES3_DISCARD_STENCIL) || !ctx->fb_stencil_bits));
    BOOL store_s = tg->has_s && !(no_store & OGLES3_DISCARD_STENCIL);
    /* Mesa: the early Z/S clear only without a Z/S load or store. */
    UBYTE early_zs_clear = !(tg->has_z || tg->has_s);
    UBYTE dm = tg->msaa ? V3D_DECIMATE_MODE_ALL_SAMPLES : V3D_DECIMATE_MODE_SAMPLE_0;
    const struct OGLES3Surface *c, *z = &tg->z, *st = &tg->s;

    v->current_buf = &v->binning_buf[s];
    SetBuffer(v, &v->binning_buf[s]);
    ogles2_xfb_job_end(ctx); /* after transform feedback: its counts, the specs off */
    if (ctx->probe & OGLES3_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_INCREMENT_SEMAPHORE);
    DoCommand(v, v3d_OP_FLUSH);

    /* Generic tile list. */
    v->current_buf = &v->tile_list_buf[s];
    SetBuffer(v, &v->tile_list_buf[s]);
    start = CurrentBufferAddress(v);
    DoCommand(v, v3d_OP_TILE_COORDINATES_IMPLICIT);
    if (!(no_load & OGLES3_DISCARD_COLOR))
        for (rt = 0; rt < tg->nrt; rt++)
            if ((tg->colors & (1 << rt)) && !(clear_color && (ctx->job_clear_targets & (1 << rt)))) {
                c = &tg->color[rt];
                LoadTileBufferGeneral(v, v3d_RENDER_TARGET_0 + rt, c->mem_format, FALSE, dm, c->format,
                                      c->force_alpha1, FALSE, FALSE, c->ub, 0, c->addr);
            }
    if (!clear_zs) {
        if (tg->has_z && !((no_load & OGLES3_DISCARD_DEPTH) &&
                           (!tg->packed_zs || (no_load & OGLES3_DISCARD_STENCIL) || !ctx->fb_stencil_bits)))
            LoadTileBufferGeneral(v, tg->packed_zs ? v3d_ZSTENCIL : v3d_Z, z->mem_format, FALSE, dm, z->format,
                                  FALSE, FALSE, FALSE, z->ub, 0, z->addr);
        if (tg->has_s && !(no_load & OGLES3_DISCARD_STENCIL))
            LoadTileBufferGeneral(v, v3d_STENCIL, st->mem_format, FALSE, dm, st->format, FALSE, FALSE, FALSE,
                                  st->ub, 0, st->addr);
    }
    DoCommand(v, v3d_OP_END_OF_LOADS);
    PrimListFormat(v, v3d_LIST_TRIANGLES, FALSE);
    SetInstanceid(v, 0);
    BranchToImplicitTileList(v, 0);
    if (store_c)
        for (rt = 0; rt < tg->nrt; rt++)
            if (tg->colors & (1 << rt)) {
                c = &tg->color[rt];
                StoreTileBufferGeneral(v, v3d_RENDER_TARGET_0 + rt, c->mem_format, FALSE, V3D_DITHER_MODE_NONE,
                                       dm, c->format, FALSE, FALSE, FALSE, c->ub, 0, c->addr);
            }
    if (store_z)
        StoreTileBufferGeneral(v, tg->packed_zs ? v3d_ZSTENCIL : v3d_Z, z->mem_format, FALSE,
                               V3D_DITHER_MODE_NONE, dm, z->format, FALSE, FALSE, FALSE, z->ub, 0, z->addr);
    if (store_s)
        StoreTileBufferGeneral(v, v3d_STENCIL, st->mem_format, FALSE, V3D_DITHER_MODE_NONE, dm, st->format,
                               FALSE, FALSE, FALSE, st->ub, 0, st->addr);
    if (!store_c && !store_z && !store_s) /* Mesa: a tile stores something */
        StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (clear_color || clear_zs)
        ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_RETURN_FROM_SUB_LIST);
    end = CurrentBufferAddress(v);

    /* Render list. */
    v->current_buf = &v->render_buf[s];
    SetBuffer(v, &v->render_buf[s]);
    if (ctx->probe & OGLES3_PROBE_SEMAPHORE)
        DoCommand(v, v3d_OP_WAIT_ON_SEMAPHORE);
    TileRenderingModeCFGCommon(v, tg->nrt, width, height, tg->bpp, tg->msaa, FALSE,
                               ez == V3D_EZ_GT_GE ? v3d_EARLY_Z_DIRECTION_GT_GE
                                                  : v3d_EARLY_Z_DIRECTION_LT_LE,
                               ez_off, tg->depth_type, early_zs_clear);
    /* Each target's internal type, bpp and clamp (Mesa's
     * v3d_setup_render_target: clamped to the format's range for integers,
     * to [0, 1] for sRGB, else not) and its clear value in that type; a
     * target no draw buffer names is an 8-bit one. The fragment shader
     * writes 32-bit floats or integers to the 32F and integer ones (the
     * key's f32_color_rb, draw.c), 16-bit floats to the rest. */
    for (rt = 0; rt < tg->nrt; rt++) {
        static const struct OGLES3RtFormat plain = { V3D_OUTPUT_IMAGE_FORMAT_RGBA8, V3D_INTERNAL_TYPE_8,
                                                     V3D_INTERNAL_BPP_32, 0, OGLES3_RT_UNORM, 4, 8, 0,
                                                     { 0, 1, 2, 3 }, 84 };
        const struct OGLES3RtFormat *rf = (tg->colors & (1 << rt)) ? &tg->rt[rt] : &plain;
        ULONG cc[4], pad = 0;
        c = &tg->color[rt];
        rt_bpp[rt] = rf->bpp;
        rt_type[rt] = rf->type;
        rt_clamp[rt] = rf->clamp;
        pack_clear(ctx, rf, rt, cc);
        if ((tg->colors & (1 << rt)) && c->ub) {
            ULONG ubh = 2 * v3d_utile_height((int)c->cpp);
            if (c->ub - (height + ubh - 1) / ubh >= 15)
                pad = c->ub;
        }
        clear_colors(v, rt, cc, rf->bpp, pad);
    }
    TileRenderingModeCFGColorEach(v, tg->nrt, rt_bpp, rt_type, rt_clamp);
    TileRenderingModeCFGZSClearValues(v, clear_zs ? ctx->job_clear_stencil : 0,
                                      clear_zs ? v->clear_depth_value : 1.0f);
    TileListInitialBlockSize(v, v3d_TILE_ALLOCATION_BLOCK_SIZE_64B, TRUE);

    for (;;) {
        fw = (tilesX + sw - 1) / sw;
        fh = (tilesY + sh - 1) / sh;
        if (fw * fh < 256)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }
    Multicore_Rendering_Tile_List_Set_Base(v, ta, 0);
    MulticoreRenderingSupertileCFG(v, sw, sh, fw, fh, 1, FALSE, FALSE, tilesY, tilesX);

    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ClearTileBuffers(v, !early_zs_clear, TRUE);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    TileCoordinates(v, 0, 0);
    DoCommand(v, v3d_OP_END_OF_LOADS);
    StoreTileBufferGeneral(v, v3d_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    DoCommand(v, v3d_OP_END_OF_TILE_MARKER);
    DoCommand(v, v3d_OP_FLUSH_VCD_CACHE);

    StartAddressOfGenericTileList(v, start, end);
    supertiles(v, width, height, sw, sh, tg->tile_w, tg->tile_h);
    DoCommand(v, v3d_OP_END_OF_RENDERING);
}

/* The colour image after the GPU's write: nothing here. The CPU reads it
 * only through v3dcore's v3d_texture_load (texture.c's sync, glReadPixels,
 * glCopyTex*), which invalidates the level first; it was cleaned before
 * the render with everything else (clean_all). An exec cache call costs
 * about 41 us (M6): one less per framebuffer-object job. Depth and stencil
 * images are never read by the CPU. */
static void target_dma(const struct OGLES3Target *tg, BOOL after)
{
    (void)tg;
    (void)after;
}

/* Before the GPU reads anything the CPU wrote since the last flush (the
 * control lists, the state buffer, buffer copies, the render target): one
 * clean of the whole data cache. On Emu68 every exec cache call cleans the
 * whole cache whatever its range, about 41 microseconds each
 * (tests/perf/cachetime.c), and the flush made five of them (M6). Data
 * only: clearing the instruction cache would make Emu68 drop its
 * translated code. */
static void clean_all(void)
{
    CacheClearE(0, ~0UL, CACRF_ClearD); /* length ~0: the whole cache */
}

/* After a render: a GPU that did not finish reset, the CPU cache over what
 * it wrote, the locks released, the retire list, the colour image marked. */
static void after_render(struct OGLES3Context *ctx, struct OGLES3Framebuffer *fb, const struct OGLES3Target *tg,
                         v3d_wait_result wr, BOOL holds_lock)
{
    V3DContext *v = ctx->v3d;
    ULONG t0;
    if (wr != v3d_wait_result_success) {
        /* The reset needs the submission lock; a render left running gave
         * it up when it was submitted. */
        if (holds_lock || v3d_gpu_lock(ogles2_device_gpu_lock(), GPU_LOCK_TIMEOUT_MS)) {
            ogles2_device_reset("a render job it did not finish");
            if (!holds_lock)
                v3d_gpu_unlock(ogles2_device_gpu_lock());
        }
    }
    t0 = ogles2_clock_us();
    if (fb) {
        target_dma(tg, TRUE);
    } else {
        ogles2_display_dma(&ctx->display, TRUE);
        ogles2_display_unlock(&ctx->display);
    }
    ctx->sum_post += (double)(ULONG)(ogles2_clock_us() - t0);
    /* Fragment shaders stored into storage buffers (ES 3.1): the TMU's
     * writes cleaned to memory, as the kernel does for a job Mesa flags
     * needs_flush (such a job is never left running: may_defer). */
    if (ctx->job_storage) {
        if (holds_lock)
            v3d_clean_caches();
        ctx->job_storage = FALSE;
    }
    if (holds_lock)
        v3d_gpu_unlock(ogles2_device_gpu_lock());
    if (wr == v3d_wait_result_success) {
        v3d_frame_retire(ogles2_device(), &v->frame);
        if (fb)
            ogles2_fbo_rendered(ctx, fb);
    } else {
        ogles2_log("job: render %s", (ULONG)(wr == v3d_wait_result_timed_out ? "timed out" : "failed"));
        if (!fb)
            ctx->zs_saved = FALSE;
    }
}

/* The render this context left running: waited for (unless another
 * submitter already saw it finish: take_gpu), then what follows a render. */
void ogles2_job_finish(struct OGLES3Context *ctx)
{
    v3d_wait_result wr = v3d_wait_result_success;
    BOOL done;
    ULONG t0;
    if (!ctx->pend)
        return;
    ctx->pend = FALSE;
    ObtainSemaphore(&OGLES3LibBase->lock);
    done = ctx->pend_done;
    ctx->pend_done = FALSE;
    if (OGLES3LibBase->pend_ctx == ctx)
        OGLES3LibBase->pend_ctx = 0;
    ReleaseSemaphore(&OGLES3LibBase->lock);
    if (!done) {
        /* Nothing was submitted after it (take_gpu would have marked it
         * done), so the count is one past ours at most: no wrap-around
         * case the wait cannot tell. */
        t0 = ogles2_clock_us();
        wr = v3d_wait_render_timeout(ctx->pend_frame);
        ctx->sum_render += (double)(ULONG)(ogles2_clock_us() - t0);
    }
    after_render(ctx, ctx->pend_fb, &ctx->pend_tg, wr, FALSE);
}

/* A flush may leave its render running (see the top): a framebuffer
 * object's job, or one on a screen of our own or on the screen a window is
 * all of (display.c, window_is_screen). Never one whose bitmap lock
 * someone else may need: an application's bitmap can be drawn into by
 * another task, and the window mode's copy needs the finished frame. With
 * any number of contexts and programs (M6, plan 7.1 phase 2 within ogles2):
 * the submission lock is given up once the render is submitted, and the
 * next submitter waits for that render on the GPU (take_gpu). */
static BOOL may_defer(struct OGLES3Context *ctx, BOOL object)
{
    if (ctx->job_storage) /* its caches are cleaned after it, under the lock (after_render) */
        return FALSE;
    return object || ctx->display.own_screen || ctx->display.direct;
}

/* Before a submission, with the submission lock held: a render some
 * context left running finishes on the GPU first (one job on the GPU at a
 * time: the waits read the GPU's frame counts). This task's own other
 * context: waited for whole (job_finish). Another task's: only on the
 * GPU, and marked done, so its owner does not wait again (two renders past
 * its count could look like none at the count's wrap-around); the owner
 * does the rest at its next flush. Nobody holds the lock while it builds
 * its next frame, so programs take turns at the GPU and none waits for
 * another's CPU work (programs whose tasks wait for each other included). */
static void take_gpu(struct OGLES3Context *ctx)
{
    struct OGLES3Context *p;
    struct Task *t;
    UBYTE frame;
    v3d_wait_result wr;
    ObtainSemaphore(&OGLES3LibBase->lock);
    p = OGLES3LibBase->pend_ctx;
    t = OGLES3LibBase->pend_task;
    frame = OGLES3LibBase->pend_frame;
    ReleaseSemaphore(&OGLES3LibBase->lock);
    if (!p || p == ctx)
        return;
    if (t == FindTask(0)) {
        ogles2_job_finish(p);
        return;
    }
    wr = v3d_wait_render_timeout(frame);
    if (wr != v3d_wait_result_success)
        ogles2_device_reset("another program's render it did not finish");
    ObtainSemaphore(&OGLES3LibBase->lock);
    if (OGLES3LibBase->pend_ctx == p) { /* still not finished by its owner */
        p->pend_done = TRUE;
        OGLES3LibBase->pend_ctx = 0;
    }
    ReleaseSemaphore(&OGLES3LibBase->lock);
}

/* compute.c's dispatches, with the submission lock held. */
void ogles2_job_take_gpu(struct OGLES3Context *ctx)
{
    take_gpu(ctx);
}

static BOOL any_overflow(V3DContext *v)
{
    v3d_u8 s = v->build_slot;
    return v->binning_buf[s].overflowed || v->state_buf[s].overflowed ||
           v->render_buf[s].overflowed || v->tile_list_buf[s].overflowed;
}

/* Renders the open job into the default framebuffer and waits for it.
 * Returns FALSE if the frame was dropped (the reason is logged). */
BOOL ogles2_job_flush(struct OGLES3Context *ctx, BOOL frame_end)
{
    V3DContext *v = ctx->v3d;
    V3DDevice *dev = ogles2_device();
    struct SignalSemaphore *gpu = ogles2_device_gpu_lock();
    v3d_u8 s = v->build_slot;
    v3d_wait_result wb = v3d_wait_result_success, wr = v3d_wait_result_success;
    const char *drop = 0;
    struct OGLES3Framebuffer *fb = ctx->job_fb;
    struct OGLES3Target tg;
    ULONG ta = (ULONG)v->tile_alloc_mem.hostptr, ta_size = (ULONG)v->tile_alloc_mem.size;
    ULONG ts = (ULONG)v->tile_state_mem.hostptr;

    struct zs_save *store_zs = 0;
    ULONG t0, t1, t2, t3;
    BOOL defer;

    ogles2_job_finish(ctx); /* the previous render, if it still runs */
    s = v->build_slot;
    if (!v->frame_active) {
        if (frame_end)
            ctx->zs_saved = FALSE;
        return TRUE;
    }
    t0 = t1 = t2 = t3 = ogles2_clock_us();
    ctx->sum_build += (double)(ULONG)(t0 - ctx->t_job_begin);
    ctx->n_jobs++;
    /* Depth and stencil outlive a mid-frame flush when the job drew, and
     * also when it only loaded them or only cleared them: a job with nothing
     * but a colour clear between two read-backs must hand them on (dEQP-GLES2
     * multisample.stencil lost its stencil there). */
    if (!fb && !frame_end && (v->draw_state_configured || ctx->zs_saved || v->pending_clear_depth) &&
        !(ctx->job_discard & (OGLES3_DISCARD_DEPTH | OGLES3_DISCARD_STENCIL)) &&
        !(ctx->probe & OGLES3_PROBE_NO_ZS_SAVE))
        store_zs = zs_get(ctx); /* NULL without depth and stencil, or memory */
    if (v->frame_corrupted) {
        drop = fb ? "no memory for the framebuffer object's tile pools, or a draw's uniform stream went stale"
                  : "a draw's uniform stream went stale (state buffer grew mid-draw)";
        goto out;
    }
    if (fb) {
        if (!ogles2_fbo_target(ctx, fb, TRUE, &tg)) {
            drop = "the framebuffer object's images have no GPU memory";
            goto out;
        }
        if (tg.ta_addr) {
            ta = tg.ta_addr;
            ta_size = tg.ta_size;
            ts = tg.ts_addr;
        }
    }
    defer = may_defer(ctx, fb != 0);
    if (!v3d_gpu_lock(gpu, GPU_LOCK_TIMEOUT_MS)) {
        drop = "the GPU stayed busy (another program holds the submission lock)";
        goto out;
    }
    take_gpu(ctx); /* with the lock: a render left running finishes first */
    if (!fb && !ogles2_display_lock(&ctx->display)) {
        v3d_gpu_unlock(gpu);
        drop = "the render bitmap could not be locked";
        goto out;
    }

    if (fb)
        build_render_fbo(ctx, &tg, ta);
    else
        build_render(ctx, store_zs);
    if (any_overflow(v)) {
        if (!fb)
            ogles2_display_unlock(&ctx->display);
        v3d_gpu_unlock(gpu);
        drop = "a control list overflowed";
        goto out;
    }
    t1 = ogles2_clock_us();
    ctx->sum_list += (double)(ULONG)(t1 - t0);
    clean_all(); /* the target's lines and v3d_context_flush_for_dma's buffers */
    v3d_invalidate_caches();
    if (ctx->probe & OGLES3_PROBE_SEMAPHORE) {
        /* Probe S: both lists go to the GPU at once; the render list waits
         * for the binner's semaphore. Out-of-memory service stays in the
         * binning wait. */
        v3d_u8 last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_mem[s].hostptr,
                                         (v3d_address)((ULONG)v->binning_buf[s].start +
                                                       v->binning_buf[s].used),
                                         (v3d_address)ta, (v3d_u32)ta_size, (v3d_address)ts);
        v3d_u8 frame_no = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                            (v3d_address)((ULONG)v->render_buf[s].start +
                                                          v->render_buf[s].used));
        wb = v3d_wait_binning_timeout(dev, &v->frame, last);
        v3d_frame_render_submitted(&v->frame);
        wr = v3d_wait_render_timeout(frame_no);
        goto waited;
    }
    {
        v3d_u8 last;
        t2 = ogles2_clock_us();
        ctx->sum_pre += (double)(ULONG)(t2 - t1);
        t1 = t2;
        last = v3d_submit_binning(dev, &v->frame, (v3d_address)(ULONG)v->binning_mem[s].hostptr,
                                  (v3d_address)((ULONG)v->binning_buf[s].start + v->binning_buf[s].used),
                                  (v3d_address)ta, (v3d_u32)ta_size, (v3d_address)ts);
        wb = v3d_wait_binning_timeout(dev, &v->frame, last);
        t2 = ogles2_clock_us();
    }
    ctx->sum_bin += (double)(ULONG)(t2 - t1);
    if (wb == v3d_wait_result_success) {
        v3d_u8 frame_no;
        v3d_invalidate_caches();
        frame_no = v3d_submit_render((v3d_address)(ULONG)v->render_buf[s].start,
                                     (v3d_address)((ULONG)v->render_buf[s].start +
                                                   v->render_buf[s].used));
        v3d_frame_render_submitted(&v->frame);
        if (defer) {
            /* The render runs on; ogles2_job_finish waits for it. The
             * screen's bitmap is unlocked now: it is on display, so it does
             * not move, and the lock held across the program's next frame
             * blocked input.device (P96's pointer is drawn into the screen
             * bitmap): ioquake3's input handler removal hung on it at exit. */
            if (!fb)
                ogles2_display_unlock(&ctx->display);
            ctx->pend = TRUE;
            ctx->pend_frame = frame_no;
            ctx->pend_serial = ctx->job_serial;
            ctx->pend_fb = fb;
            ctx->pend_done = FALSE;
            ObtainSemaphore(&OGLES3LibBase->lock);
            OGLES3LibBase->pend_ctx = ctx;
            OGLES3LibBase->pend_task = FindTask(0);
            OGLES3LibBase->pend_frame = frame_no;
            ReleaseSemaphore(&OGLES3LibBase->lock);
            /* The lock goes now, not at the program's next flush: the next
             * submitter, of any program, waits for this render on the GPU
             * (take_gpu). */
            v3d_gpu_unlock(gpu);
            if (fb)
                ctx->pend_tg = tg;
            ctx->frames++;
            v->build_slot = 1 - v->build_slot;
            g_v3d_cl_generation++;
            v->frame_active = FALSE;
            if (fb) {
                if (frame_end)
                    ctx->zs_saved = FALSE;
            } else {
                ctx->zs_saved = store_zs != 0; /* undone if the render fails */
            }
            return TRUE;
        }
        wr = v3d_wait_render_timeout(frame_no);
        t3 = ogles2_clock_us();
        ctx->sum_render += (double)(ULONG)(t3 - t2);
    }
waited:
    if (wb == v3d_wait_result_success) {
        after_render(ctx, fb, &tg, wr, TRUE);
    } else {
        /* A job the GPU did not finish leaves it busy or stopped: reset it
         * before anyone submits again (plan 9 M4). */
        ogles2_device_reset("a binning job it did not finish");
        if (fb) {
            target_dma(&tg, TRUE);
        } else {
            ogles2_display_dma(&ctx->display, TRUE);
            ogles2_display_unlock(&ctx->display);
        }
        v3d_gpu_unlock(gpu);
    }

    if (wb != v3d_wait_result_success) {
        drop = wb == v3d_wait_result_timed_out ? "binning timed out" : "binning failed";
        /* Where the control list executor stopped (raw little-endian words). */
        ogles2_log("job: binning stopped: CT0CA 0x%08lx CT0EA 0x%08lx CT0CS 0x%08lx; list 0x%08lx..0x%08lx "
                   "(last block 0x%08lx), %ld spill blocks",
                   (ULONG)LE32(V3D_CT0CA), (ULONG)LE32(V3D_CT0EA), (ULONG)LE32(V3D_CT0CS),
                   (ULONG)v->binning_mem[s].hostptr,
                   (ULONG)v->binning_buf[s].start + v->binning_buf[s].used, (ULONG)v->binning_buf[s].start,
                   (LONG)v->frame.job_spills);
    }
    ctx->frames++;
    v->build_slot = 1 - v->build_slot;
    g_v3d_cl_generation++;

out:
    v->frame_active = FALSE;
    if (fb) {
        /* The default framebuffer's saved depth and stencil stay for its
         * next job, unless the frame ends here. */
        if (frame_end)
            ctx->zs_saved = FALSE;
    } else {
        /* The next job loads depth and stencil only if this one stored them
         * and rendered (a frame end stores nothing). */
        ctx->zs_saved = !drop && wr == v3d_wait_result_success && store_zs;
    }
    if (drop) {
        ctx->frames_dropped++;
        ogles2_log("job: frame dropped: %s", (ULONG)drop);
        return FALSE;
    }
    return wr == v3d_wait_result_success;
}

BOOL ogles2_job_has_draws(struct OGLES3Context *ctx)
{
    return ctx->v3d && ctx->v3d->frame_active && ctx->v3d->draw_state_configured;
}

/* aglDestroyContext: forget an open job (nothing was submitted). */
void ogles2_job_discard(struct OGLES3Context *ctx)
{
    ogles2_job_finish(ctx);
    if (ctx->v3d)
        ctx->v3d->frame_active = FALSE;
}
