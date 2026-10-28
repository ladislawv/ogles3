/*
 * .v3dp writer; the format is in FORMAT.md, whose section and field order
 * this file follows. Every value is written through put_*(), which emit
 * little-endian bytes from host values, never memory images of structs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main/mtypes.h"
#include "main/shader_types.h"
#include "util/u_range_remap.h"
#include "program/prog_parameter.h"
#include "compiler/glsl_types.h"
#include "compiler/v3d_compiler.h"
#include "util/mesa-blake3.h"
#include "v3dp_writer.h"

#ifndef V3DP_MESA_COMMIT
#define V3DP_MESA_COMMIT "unknown"
#endif

#define V3DP_MAJOR 1
#define V3DP_MAJOR2 0x8002 /* format 2 (ES3 programs): provisional until release 2.1, FORMAT.md */
#define V3DP_MINOR 0
#define V3DP_V3D_VER 42

enum {
    SEC_PROGRAM = 1,
    SEC_STRINGS = 2,
    SEC_STAGE_PARAMS = 3,
    SEC_UNIFORMS = 4,
    SEC_ATTRIBUTES = 5,
    SEC_VARIANT = 6,
    SEC_UNIFORM_BLOCKS = 7, /* format 2 */
    SEC_INTERFACE = 8,      /* format 2 */
    SEC_VARIANT_OF = 9,     /* .v3dx files */
    SEC_TRANSFORM_FEEDBACK = 10, /* format 2, with the feature bit V3DP_FEATURE_TF */
    SEC_STORAGE_BLOCKS = 11,     /* format 2, with the feature bit V3DP_FEATURE_STORAGE */
    SEC_ATOMIC_BUFFERS = 12,     /* format 2, with the feature bit V3DP_FEATURE_ATOMIC */
    SEC_UNIFORM_LOCATIONS = 13,  /* format 2 */
    SEC_OPAQUE_UNITS = 14,       /* format 2 */
};

enum { VARIANT_VS = 0, VARIANT_VS_BIN = 1, VARIANT_FS = 4, VARIANT_CS = 5 };

/* The file numbers stages as Mesa does; the fixed-size arrays below are part
 * of the format, so a Mesa that changes them needs a new format version. */
static_assert(MESA_SHADER_VERTEX == 0 && MESA_SHADER_FRAGMENT == 4 && MESA_SHADER_COMPUTE == 5, "stage numbers");
static_assert(V3D_MAX_ANY_STAGE_INPUTS == 64, "vs key used_outputs");
static_assert(V3D_MAX_VS_INPUTS == 64 && V3D_MAX_FS_INPUTS == 64, "inputs");
static_assert(V3D_MAX_DRAW_BUFFERS == 8, "draw buffers");
static_assert(STATE_LENGTH == 4, "state indexes");

/* Mesa's hook (patch 0009). */
extern void (*v3d_compiled_variant_hook)(mesa_shader_stage stage,
                                         const struct v3d_key *key, size_t key_size,
                                         const struct v3d_prog_data *prog_data,
                                         const uint64_t *qpu_insts, uint32_t qpu_size);

/* ---- byte buffer ---- */

struct buf {
    uint8_t *p;
    size_t len, cap;
    int failed;
};

