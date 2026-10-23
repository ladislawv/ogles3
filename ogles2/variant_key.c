/*
 * The draw-time variants' keys (variant_key.h; docs/GLES3_PLAN.md stage A
 * item 5): the fields a draw's state gives, at their offsets in the key
 * bytes (v3dp/FORMAT.md), as Mesa's v3d_update_compiled_fs and _vs set them
 * (src/gallium/drivers/v3d/v3d_program.c). A variant serves a draw when its
 * key has those fields; its 32-bit samplers may be more than the draw's (a
 * 32-bit return reads 8-bit textures correctly too, as 1.3's .v3dw variants
 * do). Checked on the host: tools/variant_key_test.
 */
#include <string.h>
#include "variant_key.h"

#define FK_POINTS 12
#define FK_LINES  13
#define FK_MSAA   16   /* msaa, sample_alpha_to_coverage */
#define FK_SOFTWARE_BLEND 20
#define FK_IGNORE_MASK 21
#define FK_CBUFS  22
#define FK_F32    24   /* f32_color_rb, norm_16, snorm */
#define FK_FMT    32   /* color_fmt[8], blend[8]: to the logic op */
#define FK_LOGICOP 288
#define VK_NUM_OUTPUTS 12
#define VK_OUTPUTS 16  /* used_outputs[64] */
#define VK_PSIZE  85   /* per_vertex_point_size */

ULONG ogles2_vk_bits(const UBYTE *k)
{
    return (ULONG)k[0] | (ULONG)k[1] << 8 | (ULONG)k[2] << 16 | (ULONG)k[3] << 24;
}

static void set_bits(UBYTE *k, ULONG bits)
{
    k[0] = (UBYTE)bits;
    k[1] = (UBYTE)(bits >> 8);
    k[2] = (UBYTE)(bits >> 16);
    k[3] = (UBYTE)(bits >> 24);
}

void ogles2_vk_fs_want(UBYTE *want, const UBYTE *base, int prim, UBYTE cbufs, ULONG bits, BOOL a2c)
{
    memcpy(want, base, OGLES2_VK_FS_BYTES);
    set_bits(want, bits);
    want[FK_POINTS] = prim == OGLES2_VK_POINTS;
    want[FK_LINES] = prim == OGLES2_VK_LINES;
    if (a2c) { /* as Mesa's for a multisampled job with alpha to coverage */
        want[FK_MSAA] = want[FK_MSAA + 1] = 1;
        want[FK_IGNORE_MASK] = 0;
    } else {   /* the closest variant's, without it (1.x's: no MSAA in the key) */
        want[FK_MSAA + 1] = 0;
    }
    want[FK_SOFTWARE_BLEND] = 0;
    want[FK_CBUFS] = cbufs;
    want[FK_F32] = want[FK_F32 + 1] = want[FK_F32 + 2] = 0;
    memset(want + FK_FMT, 0, FK_LOGICOP - FK_FMT);
    want[FK_LOGICOP] = 12; /* PIPE_LOGICOP_COPY, little-endian */
    want[FK_LOGICOP + 1] = want[FK_LOGICOP + 2] = want[FK_LOGICOP + 3] = 0;
}

BOOL ogles2_vk_fs_fits(const UBYTE *k, const UBYTE *want)
{
    ULONG w = ogles2_vk_bits(want);
    return k && (ogles2_vk_bits(k) & w) == w && k[FK_MSAA] == want[FK_MSAA] &&
           k[FK_MSAA + 1] == want[FK_MSAA + 1] && k[FK_IGNORE_MASK] == want[FK_IGNORE_MASK] &&
           k[FK_POINTS] == want[FK_POINTS] && k[FK_LINES] == want[FK_LINES] &&
           k[FK_SOFTWARE_BLEND] == want[FK_SOFTWARE_BLEND] && k[FK_CBUFS] == want[FK_CBUFS] &&
           !memcmp(k + FK_F32, want + FK_F32, 3) && !memcmp(k + FK_FMT, want + FK_FMT, FK_LOGICOP + 4 - FK_FMT);
}

void ogles2_vk_vs_want(UBYTE *want, const UBYTE *base, UBYTE num_inputs, const UBYTE *slots, ULONG bits)
{
    memcpy(want, base, OGLES2_VK_VS_BYTES);
    set_bits(want, bits);
    if (slots) {
        want[VK_NUM_OUTPUTS] = num_inputs;
        memcpy(want + VK_OUTPUTS, slots, 64);
    }
}

BOOL ogles2_vk_vs_fits(const UBYTE *k, const UBYTE *want)
{
    ULONG w = ogles2_vk_bits(want);
    return k && k[VK_NUM_OUTPUTS] == want[VK_NUM_OUTPUTS] && !memcmp(k + VK_OUTPUTS, want + VK_OUTPUTS, 64) &&
           k[VK_PSIZE] == want[VK_PSIZE] && (ogles2_vk_bits(k) & w) == w;
}
