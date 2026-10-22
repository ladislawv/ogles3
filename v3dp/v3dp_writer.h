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

/* The "stage" of the program's attribute bindings in the hash (v3dp/FORMAT.md,
 * PROGRAM): a program linked with them (V3DGLSL_LINK_BOUND) hashes them as a
 * third source. "BIND". */
#define V3DP_HASH_BINDINGS 0x42494E44u
/* ES3 link state as pseudo-stages (FORMAT.md, format 2): transform feedback
 * varyings ("XFBV") and a separable program ("SEPA"). A program hashed with
 * either is written in format 2. */
#define V3DP_HASH_XFBV 0x58464256u
#define V3DP_HASH_SEPA 0x53455041u

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
 * and link layout), the oldest dropped past 253. Same arguments. */
int v3dp_write_variants(const char *path, struct gl_shader_program *prog,
                        unsigned num_sources, const unsigned *stages,
                        const char *const *sources);

/* A variant's key from its bytes as the files and ogles2.library have them:
 * a u32 VARIANT stage (0 vertex, 1 binning, 4 fragment), then the key
 * (FORMAT.md's fields). Fills *vs or *fs, zeroed first; the bytes used, or
 * 0 for keys it cannot read. */
struct v3d_vs_key;
struct v3d_fs_key;
size_t v3dp_key_parse(const uint8_t *b, size_t len, unsigned *variant_stage,
                      struct v3d_vs_key *vs, struct v3d_fs_key *fs);

#endif