static void put_bytes(struct buf *b, const void *data, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        uint8_t *p;
        while (cap < b->len + n)
            cap *= 2;
        p = realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void put_u8(struct buf *b, unsigned v)
{
    uint8_t x = (uint8_t)v;
    put_bytes(b, &x, 1);
}

static void put_u16(struct buf *b, unsigned v)
{
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(b, x, 2);
}

static void put_u32(struct buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(b, x, 4);
}

static void put_u64(struct buf *b, uint64_t v)
{
    put_u32(b, (uint32_t)v);
    put_u32(b, (uint32_t)(v >> 32));
}

static void put_pad4(struct buf *b)
{
    while (b->len & 3)
        put_u8(b, 0);
}

static void buf_free(struct buf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* CRC-32 as zlib's crc32() (reflected, polynomial 0xedb88320). */
static uint32_t crc32_le(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        int k;
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c ^ 0xffffffffu;
}

/* ---- captured variants ---- */

#define MAX_VARIANTS 64

static struct buf variants[MAX_VARIANTS];
static unsigned num_variants;
static int capture_failed;

static void put_key_common(struct buf *b, const struct v3d_key *k)
{
    put_u32(b, k->sampler_is_32b);
    put_u8(b, k->is_last_geometry_stage);
    put_u8(b, k->robust_uniform_access);
    put_u8(b, k->robust_storage_access);
    put_u8(b, k->robust_image_access);
    put_u8(b, k->robust_image_access_2);
    put_u8(b, k->null_descriptor);
    put_u16(b, 0);
}

static void put_prog_data_common(struct buf *b, const struct v3d_prog_data *d)
{
    put_u32(b, d->spill_size);
    put_u32(b, d->tmu_spills);
    put_u32(b, d->tmu_fills);
    put_u32(b, d->tmu_count);
    put_u32(b, d->qpu_read_stalls);
    put_u8(b, d->compile_strategy_idx);
    put_u8(b, d->threads);
    put_u8(b, d->single_seg);
    put_u8(b, d->tmu_dirty_rcl);
    put_u8(b, d->has_control_barrier);
    put_u8(b, d->has_global_address);
    put_u16(b, 0);
}

static void put_vs(struct buf *b, const struct v3d_vs_key *k, const struct v3d_vs_prog_data *d)
{
    unsigned i;

    put_key_common(b, &k->base);
    put_u8(b, k->num_used_outputs);
    put_u8(b, 0);
    put_u16(b, 0);
    for (i = 0; i < 64; i++)
        put_u8(b, k->used_outputs[i].slot_and_component);
    put_u32(b, k->va_swap_rb_mask);
    put_u8(b, k->is_coord);
    put_u8(b, k->per_vertex_point_size);
    put_u8(b, k->clamp_color);
    put_u8(b, 0);

    put_prog_data_common(b, &d->base);
    put_u8(b, d->uses_iid);
    put_u8(b, d->uses_biid);
    put_u8(b, d->uses_vid);
    put_u8(b, d->separate_segments);
    put_u8(b, d->vcm_cache_size);
    put_u8(b, d->writes_psiz);
    put_u16(b, 0);
    for (i = 0; i < 16; i++)
        put_u8(b, d->vattr_sizes[i]);
    put_u32(b, d->vpm_input_size);
    put_u32(b, d->vpm_output_size);
    for (i = 0; i < 64; i++)
        put_u32(b, (uint32_t)d->driver_location_map[i]);
}

static void put_fs(struct buf *b, const struct v3d_fs_key *k, const struct v3d_fs_prog_data *d)
{
    unsigned i;

    put_key_common(b, &k->base);
    put_u8(b, k->is_points);
    put_u8(b, k->is_lines);
    put_u8(b, k->line_smoothing);
    put_u8(b, k->point_coord_upper_left);
    put_u8(b, k->msaa);
    put_u8(b, k->sample_alpha_to_coverage);
    put_u8(b, k->sample_alpha_to_one);
    put_u8(b, k->can_earlyz_with_discard);
    put_u8(b, k->software_blend);
    put_u8(b, k->ignore_sample_mask);
    put_u8(b, k->cbufs);
    put_u8(b, k->swap_color_rb);
    put_u8(b, k->f32_color_rb);
    put_u8(b, k->norm_16);
    put_u8(b, k->snorm);
    put_u8(b, k->ucp_enables);
    put_u8(b, k->has_gs);
    put_u8(b, 0);
    put_u16(b, 0);
    for (i = 0; i < 8; i++) {
        put_u32(b, k->color_fmt[i].format);
        put_bytes(b, k->color_fmt[i].swizzle, 4);
    }
    for (i = 0; i < 8; i++) {
        put_u32(b, k->blend[i].rgb_func);
        put_u32(b, k->blend[i].rgb_src_factor);
        put_u32(b, k->blend[i].rgb_dst_factor);
        put_u32(b, k->blend[i].alpha_func);
        put_u32(b, k->blend[i].alpha_src_factor);
        put_u32(b, k->blend[i].alpha_dst_factor);
    }
    put_u32(b, k->logicop_func);
    put_u32(b, k->point_sprite_mask);

    put_prog_data_common(b, &d->base);
    put_u8(b, d->uses_pid);
    put_u8(b, d->num_inputs);
    put_u8(b, d->writes_z);
    put_u8(b, d->writes_z_from_fep);
    put_u8(b, d->disable_ez);
    put_u8(b, d->uses_center_w);
    put_u8(b, d->uses_implicit_point_line_varyings);
    put_u8(b, d->lock_scoreboard_on_first_thrsw);
    put_u8(b, d->force_per_sample_msaa);
    put_u8(b, 0);
    put_u16(b, 0);
    for (i = 0; i < 64; i++)
        put_u8(b, d->input_slots[i].slot_and_component);
    for (i = 0; i < 3; i++)
        put_u32(b, d->flat_shade_flags[i]);
    for (i = 0; i < 3; i++)
        put_u32(b, d->noperspective_flags[i]);
    for (i = 0; i < 3; i++)
        put_u32(b, d->centroid_flags[i]);
}

/* A compute shader (ES 3.1, docs/GLES3_PLAN.md stage D item 1): the common
 * key (Mesa's v3d_update_compiled_cs has no more), then its prog data. */
static void put_cs(struct buf *b, const struct v3d_key *k, const struct v3d_compute_prog_data *d)
{
    put_key_common(b, k);
    put_prog_data_common(b, &d->base);
    put_u32(b, d->shared_size);
    put_u16(b, d->local_size[0]);
    put_u16(b, d->local_size[1]);
    put_u16(b, d->local_size[2]);
    put_u8(b, d->can_use_supergroups);
    put_u8(b, 0);
}

/* ---- keys from their bytes (the draw-time variants: v3dp_key_parse) ---- */

struct rd {
    const uint8_t *p, *end;
    int bad;
};

static unsigned get_u8(struct rd *r)
{
    if (r->p + 1 > r->end) {
        r->bad = 1;
        return 0;
    }
    return *r->p++;
}

static uint32_t get_u32(struct rd *r)
{
    uint32_t v;
    if (r->p + 4 > r->end) {
        r->bad = 1;
        return 0;
    }
    v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    r->p += 4;
    return v;
}

/* The inverse of put_key_common, put_vs's and put_fs's key parts. */
static void get_key_common(struct rd *r, struct v3d_key *k)
{
    k->sampler_is_32b = get_u32(r);
    k->is_last_geometry_stage = get_u8(r);
    k->robust_uniform_access = get_u8(r);
    k->robust_storage_access = get_u8(r);
    k->robust_image_access = get_u8(r);
    k->robust_image_access_2 = get_u8(r);
    k->null_descriptor = get_u8(r);
    r->p += 2;
}

static void get_vs_key(struct rd *r, struct v3d_vs_key *k)
{
    unsigned i;
    get_key_common(r, &k->base);
    k->num_used_outputs = get_u8(r);
    r->p += 3;
    for (i = 0; i < 64; i++)
        k->used_outputs[i].slot_and_component = get_u8(r);
    k->va_swap_rb_mask = get_u32(r);
    k->is_coord = get_u8(r);
    k->per_vertex_point_size = get_u8(r);
    k->clamp_color = get_u8(r);
    r->p += 1;
}

static void get_fs_key(struct rd *r, struct v3d_fs_key *k)
{
    unsigned i, j;
    get_key_common(r, &k->base);
    k->is_points = get_u8(r);
    k->is_lines = get_u8(r);
    k->line_smoothing = get_u8(r);
    k->point_coord_upper_left = get_u8(r);
    k->msaa = get_u8(r);
    k->sample_alpha_to_coverage = get_u8(r);
    k->sample_alpha_to_one = get_u8(r);
    k->can_earlyz_with_discard = get_u8(r);
    k->software_blend = get_u8(r);
    k->ignore_sample_mask = get_u8(r);
    k->cbufs = get_u8(r);
    k->swap_color_rb = get_u8(r);
    k->f32_color_rb = get_u8(r);
    k->norm_16 = get_u8(r);
    k->snorm = get_u8(r);
    k->ucp_enables = get_u8(r);
    k->has_gs = get_u8(r);
    r->p += 3;
    for (i = 0; i < 8; i++) {
        k->color_fmt[i].format = get_u32(r);
        for (j = 0; j < 4; j++)
            k->color_fmt[i].swizzle[j] = get_u8(r);
    }
    for (i = 0; i < 8; i++) {
        k->blend[i].rgb_func = get_u32(r);
        k->blend[i].rgb_src_factor = get_u32(r);
        k->blend[i].rgb_dst_factor = get_u32(r);
        k->blend[i].alpha_func = get_u32(r);
        k->blend[i].alpha_src_factor = get_u32(r);
        k->blend[i].alpha_dst_factor = get_u32(r);
    }
    k->logicop_func = get_u32(r);
    k->point_sprite_mask = get_u32(r);
}

size_t v3dp_key_parse(const uint8_t *b, size_t len, unsigned *variant_stage, struct v3d_vs_key *vs,
                      struct v3d_fs_key *fs)
{
    struct rd r;
    r.p = b;
    r.end = b + len;
    r.bad = 0;
    *variant_stage = get_u32(&r);
    if (*variant_stage == VARIANT_VS || *variant_stage == VARIANT_VS_BIN) {
        memset(vs, 0, sizeof(*vs)); /* padding too: the driver's cache compares keys with memcmp */
        get_vs_key(&r, vs);
        if (vs->is_coord != (*variant_stage == VARIANT_VS_BIN))
            r.bad = 1;
    } else if (*variant_stage == VARIANT_FS) {
        memset(fs, 0, sizeof(*fs));
        get_fs_key(&r, fs);
    } else {
        r.bad = 1;
    }
    return r.bad || r.p > r.end ? 0 : (size_t)(r.p - b);
}

/* The last fragment variant's inputs (v3dp_last_fs_inputs). */
static int last_fs_valid;
static uint8_t last_fs_num_inputs, last_fs_slots[64];

static void variant_hook(mesa_shader_stage stage, const struct v3d_key *key, size_t key_size,
                         const struct v3d_prog_data *prog_data,
                         const uint64_t *qpu_insts, uint32_t qpu_size)
{
    struct buf *b;
    unsigned i;

    if (num_variants == MAX_VARIANTS) {
        fprintf(stderr, "v3dp: more than %d variants\n", MAX_VARIANTS);
        capture_failed = 1;
        return;
    }
    b = &variants[num_variants++];
    memset(b, 0, sizeof(*b));

    if (stage == MESA_SHADER_VERTEX && key_size == sizeof(struct v3d_vs_key)) {
        const struct v3d_vs_key *k = (const struct v3d_vs_key *)key;
        put_u32(b, k->is_coord ? VARIANT_VS_BIN : VARIANT_VS);
        put_vs(b, k, (const struct v3d_vs_prog_data *)prog_data);
    } else if (stage == MESA_SHADER_FRAGMENT && key_size == sizeof(struct v3d_fs_key)) {
        const struct v3d_fs_prog_data *fd = (const struct v3d_fs_prog_data *)prog_data;
        put_u32(b, VARIANT_FS);
        put_fs(b, (const struct v3d_fs_key *)key, fd);
        last_fs_num_inputs = fd->num_inputs;
        for (i = 0; i < 64; i++)
            last_fs_slots[i] = fd->input_slots[i].slot_and_component;
        last_fs_valid = 1;
    } else if (stage == MESA_SHADER_COMPUTE && key_size == sizeof(struct v3d_key)) {
        put_u32(b, VARIANT_CS);
        put_cs(b, key, (const struct v3d_compute_prog_data *)prog_data);
    } else {
        fprintf(stderr, "v3dp: unexpected variant (stage %d, key size %u)\n", (int)stage,
                (unsigned)key_size);
        capture_failed = 1;
        return;
    }

    put_u32(b, prog_data->uniforms.count);
    for (i = 0; i < prog_data->uniforms.count; i++)
        put_u32(b, prog_data->uniforms.contents[i]);
    for (i = 0; i < prog_data->uniforms.count; i++)
        put_u32(b, prog_data->uniforms.data[i]);

    put_u32(b, qpu_size);
    for (i = 0; i < qpu_size / 8; i++)
        put_u64(b, qpu_insts[i]);

    if (b->failed) {
        capture_failed = 1;
        return;
    }
    /* While the hook is set the driver recompiles on every request (patch
     * 0009), so draws that compute the same key report the same variant
     * again. Keep the first; the compile is deterministic. */
    for (i = 0; i + 1 < num_variants; i++) {
        if (variants[i].len == b->len && !memcmp(variants[i].p, b->p, b->len)) {
            buf_free(b);
            num_variants--;
            return;
        }
    }
}

void v3dp_capture_begin(void)
{
    unsigned i;
    for (i = 0; i < num_variants; i++)
        buf_free(&variants[i]);
    num_variants = 0;
    capture_failed = 0;
    last_fs_valid = 0;
    v3d_compiled_variant_hook = variant_hook;
}

void v3dp_capture_end(void)
{
    v3d_compiled_variant_hook = NULL;
}

int v3dp_last_fs_inputs(uint8_t *num_inputs, uint8_t *slots)
{
    if (!last_fs_valid)
        return 0;
    *num_inputs = last_fs_num_inputs;
    memcpy(slots, last_fs_slots, 64);
    return 1;
}

/* ---- program tables ---- */

/* The driver's transform feedback layout for the next program (v3dp_set_tf). */
static struct v3dp_tf tf_layout;
static int tf_set;

void v3dp_set_tf(const struct v3dp_tf *tf)
{
    tf_set = tf != NULL;
    if (tf)
        tf_layout = *tf;
}

/* The program's linked transform feedback varyings, or NULL. */
static const struct gl_transform_feedback_info *tf_info(const struct gl_shader_program *prog)
{
    const struct gl_transform_feedback_info *info =
        prog->last_vert_prog ? prog->last_vert_prog->sh.LinkedTransformFeedback : NULL;
    return info && info->NumVarying > 0 ? info : NULL;
}

/* Offset of s in the string table; the table starts with the empty string. */
static uint32_t str_add(struct buf *strings, const char *s)
{
    uint32_t off;
    if (!s || !*s)
        return 0;
    off = (uint32_t)strings->len;
    put_bytes(strings, s, strlen(s) + 1);
    return off;
}

static int stage_index(mesa_shader_stage stage)
{
    return stage == MESA_SHADER_VERTEX ? 0 : stage == MESA_SHADER_FRAGMENT ? 1 : -1;
}

/* A program's stages in file order: vertex and fragment, or (ES 3.1) compute. */
#define NUM_FILE_STAGES 3
static const mesa_shader_stage stages_in_file[NUM_FILE_STAGES] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT,
                                                                  MESA_SHADER_COMPUTE };

static int put_stage_params(struct buf *b, struct buf *strings, mesa_shader_stage stage,
                            const struct gl_program_parameter_list *pl)
{
    unsigned i, j;
    uint32_t *values = calloc(pl->NumParameterValues + 1, sizeof(uint32_t));

    if (!values)
        return -1;
    for (i = 0; i < pl->NumParameterValues; i++)
        values[i] = pl->ParameterValues[i].u;

    put_u32(b, stage);
    put_u32(b, pl->NumParameterValues);
    put_u32(b, pl->NumParameters);
    for (i = 0; i < pl->NumParameters; i++) {
        const struct gl_program_parameter *p = &pl->Parameters[i];
        unsigned kind;
        switch (p->Type) {
        case PROGRAM_UNIFORM:   kind = 0; break;
        case PROGRAM_CONSTANT:  kind = 1; break;
        case PROGRAM_STATE_VAR: kind = 2; break;
        default:
            fprintf(stderr, "v3dp: parameter %s has register file %d\n",
                    p->Name ? p->Name : "?", (int)p->Type);
            free(values);
            return -1;
        }
        /* The runtime computes state variables before each draw; the
         * template holds 0 there whatever the link left behind. */
        if (kind == 2) {
            unsigned n = p->Padded ? (p->Size + 3u) & ~3u : p->Size;
            for (j = 0; j < n && p->ValueOffset + j < pl->NumParameterValues; j++)
                values[p->ValueOffset + j] = 0;
        }
        put_u32(b, kind);
        put_u32(b, str_add(strings, p->Name));
        put_u32(b, p->ValueOffset);
        put_u32(b, p->Size);
        put_u32(b, p->DataType);
        for (j = 0; j < STATE_LENGTH; j++)
            put_u16(b, kind == 2 ? (uint16_t)p->StateIndexes[j] : 0);
    }
    for (i = 0; i < pl->NumParameterValues; i++)
        put_u32(b, values[i]);
    free(values);
    return 0;
}

static int put_uniforms(struct buf *b, struct buf *strings, struct gl_shader_program *prog, int format2)
{
    const struct gl_shader_program_data *data = prog->data;
    unsigned i, j, s;

    put_u32(b, data->NumUniformStorage);
    for (i = 0; i < data->NumUniformStorage; i++) {
        const struct gl_uniform_storage *u = &data->UniformStorage[i];
        const struct glsl_type *base = glsl_without_array(u->type);
        int sampler = glsl_type_is_sampler(base);
        int image = glsl_type_is_image(base) && format2; /* ES 3.1: flags bit 3, its image index */

        put_u32(b, str_add(strings, u->name.string));
        put_u32(b, u->type->gl_type);
        put_u32(b, u->array_elements);
        put_u32(b, (sampler ? 1u : 0u) | (u->builtin ? 2u : 0u) | (u->hidden ? 4u : 0u) | (image ? 8u : 0u));
        put_u32(b, u->num_driver_storage);
        for (j = 0; j < u->num_driver_storage; j++) {
            const struct gl_uniform_driver_storage *ds = &u->driver_storage[j];
            int found = -1;
            uint32_t offset = 0;
            for (s = 0; s < NUM_FILE_STAGES && found < 0; s++) {
                const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[s]];
                const struct gl_program_parameter_list *pl;
                const union gl_constant_value *pv;
                if (!sh)
                    continue;
                pl = sh->Program->Parameters;
                pv = (const union gl_constant_value *)ds->data;
                if (pv >= pl->ParameterValues &&
                    pv < pl->ParameterValues + pl->NumParameterValues) {
                    found = (int)s;
                    offset = (uint32_t)(pv - pl->ParameterValues);
                }
            }
            if (found < 0) {
                fprintf(stderr, "v3dp: uniform %s: storage outside the stages' constants\n",
                        u->name.string);
                return -1;
            }
            put_u32(b, stages_in_file[found]);
            put_u32(b, offset);
            put_u8(b, ds->element_stride);
            put_u8(b, ds->vector_stride);
            put_u8(b, ds->format);
            put_u8(b, 0);
        }
        /* The vertex and fragment stages' sampler (or image) index; a
         * compute program's compute stage's in the first word (FORMAT.md,
         * format 2). */
        for (s = 0; s < 2; s++) {
            mesa_shader_stage st = prog->_LinkedShaders[MESA_SHADER_COMPUTE] ? (s ? MESA_SHADER_NONE : MESA_SHADER_COMPUTE)
                                                                         : stages_in_file[s];
            put_u32(b, (sampler || image) && st != MESA_SHADER_NONE && u->opaque[st].active ? u->opaque[st].index
                                                                                          : 0xffffffffu);
        }
        /* Format 2: a uniform block member's place in its block (its
         * storage places are none: the block's buffer holds it); a storage
         * block's member (a buffer variable, ES 3.1) the same, flags2 bit 1,
         * its block one of STORAGE_BLOCKS. */
        if (format2) {
            /* an atomic counter (ES 3.1): its buffer (ATOMIC_BUFFERS) in the
             * block word, its offset and array stride, flags2 bit 2 */
            int atomic = glsl_contains_atomic(u->type) && u->atomic_buffer_index >= 0;
            int in_block = u->block_index >= 0 || atomic;
            put_u32(b, !in_block ? 0xffffffffu : atomic ? (uint32_t)u->atomic_buffer_index : (uint32_t)u->block_index);
            put_u32(b, in_block ? (uint32_t)u->offset : 0xffffffffu);
            put_u32(b, in_block ? (uint32_t)u->array_stride : 0xffffffffu);
            put_u32(b, in_block ? (uint32_t)u->matrix_stride : 0xffffffffu);
            put_u32(b, (in_block && u->row_major ? 1u : 0u) | (in_block && u->is_shader_storage ? 2u : 0u) |
                           (atomic ? 4u : 0u));
        }
    }
    return 0;
}

