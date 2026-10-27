/*
 * ogles2: compute shader dispatches with the CSD of V3D 4.2. include/v3d_csd.h.
 *
 * The registers are the Linux kernel's V3D 4.x core registers
 * (drivers/gpu/drm/v3d/v3d_regs.h: V3D_CSD_STATUS 0x900, the queued
 * configuration words 0x904-0x91c), written as its v3d_csd_job_run writes
 * them: CFG1 to CFG6, then CFG0, which queues the dispatch. Completion is
 * polled, as Mesa's simulator does (v3dx_simulator.c): the completed
 * dispatches' count in CSD_STATUS changes and no dispatch is current or
 * queued. Then the caches are cleaned as the kernel's v3d_clean_caches does
 * before it signals a CSD job done.
 */
#include <exec/types.h>
#include "../include/v3d_csd.h"
#include "v3d_hw.h"
#include "v3d_regs.h"
#include "v3d_debug.h"

#define V3D_CSD_STATUS      *(volatile uint32_t*)(V3D_BASE+0x00900)
#define V3D_CSD_QUEUED_CFG0 ((volatile uint32_t*)(V3D_BASE+0x00904)) /* CFG0-CFG6: +4 each */
#define V3D_CSD_CURRENT_CFG4 *(volatile uint32_t*)(V3D_BASE+0x00930)
/* V3D_CSD_STATUS, natural bit order (test the LE32-swapped word). */
#define CSD_STATUS_QUEUED   0x1 /* HAVE_QUEUED_DISPATCH */
#define CSD_STATUS_CURRENT  0x2 /* HAVE_CURRENT_DISPATCH */
#define CSD_STATUS_DONE_SHIFT 4 /* NUM_COMPLETED_JOBS, bits 11:4 (wraps) */

/* V3D_L2TCACTL's bits as v3d_hw.c writes it, unswapped (v3d_regs.h): TMUWCF
 * is bit 8, the flush mode FLM bits 2:1 (clean: 2), L2TFLS bit 0. */
#define L2TCACTL_TMUWCF    0x00010000
#define L2TCACTL_FLM_CLEAN 0x04000000

/* Polls of a register before the GPU counts as stuck: the dispatcher being
 * idle before a dispatch, a cache operation. */
#define CSD_POLLS_MIN 200000UL
/* Polls of V3D_CSD_STATUS without progress (the batches left to start,
 * CURRENT_CFG4, standing still) before a dispatch counts as stuck: whole
 * seconds, since one batch may take long (dEQP-GLES31's atomic_counter
 * cases: thousands of atomic operations on one counter took longer than a
 * fixed 200,000 polls, and the dispatch was reset as stuck). */
#define CSD_STALL_POLLS 4000000UL

static v3d_u8 completed(void)
{
    return (v3d_u8)(((v3d_u32)LE32(V3D_CSD_STATUS) >> CSD_STATUS_DONE_SHIFT) & 0xFF);
}

static int busy(void)
{
    return ((v3d_u32)LE32(V3D_CSD_STATUS) & (CSD_STATUS_QUEUED | CSD_STATUS_CURRENT)) != 0;
}

void v3d_clean_caches(void)
{
    v3d_u32 polls;
    for (polls = 0; (V3D_L2TCACTL & V3D_L2TCACTL_L2TFLS) && polls < CSD_POLLS_MIN; polls++)
        ; /* GFXH-1897: a flush still under way first */
    V3D_L2TCACTL = L2TCACTL_TMUWCF;
    for (polls = 0; (V3D_L2TCACTL & L2TCACTL_TMUWCF) && polls < CSD_POLLS_MIN; polls++)
        ;
    V3D_L2TCACTL = V3D_L2TCACTL_L2TFLS | L2TCACTL_FLM_CLEAN;
    for (polls = 0; (V3D_L2TCACTL & V3D_L2TCACTL_L2TFLS) && polls < CSD_POLLS_MIN; polls++)
        ;
}

int v3d_csd_dispatch(const v3d_u32 cfg[7], v3d_u32 batches)
{
    v3d_u32 polls, left, last;
    v3d_u8 before;
    int i;

    /* Another client's dispatch would be ours to wait for; the lock keeps
     * them out, so the dispatcher is normally idle. */
    for (polls = 0; busy() && polls < CSD_POLLS_MIN; polls++)
        ;
    before = completed();
    v3d_invalidate_caches();
    for (i = 1; i <= 6; i++)
        V3D_CSD_QUEUED_CFG0[i] = LE32(cfg[i]);
    V3D_CSD_QUEUED_CFG0[0] = LE32(cfg[0]); /* queues it */

    (void)batches;
    last = V3D_CSD_CURRENT_CFG4;
    for (polls = 0; completed() == before || busy(); polls++) {
        if ((left = V3D_CSD_CURRENT_CFG4) != last) { /* progress: the watchdog starts again */
            last = left;
            polls = 0;
        }
        if (polls > CSD_STALL_POLLS) {
            D(("v3d_csd: no progress in %lu polls (batches left %lu)\n", (ULONG)polls, (ULONG)LE32(left)));
            return -2;
        }
    }
    v3d_clean_caches();
    return 0;
}
