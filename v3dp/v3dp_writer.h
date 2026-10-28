/*
 * .v3dp writer (FORMAT.md): serializes a linked GLES2 program and every V3D
 * variant the driver compiled for it, field by field in little-endian, so the
 * m68k compiler module and the PC compiler produce the same bytes.
 *
 * Needs Mesa patch 0009 (v3d_compiled_variant_hook).
 */
#ifndef V3DP_WRITER_H
#define V3DP_WRITER_H

#include <stddef.h>
#include <stdint.h>

struct gl_shader_program;

/* Start capturing variants: installs the driver hook and forgets earlier ones. */
void v3dp_capture_begin(void);
/* Stop capturing (removes the hook); the captured variants stay until the next begin. */
void v3dp_capture_end(void);
/* The inputs (num_inputs, input_slots[64]) of the last fragment variant compiled since
 * v3dp_capture_begin: the used outputs of a vertex key that follows it
 * (V3DGLSL_KEY_OUTPUTS_FROM_FS). 0 if none was compiled. */
int v3dp_last_fs_inputs(uint8_t *num_inputs, uint8_t *slots);

/* The "stage" of the program's attribute bindings in the hash (v3dp/FORMAT.md,
 * PROGRAM): a program linked with them (V3DGLSL_LINK_BOUND) hashes them as a
 * third source. "BIND". */
#define V3DP_HASH_BINDINGS 0x42494E44u
/* ES3 link state as pseudo-stages (FORMAT.md, format 2): transform feedback
 * varyings ("XFBV") and a separable program ("SEPA"). A program hashed with
 * either is written in format 2. */
#define V3DP_HASH_XFBV 0x58464256u
#define V3DP_HASH_SEPA 0x53455041u
#define V3DP_HASH_PIPV 0x50495056u /* a pipeline's: its vertex program's hash, 64 hex digits */
#define V3DP_HASH_PIPF 0x50495046u /* and its fragment program's */

/* Format 2's must-understand feature bits (PROGRAM's features word). */
#define V3DP_FEATURE_TF 0x1u /* transform feedback: a TRANSFORM_FEEDBACK section */
#define V3DP_FEATURE_STORAGE 0x2u /* shader storage blocks: a STORAGE_BLOCKS section (ES 3.1) */
#define V3DP_FEATURE_ATOMIC 0x8u  /* atomic counter buffers: an ATOMIC_BUFFERS section (ES 3.1) */
#define V3DP_FEATURE_SEPARABLE 0x10u /* ES 3.1: a separable program; one stage (and no variants) or two */
#define V3DP_FEATURE_COMPUTE 0x4u /* a compute program: its one stage compute, VARIANTs of stage 5 (ES 3.1) */

/* Transform feedback as the V3D driver laid it out for the program's vertex
 * shader (its v3d_uncompiled_shader, read after a draw): written with a
 * program that has transform feedback varyings (format 2,
 * TRANSFORM_FEEDBACK). */
struct v3dp_tf {
    unsigned num_specs;           /* TRANSFORM_FEEDBACK_OUTPUT_DATA_SPECs */
    uint16_t specs[16];           /* packed, as the driver emits them */
    uint16_t specs_psiz[16];      /* the same with a point size written per vertex (points) */
    unsigned num_outputs;         /* the binning shader's outputs for them */
    uint8_t outputs[64];          /* their slot_and_component */
    unsigned stride[4];           /* each buffer's stride in 32-bit words */
};

/* The layout v3dp_write takes for the next program (NULL: none). */
void v3dp_set_tf(const struct v3dp_tf *tf);

/* Writes the program and the captured variants to path: in format 2 for a
 * GLSL ES 3.x program or one with ES3 link state, else in format 1.0.
 * sources[i] is the source of the shader attached i-th, stages[i] its
 * mesa_shader_stage (or a V3DP_HASH_* pseudo-stage: hashed only).
 * Returns 0, or -1 after printing the reason to stderr. */
int v3dp_write(const char *path, struct gl_shader_program *prog,
               unsigned num_sources, const unsigned *stages,
               const char *const *sources);

/* The program's draw-time variants file (.v3dx, FORMAT.md): the captured
 * variants added to those of the file already at path (the same program
 * and link layout), the oldest dropped past 253. Same arguments, and the
 * layout CRC the program's .v3dp has (0: not checked): nothing is written
 * if this link's is another. */
int v3dp_write_variants(const char *path, struct gl_shader_program *prog,
                        unsigned num_sources, const unsigned *stages,
                        const char *const *sources, uint32_t expect_layout_crc);
/* ES 3.1: a program pipeline's variants (captured from draws through a Mesa
 * pipeline of two separable programs) as a .v3dx: its hash the sources'
 * (V3DP_HASH_PIPV and _PIPF: the two programs' hashes), its VARIANT_OF
 * layout_crc (the library composes the pipeline's image and its CRC). */
int v3dp_write_pipeline_variants(const char *path, struct gl_shader_program *vertex_prog,
                                 unsigned num_sources, const unsigned *stages,
                                 const char *const *sources, uint32_t layout_crc);

/* A variant's key from its bytes as the files and ogles2.library have them:
 * a u32 VARIANT stage (0 vertex, 1 binning, 4 fragment), then the key
 * (FORMAT.md's fields). Fills *vs or *fs, zeroed first; the bytes used, or
 * 0 for keys it cannot read. */
struct v3d_vs_key;
struct v3d_fs_key;
size_t v3dp_key_parse(const uint8_t *b, size_t len, unsigned *variant_stage,
                      struct v3d_vs_key *vs, struct v3d_fs_key *fs);

#endif