/* Format 2: the program's uniform blocks, then each stage's map from its
 * block numbers (QUNIFORM_UBO_ADDR's unit - 1) to the program's. */
static int put_uniform_blocks(struct buf *b, struct buf *strings, struct gl_shader_program *prog)
{
    const struct gl_shader_program_data *d = prog->data;
    unsigned i, j, n, s;

    put_u32(b, d->NumUniformBlocks);
    for (i = 0; i < d->NumUniformBlocks; i++) {
        const struct gl_uniform_block *ub = &d->UniformBlocks[i];
        put_u32(b, str_add(strings, ub->name.string));
        put_u32(b, ub->Binding);
        put_u32(b, ub->UniformBufferSize);
        put_u32(b, ub->stageref);
        put_u32(b, (uint32_t)ub->_Packing);
        for (j = 0, n = 0; j < d->NumUniformStorage; j++)
            if (d->UniformStorage[j].block_index == (int)i && !d->UniformStorage[j].is_shader_storage)
                n++;
        put_u32(b, n);
        for (j = 0; j < d->NumUniformStorage; j++)
            if (d->UniformStorage[j].block_index == (int)i && !d->UniformStorage[j].is_shader_storage)
                put_u32(b, j);
    }
    for (s = 0, n = 0; s < NUM_FILE_STAGES; s++)
        if (prog->_LinkedShaders[stages_in_file[s]])
            n++;
    put_u32(b, n);
    for (s = 0; s < NUM_FILE_STAGES; s++) {
        const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[s]];
        if (!sh)
            continue;
        put_u32(b, stages_in_file[s]);
        put_u32(b, sh->Program->sh.NumUniformBlocks);
        for (i = 0; i < sh->Program->sh.NumUniformBlocks; i++) {
            const struct gl_uniform_block *ub = sh->Program->sh.UniformBlocks[i];
            if (ub < d->UniformBlocks || ub >= d->UniformBlocks + d->NumUniformBlocks) {
                fprintf(stderr, "v3dp: a stage's uniform block is not the program's\n");
                return -1;
            }
            put_u32(b, (uint32_t)(ub - d->UniformBlocks));
        }
    }
    return 0;
}

