/*
 * ogles2: compute shader dispatches with the CSD (compute shader dispatcher)
 * of V3D 4.2, for OpenGL ES 3.1's glDispatchCompute (docs/GLES3_PLAN.md
 * stage D item 1). New in ogles2's copy of the backend.
 */
#ifndef V3D_CSD_H
#define V3D_CSD_H

#include "v3d_types.h"

/* One dispatch, waited for: the seven configuration words as Mesa's
 * v3d_launch_grid makes them (common/v3d_csd.h: CFG0-2 the work group
 * counts, CFG3 the supergroup and work group sizes, CFG4 the batches - 1,
 * CFG5 the shader's address and flags, CFG6 the uniforms' address);
 * `batches` scales the wait. The caller holds the GPU submission lock, has
 * made every job whose results the shader reads finish, and has cleaned the
 * CPU data cache over what the shader reads. The GPU's caches are
 * invalidated before the start and the TMU's writes cleaned to memory after
 * the end (the kernel's v3d_csd_job_run and v3d_clean_caches). Returns 0
 * when it finished, -2 when it did not. */
int v3d_csd_dispatch(const v3d_u32 cfg[7], v3d_u32 batches);

/* The kernel's v3d_clean_caches: the TMU's write cache flushed and the L2T
 * cleaned, so memory holds what shaders stored (after any job whose shaders
 * write memory). The caller holds the GPU submission lock. */
void v3d_clean_caches(void);

#endif
