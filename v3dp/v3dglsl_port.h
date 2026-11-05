/*
 * The on-target shader compiler's message port (plan section 8.4, stage S2).
 *
 * The compiler is v3dglslc (tools/v3dglslc) started as a resident server:
 * "v3dglslc SERVER" adds the public port V3DGLSL_PORT_NAME and serves
 * requests until it gets V3DGLSL_QUIT. ogles3.library starts it on the
 * first program it has no binary for, and stops it when the library is
 * expunged. Each request carries both sources; the compiler writes the
 * program's .v3dp file (v3dp/FORMAT.md) where the request says, and the
 * library loads it like any other.
 *
 * The two sides are built by different compilers with different C
 * libraries (ogles2: GCC 6 and clib2; the compiler: GCC 16 and libnix):
 * this header uses only Exec types, and all memory belongs to the sender,
 * except a request without a reply port (mn_ReplyPort NULL: a QUIT sent
 * where the sender cannot wait, such as a library expunge), which must be
 * AllocVec'd and which the server frees with FreeVec.
 */
#ifndef V3DGLSL_PORT_H
#define V3DGLSL_PORT_H

/* V3DGLSL_OPS_ONLY: the op codes and key flags alone, for code without
 * Exec (the host compiler's SERVE, tools/compile_bridge/compile_bridge.py's peer). */
#ifndef V3DGLSL_OPS_ONLY
#include <exec/ports.h>
#endif

/* ogles3's own (2026-11-04): ogles2.library 1.x and its v3dglslc serve
 * "v3dglsl compiler"; sharing it, the library that came second sent its
 * programs to the other's compiler (ES 3 shaders failed, programs went to
 * the cache from the wrong compiler), and either library's expunge stopped
 * the other's compiler (QUIT). */
#define V3DGLSL_PORT_NAME "v3dglsl3 compiler"
#define V3DGLSL_VERSION   1

#define V3DGLSL_LINK    1 /* compile vs and fs, link, write the .v3dp to out_path */
#define V3DGLSL_QUIT    2 /* remove the port and exit (queued requests fail) */
#define V3DGLSL_COMPILE 3 /* compile one shader (vs or fs, the other NULL): status and log only */
#define V3DGLSL_LINK32  4 /* as LINK, but write the variants for samplers returning 32 bits (float
                           * and depth textures): the program's .v3dw file. A compiler older than
                           * this answers 3 (bad request). */
#define V3DGLSL_LINK_A2C 6 /* as LINK32, but the alpha-to-coverage fragment variants (Mesa's key
                            * msaa and sample_alpha_to_coverage, patch 0014): the program's .v3da
                            * file, for a 4x MSAA context with GL_SAMPLE_ALPHA_TO_COVERAGE. A
                            * compiler older than this answers 3. */
#define V3DGLSL_LINK_BOUND 5 /* as LINK, glBindAttribLocation's `bindings` applied before the link
                              * (GLES 2.0 2.10.4: aliased attributes, two names bound to one
                              * location, which Mesa's linker must know of); they are part of the
                              * program's hash (v3dp/FORMAT.md). A compiler older than this
                              * answers 3. */
#define V3DGLSL_COMPILE_VARIANT 7 /* link vs and fs (with `bindings` if not NULL), then compile the
                                   * variants of `keys` (each a u32 VARIANT stage and the key's
                                   * bytes, v3dp/FORMAT.md) into the program's .v3dx: out_path with
                                   * ".v3dp" turned into ".v3dx", its variants kept. Nothing is
                                   * written (result 1) if the link's layout CRC is not
                                   * `layout_crc` (another compiler made the program's .v3dp).
                                   * docs/GLES3_PLAN.md stage A item 5. A compiler older than this
                                   * answers 3. */
#define V3DGLSL_LINK_COMPUTE 8 /* ES 3.1: compile `vs` as a compute shader, link it alone, write the
                                * compute program's .v3dp to out_path (fs unused). docs/GLES3_PLAN.md
                                * stage D item 1. A compiler older than this answers 3. */
#define V3DGLSL_COMPILE_COMPUTE 9 /* ES 3.1: as COMPILE, `vs` a compute shader. A compiler older than
                                   * this answers 3. */
#define V3DGLSL_LINK_SEPARABLE 10 /* ES 3.1: as LINK_BOUND (`bindings` may be NULL), the program
                                  * linked with GL_PROGRAM_SEPARABLE; vs or fs may be NULL (a program
                                  * of one stage: no variants then, docs/GLES3_PLAN.md stage D item
                                  * 5). `bindings` may begin with V3DGLSL_SEPA_MARK too (as a
                                  * COMPILE_VARIANT's of a separable program must). A compiler older
                                  * than this answers 3. */
#define V3DGLSL_SEPA_MARK "SEPA\n" /* link state's first line: the program is separable */
#define V3DGLSL_LINK_PIPELINE 11 /* ES 3.1: a program pipeline's variants: `vs` and `fs` each linked
                                  * separable, drawn through a Mesa pipeline (the key states, or
                                  * with `keys` as COMPILE_VARIANT), into the .v3dx beside out_path,
                                  * its VARIANT_OF `layout_crc` (the library composes the image).
                                  * `bindings`: V3DGLSL_PIPE_MARK, the two programs' hashes as 64 hex
                                  * digits each and "\n", then the vertex program's link state; a
                                  * COMPILE_VARIANT whose bindings begin so is a pipeline's too
                                  * (docs/GLES3_PLAN.md stage D item 5). A compiler older than this
                                  * answers 3. */
#define V3DGLSL_PIPE_MARK "PIPE "
#define V3DGLSL_KEY_OUTPUTS_FROM_FS 0x100 /* in a vertex key's stage word: its used outputs are
                                           * the inputs of the fragment variant compiled just
                                           * before it in the same request (Mesa's
                                           * v3d_update_compiled_vs), whatever its bytes say:
                                           * both variants in one request */

#ifndef V3DGLSL_OPS_ONLY
struct V3DGLSLRequest
{
    struct Message msg;   /* mn_ReplyPort: the sender's */
    ULONG version;        /* V3DGLSL_VERSION */
    ULONG op;             /* V3DGLSL_* */
    const char *vs, *fs;  /* GLSL ES sources, NUL-terminated, exactly as the program gave them */
    const char *out_path; /* the .v3dp file to write */
    char *log;            /* compile and link messages, NUL-terminated (may be truncated) */
    ULONG log_size;
    LONG result;          /* reply: 0 done (written); 1 compile or link failed; 2 not written; 3 bad request */
    const char *bindings; /* V3DGLSL_LINK_BOUND only (read for no other op: an older sender's
                           * request ends before it): "<index> <name>\n" lines, sorted by name;
                           * also V3DGLSL_COMPILE_VARIANT's (NULL: none) */
    /* V3DGLSL_COMPILE_VARIANT only (read for no other op): */
    const UBYTE *keys;
    ULONG keys_len;
    ULONG layout_crc;     /* the program's .v3dp's (FORMAT.md, VARIANT_OF) */
};
#endif

#endif