/* Format 2, ES 3.1: the program's shader storage blocks, then each stage's
 * map from its storage buffer numbers (QUNIFORM_SSBO_OFFSET's data) to the
 * program's, as UNIFORM_BLOCKS. */
static int put_storage_blocks(struct buf *b, struct buf *strings, struct gl_shader_program *prog)
{
    const struct gl_shader_program_data *d = prog->data;
    unsigned i, j, n, s;

    put_u32(b, d->NumShaderStorageBlocks);
    for (i = 0; i < d->NumShaderStorageBlocks; i++) {
        const struct gl_uniform_block *sb = &d->ShaderStorageBlocks[i];
        put_u32(b, str_add(strings, sb->name.string));
        put_u32(b, sb->Binding);
        put_u32(b, sb->UniformBufferSize);
        put_u32(b, sb->stageref);
        put_u32(b, (uint32_t)sb->_Packing);
        for (j = 0, n = 0; j < d->NumUniformStorage; j++)
            if (d->UniformStorage[j].block_index == (int)i && d->UniformStorage[j].is_shader_storage)
                n++;
        put_u32(b, n);
        /* each member's index in UNIFORMS, then its top-level array's
         * size and stride (GL_TOP_LEVEL_ARRAY_SIZE and _STRIDE) */
        for (j = 0; j < d->NumUniformStorage; j++)
            if (d->UniformStorage[j].block_index == (int)i && d->UniformStorage[j].is_shader_storage) {
                put_u32(b, j);
                put_u32(b, (uint32_t)d->UniformStorage[j].top_level_array_size);
                put_u32(b, (uint32_t)d->UniformStorage[j].top_level_array_stride);
            }
    }
    for (s = 0, n = 0; s < NUM_FILE_STAGES; s++)
        if (prog->_LinkedShaders[stages_in_file[s]])
            n++;
    put_u32(b, n);
    for (s = 0; s < NUM_FILE_STAGES; s++) {
        const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[s]];
        unsigned num = 0, base, k;
        int maxb = -1;
        if (!sh)
            continue;
        /* The stage's atomic counters are storage buffers after its own
         * blocks, at base + binding (nir_lower_atomics_to_ssbo, the state
         * tracker's st_bind_atomics; it raises the NIR's num_ssbos, not the
         * linked program's): those slots are written 0x8000 | binding, up
         * to the highest binding the stage uses. The reader checks that a
         * variant reads no slot past the map. */
        base = sh->Program->info.num_ssbos;
        for (k = 0; k < d->NumAtomicBuffers; k++)
            if (d->AtomicBuffers[k].StageReferences[stages_in_file[s]] && (int)d->AtomicBuffers[k].Binding > maxb)
                maxb = (int)d->AtomicBuffers[k].Binding;
        num = base + (unsigned)(maxb + 1);
        put_u32(b, stages_in_file[s]);
        put_u32(b, num);
        for (i = 0; i < num; i++) {
            const struct gl_uniform_block *sb;
            if (i >= base) {
                put_u32(b, 0x8000u | (i - base));
                continue;
            }
            sb = sh->Program->sh.ShaderStorageBlocks[i];
            if (sb < d->ShaderStorageBlocks || sb >= d->ShaderStorageBlocks + d->NumShaderStorageBlocks) {
                fprintf(stderr, "v3dp: a stage's storage block is not the program's\n");
                return -1;
            }
            put_u32(b, (uint32_t)(sb - d->ShaderStorageBlocks));
        }
    }
    return 0;
}

