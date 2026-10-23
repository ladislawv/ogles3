#ifndef OGLES2_VARIANT_KEY_H
#define OGLES2_VARIANT_KEY_H
/* The draw-time variants' keys (docs/GLES3_PLAN.md stage A item 5;
 * variant_key.c): the key an OpenGL ES 3 program's draw needs, computed as
 * Mesa's v3d_update_compiled_fs and _vs do, and whether a variant's key
 * serves it. Key bytes are v3dp/FORMAT.md's. Checked on the host against
 * the keys v3dglslc stores (tools/variant_key_test, `make variant-key-test`). */
#ifdef VARIANT_KEY_HOST_TEST
typedef unsigned int ULONG;
typedef unsigned char UBYTE;
typedef short BOOL;
#else
#include <exec/types.h>
#endif

#define OGLES2_VK_FS_BYTES 296 /* OGLES2_FS_KEY_BYTES */
#define OGLES2_VK_VS_BYTES 88  /* OGLES2_VS_KEY_BYTES */

/* The rasterizer's primitive class (Mesa's is_points, is_lines). */
#define OGLES2_VK_TRIANGLES 0
#define OGLES2_VK_POINTS    1
#define OGLES2_VK_LINES     2

/* The fragment key's sample_alpha_to_coverage byte. */
#define OGLES2_VK_FS_A2C 17

/* Both keys start with sampler_is_32b, a little-endian u32. */
ULONG ogles2_vk_bits(const UBYTE *key);

/* The fragment key for a draw: `base` (the closest variant's: the fields a
 * draw does not change keep the compiler's values, such as point sprites')
 * with the fields computed from the draw: the primitive class, the colour
 * buffers drawn into, the 32-bit samplers, alpha to coverage (a 4x MSAA
 * default framebuffer with GL_SAMPLE_ALPHA_TO_COVERAGE); every target 8-bit
 * or 16F (stage A): no software blending, formats, blend state, f32_color_rb,
 * norm_16 or snorm, the logic op COPY. */
void ogles2_vk_fs_want(UBYTE *want, const UBYTE *base, int prim, UBYTE cbufs, ULONG bits, BOOL a2c);
/* A fragment variant serves the draw: its key has the computed fields, its
 * 32-bit samplers at least the draw's. */
BOOL ogles2_vk_fs_fits(const UBYTE *key, const UBYTE *want);

/* The vertex key: `base` with the 32-bit samplers and, unless `slots` is
 * NULL, the fragment variant's inputs (num_inputs, input_slots[64]) as its
 * used outputs. */
void ogles2_vk_vs_want(UBYTE *want, const UBYTE *base, UBYTE num_inputs, const UBYTE *slots, ULONG bits);
BOOL ogles2_vk_vs_fits(const UBYTE *key, const UBYTE *want);
#endif