/* Format 2, ES 3.1: the program's atomic counter buffers (Mesa's
 * AtomicBuffers): binding, minimum size, the stages that reference it (bit
 * per stage), its counters' indices in UNIFORMS. */
/* Format 2: the program's uniform locations as Mesa numbered them (its
 * UniformRemapTable, ranges of locations each pointing at a uniform;
 * explicit locations, ES 3.1, among them): per location the uniform's
 * index in UNIFORMS (an array's elements on consecutive locations),
 * 0xfffffffe for an explicit location of a uniform that is not active
 * (glUniform* there is ignored), 0xffffffff for none. */
#define MAX_UNIFORM_LOCATIONS 4096 /* GL_MAX_UNIFORM_LOCATIONS (Mesa's) */
static int put_uniform_locations(struct buf *b, struct gl_shader_program *prog)
{
    const struct gl_shader_program_data *d = prog->data;
    const struct range_remap *r = prog->UniformRemapTable;
    uint32_t *loc;
    unsigned i, n = 0, k;

    for (i = 0; r && r->sorted_array && i < r->sorted_array_length; i++)
        if (r->sorted_array[i].end + 1 > n)
            n = r->sorted_array[i].end + 1;
    if (n > MAX_UNIFORM_LOCATIONS || !(loc = malloc((n + 1) * sizeof(*loc)))) {
        fprintf(stderr, "v3dp: %u uniform locations\n", n);
        return -1;
    }
    for (k = 0; k < n; k++)
        loc[k] = 0xffffffffu;
    for (i = 0; r && r->sorted_array && i < r->sorted_array_length; i++) {
        const struct range_entry *e = &r->sorted_array[i];
        const struct gl_uniform_storage *u = e->ptr;
        uint32_t v;
        if (!u) {
            continue;
        } else if (u == INACTIVE_UNIFORM_EXPLICIT_LOCATION) {
            v = 0xfffffffeu;
        } else if (u >= d->UniformStorage && u < d->UniformStorage + d->NumUniformStorage) {
            v = (uint32_t)(u - d->UniformStorage);
        } else {
            fprintf(stderr, "v3dp: a uniform location of no uniform of the program\n");
            free(loc);
            return -1;
        }
        for (k = e->start; k <= e->end; k++)
            loc[k] = v;
    }
    put_u32(b, n);
    for (k = 0; k < n; k++)
        put_u32(b, loc[k]);
    free(loc);
    return 0;
}

/* Format 2: the units the program's samplers and images start with
 * (layout(binding), ES 3.1; else 0): per such uniform its index in
 * UNIFORMS, its elements, and each one's unit (Mesa's storage). */
static void put_opaque_units(struct buf *b, struct gl_shader_program *prog)
{
    const struct gl_shader_program_data *d = prog->data;
    unsigned i, e, n = 0, ne;

    for (i = 0; i < d->NumUniformStorage; i++) {
        const struct glsl_type *t = glsl_without_array(d->UniformStorage[i].type);
        if ((glsl_type_is_sampler(t) || glsl_type_is_image(t)) && d->UniformStorage[i].storage)
            n++;
    }
    put_u32(b, n);
    for (i = 0; i < d->NumUniformStorage; i++) {
        const struct gl_uniform_storage *u = &d->UniformStorage[i];
        const struct glsl_type *t = glsl_without_array(u->type);
        if (!(glsl_type_is_sampler(t) || glsl_type_is_image(t)) || !u->storage)
            continue;
        ne = u->array_elements ? u->array_elements : 1;
        put_u32(b, i);
        put_u32(b, ne);
        for (e = 0; e < ne; e++)
            put_u32(b, (uint32_t)u->storage[e].i);
    }
}

static void put_atomic_buffers(struct buf *b, struct gl_shader_program *prog)
{
    const struct gl_shader_program_data *d = prog->data;
    unsigned i, j, stages;

    put_u32(b, d->NumAtomicBuffers);
    for (i = 0; i < d->NumAtomicBuffers; i++) {
        const struct gl_active_atomic_buffer *ab = &d->AtomicBuffers[i];
        put_u32(b, ab->Binding);
        put_u32(b, ab->MinimumSize);
        for (j = 0, stages = 0; j < MESA_SHADER_STAGES; j++)
            if (ab->StageReferences[j])
                stages |= 1u << j;
        put_u32(b, stages);
        put_u32(b, ab->NumUniforms);
        for (j = 0; j < ab->NumUniforms; j++)
            put_u32(b, ab->Uniforms[j]);
    }
}

/* Format 2: the program's inputs and outputs (Mesa's GL_PROGRAM_INPUT and
 * GL_PROGRAM_OUTPUT resources, built-ins included). */
static int put_interface(struct buf *b, struct buf *strings, struct gl_shader_program *prog)
{
    const struct gl_program_resource *res = prog->data->ProgramResourceList;
    unsigned i, n = 0;

    for (i = 0; i < prog->data->NumProgramResourceList; i++)
        if (res[i].Type == GL_PROGRAM_INPUT || res[i].Type == GL_PROGRAM_OUTPUT)
            n++;
    put_u32(b, n);
    for (i = 0; i < prog->data->NumProgramResourceList; i++) {
        const struct gl_shader_variable *var = res[i].Data;
        if (res[i].Type != GL_PROGRAM_INPUT && res[i].Type != GL_PROGRAM_OUTPUT)
            continue;
        put_u32(b, res[i].Type);
        put_u32(b, str_add(strings, var->name.string));
        put_u32(b, glsl_without_array(var->type)->gl_type);
        put_u32(b, glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 0);
        put_u32(b, (uint32_t)var->location);
        put_u8(b, var->interpolation);
        put_u8(b, var->precision);
        put_u8(b, var->explicit_location ? 1 : 0);
        put_u8(b, var->index);
        put_u32(b, res[i].StageReferences);
    }
    return 0;
}

static int put_attributes(struct buf *b, struct buf *strings, struct gl_shader_program *prog, int format2)
{
    const struct gl_linked_shader *vs = prog->_LinkedShaders[MESA_SHADER_VERTEX];
    const struct gl_program_resource *res = prog->data->ProgramResourceList;
    uint64_t inputs_read = vs ? vs->Program->info.inputs_read : 0;
    unsigned i, n = 0;

    for (i = 0; i < prog->data->NumProgramResourceList; i++)
        if (res[i].Type == GL_PROGRAM_INPUT &&
            ((const struct gl_shader_variable *)res[i].Data)->location >= 0)
            n++;

    put_u32(b, n);
    for (i = 0; i < prog->data->NumProgramResourceList; i++) {
        const struct gl_shader_variable *var = res[i].Data;
        unsigned attr;
        if (res[i].Type != GL_PROGRAM_INPUT || var->location < 0)
            continue;
        /* GL resource locations of vertex inputs are generic attribute
         * numbers; the driver's input slot is the number of inputs read
         * below it (nir_recompute_io_bases, st_atom_array.cpp). */
        attr = VERT_ATTRIB_GENERIC0 + (unsigned)var->location;
        /* A matrix's columns are inputs of their own, and one the shader
         * does not read has no slot (mat2(m3) reads two of three): the
         * location word carries which columns are read (FORMAT.md), the
         * slot is that of the first of them. */
        {
            unsigned cols = var->type->matrix_columns > 1 ? var->type->matrix_columns : 1, k, mask = 0;
            int first = -1;
            for (k = 0; k < cols; k++)
                if ((inputs_read >> (attr + k)) & 1) {
                    mask |= 1u << k;
                    if (first < 0)
                        first = (int)k;
                }
            put_u32(b, str_add(strings, var->name.string));
            put_u32(b, var->type->gl_type);
            /* Format 2: bit 30, a location of its own (layout(location)). */
            put_u32(b, (uint32_t)var->location | (mask << 8) | 0x80000000u |
                           (format2 && var->explicit_location ? 0x40000000u : 0u));
            put_u32(b, first >= 0 ? (uint32_t)util_bitcount64(inputs_read & BITFIELD64_MASK(attr + first))
                                  : 0xffffffffu);
        }
    }
    return 0;
}

/* TRANSFORM_FEEDBACK (format 2): the linked varyings and the driver's
 * layout of them. -1 if the driver laid none out for the program. */
static int put_tf(struct buf *b, struct buf *strings, const struct gl_shader_program *prog,
                  const struct gl_transform_feedback_info *info)
{
    unsigned i;
    if (!tf_set) {
        fprintf(stderr, "v3dp: the driver laid out no transform feedback for the program's varyings\n");
        return -1;
    }
    put_u32(b, prog->TransformFeedback.BufferMode);
    put_u32(b, (uint32_t)info->NumVarying);
    for (i = 0; i < (unsigned)info->NumVarying; i++) {
        const struct gl_transform_feedback_varying_info *v = &info->Varyings[i];
        put_u32(b, str_add(strings, v->name.string));
        put_u32(b, v->Type);
        put_u32(b, (uint32_t)v->Size);
        put_u32(b, (uint32_t)v->BufferIndex);
        put_u32(b, (uint32_t)v->Offset);
    }
    for (i = 0; i < 4; i++)
        put_u32(b, tf_layout.stride[i]);
    put_u32(b, tf_layout.num_specs);
    for (i = 0; i < tf_layout.num_specs; i++)
        put_u16(b, tf_layout.specs[i]);
    for (i = 0; i < tf_layout.num_specs; i++)
        put_u16(b, tf_layout.specs_psiz[i]);
    put_pad4(b);
    put_u32(b, tf_layout.num_outputs);
    for (i = 0; i < tf_layout.num_outputs; i++)
        put_u8(b, tf_layout.outputs[i]);
    put_pad4(b);
    return 0;
}

static void put_section(struct buf *out, uint32_t type, const struct buf *payload)
{
    put_u32(out, type);
    put_u32(out, (uint32_t)payload->len);
    put_bytes(out, payload->p, payload->len);
    put_pad4(out);
}

/* ---- .v3dx: a program's draw-time variants (FORMAT.md) ---- */

#define MAX_V3DX_VARIANTS (256 - 3) /* 256 sections: PROGRAM, STRINGS, VARIANT_OF and the variants */

/* The VARIANT payloads of an existing .v3dx of the same program and link
 * layout (its PROGRAM's hash and its VARIANT_OF's CRC), oldest first, into
 * out[] (at most max); their count, 0 when there is no such file. */
static unsigned old_variants(const char *path, const uint8_t *hash, uint32_t layout_crc, unsigned major,
                             struct buf *out, unsigned max)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long n;
    unsigned nsec, i, count = 0, ok = 0;
    size_t o;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = n > 32 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        fclose(f);
        return 0;
    }
    fclose(f);
    if (memcmp(d, "V3DP", 4) || (unsigned)(d[4] | d[5] << 8) != major ||
        crc32_le(d + 32, (size_t)n - 32) != (uint32_t)(d[28] | d[29] << 8 | d[30] << 16 | (uint32_t)d[31] << 24)) {
        free(d);
        return 0;
    }
    nsec = (unsigned)(d[24] | d[25] << 8 | d[26] << 16 | (uint32_t)d[27] << 24);
    for (i = 0, o = 32; i < nsec && o + 8 <= (size_t)n; i++) {
        uint32_t type = (uint32_t)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24);
        uint32_t len = (uint32_t)(d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24);
        const uint8_t *p = d + o + 8;
        if (o + 8 + len > (size_t)n)
            break;
        if (type == SEC_PROGRAM && (len < 32 || memcmp(p, hash, 32)))
            break; /* another program's */
        if (type == SEC_VARIANT_OF)
            ok = len >= 4 && (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) == layout_crc;
        if (type == SEC_VARIANT && ok && count < max) {
            memset(&out[count], 0, sizeof(out[count]));
            put_bytes(&out[count], p, len);
            count++;
        }
        o += 8 + ((len + 3) & ~3u);
    }
    free(d);
    if (!ok) {
        for (i = 0; i < count; i++)
            buf_free(&out[i]);
        count = 0;
    }
    return count;
}

/* Writes the program's file (v3dx 0) or its draw-time variants file (v3dx
 * 1: the captured variants added to those of the existing file, which a
 * temporary file and a rename replace). */
static int write_file(const char *path, struct gl_shader_program *prog, unsigned num_sources,
                      const unsigned *stages, const char *const *sources, int v3dx, uint32_t expect_crc)
{
    /* program = PROGRAM section, body = the sections after STRINGS (STRINGS
     * is complete only once they are written). */
    struct buf program = { 0 }, body = { 0 }, strings = { 0 }, sec = { 0 }, file = { 0 };
    struct mesa_blake3 h;
    blake3_hash hash;
    char commit[12];
    unsigned i, num_sections = 0, stage_mask = 0, nold = 0;
    int rc = -1, format2 = prog->GLSL_Version >= 300, separable = 0;
    size_t layout_end = 0;
    struct buf *old = NULL;
    char tmp[600];
    FILE *f;

    if (capture_failed) {
        fprintf(stderr, "v3dp: variant capture failed\n");
        return -1;
    }
    for (i = 0; i < num_sources; i++) {
        if (stages[i] == V3DP_HASH_XFBV || stages[i] == V3DP_HASH_SEPA)
            format2 = 1;
        if (stages[i] == V3DP_HASH_SEPA)
            separable = 1;
    }
    /* ES 3.1's objects come with stage D (docs/GLES3_PLAN.md §7): storage
     * blocks, atomic counters, compute programs and images. */
    if (prog->data->NumShaderStorageBlocks || prog->data->NumAtomicBuffers ||
        prog->_LinkedShaders[MESA_SHADER_COMPUTE])
        format2 = 1;
    for (i = 0; i < NUM_FILE_STAGES; i++) {
        const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[i]];
        if (sh && sh->Program->info.num_images)
            format2 = 1;
    }
    put_u8(&strings, 0);

    /* PROGRAM */
    _mesa_blake3_init(&h);
    for (i = 0; i < num_sources; i++) {
        struct buf hb = { 0 };
        put_u32(&hb, stages[i]);
        put_u32(&hb, (uint32_t)strlen(sources[i]));
        if (hb.failed)
            goto out;
        _mesa_blake3_update(&h, hb.p, hb.len);
        _mesa_blake3_update(&h, sources[i], strlen(sources[i]));
        buf_free(&hb);
    }
    _mesa_blake3_final(&h, hash);
    for (i = 0; i < NUM_FILE_STAGES; i++)
        if (prog->_LinkedShaders[stages_in_file[i]])
            stage_mask |= 1u << stages_in_file[i];
    put_bytes(&sec, hash, sizeof(hash));
    put_u32(&sec, stage_mask);
    if (format2) /* features (FORMAT.md, format 2) */
        put_u32(&sec, (tf_info(prog) ? V3DP_FEATURE_TF : 0) |
                          (prog->data->NumShaderStorageBlocks || prog->data->NumAtomicBuffers
                               ? V3DP_FEATURE_STORAGE : 0) |
                          (prog->data->NumAtomicBuffers ? V3DP_FEATURE_ATOMIC : 0) |
                          (prog->_LinkedShaders[MESA_SHADER_COMPUTE] ? V3DP_FEATURE_COMPUTE : 0) |
                          (separable ? V3DP_FEATURE_SEPARABLE : 0));
    put_section(&program, SEC_PROGRAM, &sec);
    num_sections++;
    buf_free(&sec);

    /* STAGE_PARAMS */
    for (i = 0; i < NUM_FILE_STAGES; i++) {
        const struct gl_linked_shader *sh = prog->_LinkedShaders[stages_in_file[i]];
        if (!sh)
            continue;
        if (put_stage_params(&sec, &strings, stages_in_file[i], sh->Program->Parameters))
            goto out;
        put_section(&body, SEC_STAGE_PARAMS, &sec);
        num_sections++;
        buf_free(&sec);
    }

    /* UNIFORMS, ATTRIBUTES; format 2: UNIFORM_BLOCKS, INTERFACE */
    if (put_uniforms(&sec, &strings, prog, format2))
        goto out;
    put_section(&body, SEC_UNIFORMS, &sec);
    num_sections++;
    buf_free(&sec);
    if (put_attributes(&sec, &strings, prog, format2))
        goto out;
    put_section(&body, SEC_ATTRIBUTES, &sec);
    num_sections++;
    buf_free(&sec);
    layout_end = body.len; /* STAGE_PARAMS, UNIFORMS, ATTRIBUTES: a .v3dx's VARIANT_OF */
    if (format2) {
        if (put_uniform_blocks(&sec, &strings, prog))
            goto out;
        put_section(&body, SEC_UNIFORM_BLOCKS, &sec);
        num_sections++;
        buf_free(&sec);
        if (put_interface(&sec, &strings, prog))
            goto out;
        put_section(&body, SEC_INTERFACE, &sec);
        num_sections++;
        buf_free(&sec);
        if (tf_info(prog)) {
            if (put_tf(&sec, &strings, prog, tf_info(prog)))
                goto out;
            put_section(&body, SEC_TRANSFORM_FEEDBACK, &sec);
            num_sections++;
            buf_free(&sec);
        }
        if (prog->data->NumShaderStorageBlocks || prog->data->NumAtomicBuffers) {
            if (put_storage_blocks(&sec, &strings, prog))
                goto out;
            put_section(&body, SEC_STORAGE_BLOCKS, &sec);
            num_sections++;
            buf_free(&sec);
        }
        if (prog->data->NumAtomicBuffers) {
            put_atomic_buffers(&sec, prog);
            put_section(&body, SEC_ATOMIC_BUFFERS, &sec);
            num_sections++;
            buf_free(&sec);
        }
        put_opaque_units(&sec, prog);
        put_section(&body, SEC_OPAQUE_UNITS, &sec);
        num_sections++;
        buf_free(&sec);
        if (put_uniform_locations(&sec, prog))
            goto out;
        put_section(&body, SEC_UNIFORM_LOCATIONS, &sec);
        num_sections++;
        buf_free(&sec);
    }

    if (v3dx) {
        /* A .v3dx: PROGRAM, an empty STRINGS, VARIANT_OF, the variants of
         * the existing file (oldest dropped past the limit), then the new
         * ones that are not among them. */
        uint32_t crc = crc32_le(body.p, layout_end);
        unsigned keep, j;
        if (expect_crc && crc != expect_crc) {
            fprintf(stderr, "v3dp: the program's link lays it out otherwise (%08x, not %08x): another "
                            "compiler made its .v3dp\n", (unsigned)crc, (unsigned)expect_crc);
            goto out;
        }
        if (body.failed || !(old = calloc(MAX_V3DX_VARIANTS, sizeof(*old))))
            goto out;
        nold = old_variants(path, hash, crc, format2 ? V3DP_MAJOR2 : V3DP_MAJOR, old, MAX_V3DX_VARIANTS);
        buf_free(&body);
        buf_free(&strings);
        put_u8(&strings, 0);
        put_section(&program, SEC_STRINGS, &strings);
        put_u32(&sec, crc);
        put_section(&program, SEC_VARIANT_OF, &sec);
        buf_free(&sec);
        num_sections = 3;
        for (i = 0; i < num_variants; i++) /* the new ones already there are not new */
            for (j = 0; j < nold; j++)
                if (old[j].len == variants[i].len && !memcmp(old[j].p, variants[i].p, old[j].len)) {
                    buf_free(&variants[i]);
                    break;
                }
        for (i = 0, keep = 0; i < num_variants; i++)
            if (variants[i].len)
                keep++;
        for (j = nold + keep > MAX_V3DX_VARIANTS ? nold + keep - MAX_V3DX_VARIANTS : 0; j < nold; j++) {
            put_section(&program, SEC_VARIANT, &old[j]);
            num_sections++;
        }
        for (i = 0; i < num_variants; i++)
            if (variants[i].len) {
                put_section(&program, SEC_VARIANT, &variants[i]);
                num_sections++;
            }
    } else {
        /* VARIANT, in compile order */
        for (i = 0; i < num_variants; i++) {
            put_section(&body, SEC_VARIANT, &variants[i]);
            num_sections++;
        }
        /* PROGRAM, STRINGS, the rest: everything the header's CRC covers */
        put_section(&program, SEC_STRINGS, &strings);
        num_sections++;
        put_bytes(&program, body.p, body.len);
    }
    if (program.failed || body.failed || strings.failed) {
        fprintf(stderr, "v3dp: out of memory\n");
        goto out;
    }

    /* Header */
    memset(commit, 0, sizeof(commit));
    memcpy(commit, V3DP_MESA_COMMIT,
           strlen(V3DP_MESA_COMMIT) < sizeof(commit) ? strlen(V3DP_MESA_COMMIT) : sizeof(commit));
    put_bytes(&file, "V3DP", 4);
    put_u16(&file, format2 ? V3DP_MAJOR2 : V3DP_MAJOR);
    put_u16(&file, V3DP_MINOR);
    put_u32(&file, V3DP_V3D_VER);
    put_bytes(&file, commit, sizeof(commit));
    put_u32(&file, num_sections);
    put_u32(&file, crc32_le(program.p, program.len));
    put_bytes(&file, program.p, program.len);
    if (file.failed) {
        fprintf(stderr, "v3dp: out of memory\n");
        goto out;
    }

    /* A .v3dx replaces its old self through a temporary file, so a reader
     * never sees half of it. */
    snprintf(tmp, sizeof(tmp), v3dx ? "%s.new" : "%s", path);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "v3dp: cannot create %s\n", tmp);
        goto out;
    }
    if (fwrite(file.p, 1, file.len, f) != file.len) {
        fprintf(stderr, "v3dp: write error on %s\n", tmp);
        fclose(f);
        goto out;
    }
    if (fclose(f)) {
        fprintf(stderr, "v3dp: write error on %s\n", tmp);
        goto out;
    }
    if (v3dx) {
        remove(path); /* rename() does not replace a file on AmigaOS */
        if (rename(tmp, path)) {
            fprintf(stderr, "v3dp: cannot rename %s to %s\n", tmp, path);
            remove(tmp);
            goto out;
        }
    }
    fprintf(stderr, "v3dp: wrote %s (%u bytes, %u variants%s)\n", path, (unsigned)file.len,
            v3dx ? num_sections - 3 : num_variants, format2 ? ", format 2" : "");
    rc = 0;
out:
    buf_free(&program);
    buf_free(&body);
    buf_free(&strings);
    buf_free(&sec);
    buf_free(&file);
    if (old) {
        for (i = 0; i < nold; i++)
            buf_free(&old[i]);
        free(old);
    }
    return rc;
}

int v3dp_write(const char *path, struct gl_shader_program *prog, unsigned num_sources,
               const unsigned *stages, const char *const *sources)
{
    return write_file(path, prog, num_sources, stages, sources, 0, 0);
}

int v3dp_write_variants(const char *path, struct gl_shader_program *prog, unsigned num_sources,
                        const unsigned *stages, const char *const *sources, uint32_t expect_layout_crc)
{
    return write_file(path, prog, num_sources, stages, sources, 1, expect_layout_crc);
}
