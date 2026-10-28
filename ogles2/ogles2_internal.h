#ifndef OGLES3_INTERNAL_H
#define OGLES3_INTERNAL_H
/*
 * ogles3.library internals shared by the library's own files.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>
#include "ogles2_api.h" /* generated: dispatch struct and every ogles2_* prototype */
#include <GLES2/gl2ext.h>
#include <GLES3/gl31.h>       /* the OpenGL ES 3.0 and 3.1 enums (ES3 contexts) */
#include <libraries/ogles3.h> /* the extensions' enums gl2ext.h lacks */
#include "variant_key.h"      /* struct OGLES3VkTargets (a program's last variants' key) */

/* 1.1: glDrawRangeElements appended to the dispatch table. 1.2: the
 * extension entries of AmigaOS 4's ogles2 (program binaries, glPolygonMode,
 * glProvokingVertex, glGetBufferParameterivOES). 1.3: M7's extensions
 * (docs/M7_EXTENSIONS_PLAN.md: debug markers, clip control, draw buffers).
 * glGetString's GL_VERSION (context.c) names the same revision. */
/* ogles3.library 1.0 (2026-10-28): the ogles2.library 1.3 work with OpenGL
 * ES 3.0 and 3.1 (docs/GLES3_PLAN.md), a library of its own beside
 * ogles2.library 1.x. */
#define OGLES3_LIB_VERSION  1
#define OGLES3_LIB_REVISION 0
#define OGLES3_LIB_DATE     "28.10.2026"
/* the release's name (its archives, release/build_release.sh): a release may
   rebuild a library without a new version.revision */
#define OGLES3_RELEASE      "1.0-beta1"

struct OGLES3Library
{
    struct Library lib;
    BPTR segList;
    struct SignalSemaphore lock; /* library-wide state: context list, log */
    struct SignalSemaphore compiler_lock; /* starting the shader compiler (compiler.c) */
    ULONG contexts;                       /* with a GPU, in every program */
    /* Under `lock`: */
    ULONG serial;                         /* the last job serial handed out (job.c): unique across contexts */
    struct OGLES3Context *pend_ctx;       /* a context whose render may still run (job.c), */
    struct Task *pend_task;               /* the task that submitted it, */
    UBYTE pend_frame;                     /* and its v3d_submit_render count */
};

/* Objects shared between contexts (OGLES3_CCT_SHARE_WITH; GLES 2.0 appendix
 * C, plan 13 M6): the name tables of textures, buffers, shaders and programs
 * (with the images their programs run) and renderbuffers. Every context is
 * in a group (alone without the tag). Framebuffers, the default textures
 * (name 0) and every binding stay per context. A deleted object is detached
 * from every member (GL would keep it while another context has it bound).
 * The members may be used from several tasks: every call into a member of
 * a group of two or more runs under the group's semaphore (share.c). */
struct OGLES3Texture;
struct OGLES3Buffer;
struct OGLES3Renderbuffer;
struct OGLES3Image;
struct OGLES3Sampler;
struct OGLES3VaoState;
struct OGLES3Sync;
struct OGLES3QueryState;
struct OGLES3XfbState;
struct OGLES3Variant;
struct OGLES3Program;

#define OGLES3_MAX_XFB_BUFFERS 4 /* GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS (state.c) */

/* An indexed transform feedback binding (glBindBufferBase, glBindBufferRange),
 * a transform feedback object's (xfb.c); size 0: the whole buffer. */
struct OGLES3XfbBinding
{
    struct OGLES3Buffer *b;
    ULONG offset, size;
};
/* A name past OGLES3_NEAR_NAMES and its table slot (share.c). */
struct OGLES3FarName
{
    struct OGLES3FarName *next;
    GLuint name, slot;
};

struct OGLES3Share
{
    ULONG members;                    /* contexts in the group */
    struct OGLES3Context *first;      /* the members, through share_next */
    struct OGLES3Texture **tex;       /* texture.c: by name */
    GLuint ntex;
    struct OGLES3Buffer **buf;        /* buffer.c */
    GLuint nbuf;
    void **objects;                   /* program.c: shaders and programs, by name (0 never used) */
    GLuint nobjects;
    struct OGLES3Image *images;       /* program.c: the program cache, by hash (plan 7.8) */
    struct OGLES3Renderbuffer **rbs;  /* fbo.c */
    GLuint nrbs;
    struct OGLES3FarName *far_tex, *far_buf, *far_rb; /* their tables' names past OGLES3_NEAR_NAMES */
    struct SignalSemaphore lock;      /* every call into a member, while there are two or more */
    struct OGLES3Sampler **smp;       /* texture.c: ES 3.0's sampler objects, by name (glGenSamplers') */
    GLuint nsmp;
    struct OGLES3Sync *syncs;         /* sync.c: ES 3.0's sync objects */
    struct OGLES3Buffer *orphans;     /* buffer.c: deleted buffers vertex array objects still use */
    struct OGLES3Label *labels;       /* debug_khr.c: KHR_debug's object labels */
};

/* glPixelStorei's state for one direction (ES 3.0 3.7.1 and 4.3.1; the
 * row length, skips and image height are ES 3.0's: packing has no image
 * height or skipped images). */
struct OGLES3PixelStore
{
    GLint alignment, row_length, image_height, skip_pixels, skip_rows, skip_images;
    BOOL own;                       /* the library's own images: no pixel buffer (buffer.c) */
};

/* A GL context. Opaque to clients (struct OGLES3Context in the headers). */
#define OGLES3_CONTEXT_MAGIC 0x474c4532UL /* "GLE2" */
#define OGLES3_MAX_VERTEX_ATTRIBS 16
/* Texture units are binding points: a program may use units 0-47 across its
 * stages (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: ES 3.1's minimum, in every
 * context version, docs/GLES3_PLAN.md §2; 32 until 1.3), but a stage has at
 * most 16 samplers, its texture indices 0-15 (GL_MAX_TEXTURE_IMAGE_UNITS, the
 * TMU_CONFIG recipes' index). M7 step 2; plan 7.6. */
#define OGLES3_MAX_TEXTURE_UNITS  48
#define OGLES3_MAX_STAGE_SAMPLERS 16
#define OGLES3_MAX_STAGE_IMAGES 32 /* ES 3.1: GL_MAX_*_IMAGE_UNIFORMS */
#define OGLES3_MAX_VIEWPORT 4096 /* V3D_MAX_FRAMEBUFFER_SIZE on 4.2; GL_MAX_VIEWPORT_DIMS */

/* glEnable capabilities, bits of OGLES3State.caps */
#define OGLES3_CAP_CULL_FACE                (1UL << 0)
#define OGLES3_CAP_BLEND                    (1UL << 1)
#define OGLES3_CAP_DITHER                   (1UL << 2)
#define OGLES3_CAP_STENCIL_TEST             (1UL << 3)
#define OGLES3_CAP_DEPTH_TEST               (1UL << 4)
#define OGLES3_CAP_SCISSOR_TEST             (1UL << 5)
#define OGLES3_CAP_POLYGON_OFFSET_FILL      (1UL << 6)
#define OGLES3_CAP_SAMPLE_ALPHA_TO_COVERAGE (1UL << 7)
#define OGLES3_CAP_SAMPLE_COVERAGE          (1UL << 8)
#define OGLES3_CAP_PRIMITIVE_RESTART       (1UL << 9) /* GL_PRIMITIVE_RESTART_FIXED_INDEX (ES 3.0) */
#define OGLES3_CAP_RASTERIZER_DISCARD      (1UL << 10) /* GL_RASTERIZER_DISCARD (ES 3.0) */
#define OGLES3_CAP_SAMPLE_MASK             (1UL << 11) /* GL_SAMPLE_MASK (ES 3.1) */

/* Fixed-function state (state.c), GLES 2.0 defaults set by ogles2_state_init. */
struct OGLES3State
{
    ULONG caps;                     /* OGLES3_CAP_* bits (glEnable) */
    GLboolean depth_mask;
    GLboolean color_mask[4];
    GLboolean sample_coverage_invert;
    GLfloat clear_color[4];
    GLfloat clear_depth;
    GLfloat line_width;
    GLfloat depth_range[2];         /* near, far */
    GLenum clip_origin, clip_depth; /* glClipControlEXT (M7): GL_LOWER_LEFT_EXT, GL_NEGATIVE_ONE_TO_ONE_EXT */
    GLfloat polygon_offset_factor, polygon_offset_units;
    GLfloat sample_coverage_value;
    GLbitfield sample_mask;         /* ES 3.1's glSampleMaski word 0 (GL_MAX_SAMPLE_MASK_WORDS 1) */
    GLfloat attrib[OGLES3_MAX_VERTEX_ATTRIBS][4]; /* current generic attributes */
    GLenum generate_mipmap_hint;    /* glHint */
    GLenum derivative_hint;         /* glHint, OES_standard_derivatives */
    GLenum polygon_mode;            /* glPolygonMode (AmigaOS 4): GL_FILL, GL_LINE, GL_POINT */
    GLenum provoking_vertex;        /* glProvokingVertex (AmigaOS 4) */
    GLenum cull_face_mode, front_face, depth_func;
    GLenum blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLenum blend_eq_rgb, blend_eq_alpha;
    GLfloat blend_color[4];
    /* stencil, [0] front and [1] back */
    GLenum stencil_func[2], stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
    GLint stencil_ref[2];
    GLuint stencil_value_mask[2], stencil_write_mask[2];
    GLint clear_stencil;
    /* ES 3.0's typed current values (glVertexAttribI4*): the kind of each
     * (0 float in attrib, 1 int, 2 unsigned in attrib_word) */
    UBYTE attrib_kind[OGLES3_MAX_VERTEX_ATTRIBS];
    ULONG attrib_word[OGLES3_MAX_VERTEX_ATTRIBS][4];
};

struct OGLES3Buffer; /* buffer.c */

/* glVertexAttribPointer state, one per generic attribute. */
struct OGLES3Array
{
    GLboolean enabled, normalized;
    GLint size;          /* 1..4 components */
    GLenum type;
    GLsizei stride;      /* as given; 0 = tightly packed */
    const void *pointer; /* with a buffer: the offset into it */
    struct OGLES3Buffer *buffer; /* the GL_ARRAY_BUFFER binding when it was set; NULL: client memory */
    GLuint divisor;      /* ES 3.0's glVertexAttribDivisor: 0 per vertex, n an element per n instances */
    GLboolean integer;   /* ES 3.0's glVertexAttribIPointer: read as integers */
    /* ES 3.1's vertex attribute binding (docs/GLES3_PLAN.md stage D item 5):
     * the attribute reads binding `binding` at `reloffset`; slot i also
     * holds binding i's own state (bbuffer at boffset, bstride exact,
     * bdivisor). buffer, pointer, stride and divisor above are the draw's,
     * made from them (draw.c, vab_refresh); zero_stride: the binding's
     * stride is 0, every vertex reads its first element. ustride is
     * glVertexAttribPointer's stride as given (its query). */
    UBYTE binding;
    GLboolean zero_stride;
    GLuint reloffset;
    struct OGLES3Buffer *bbuffer;
    const void *boffset;
    GLsizei bstride, ustride;
    GLuint bdivisor;
};

/* ---- program images (program.c, v3dp.c) ----
 * What a linked program runs: compiled shaders plus what the driver must
 * know to draw with them, Mesa's prog_data in short. Loaded from .v3dp files
 * (plan section 8.3, v3dp/FORMAT.md) by v3dp.c; one is built in (PiStorm3D's
 * smooth-shaded shaders, builtin_shaders.h), for clears drawn as rectangles. */

/* Uniform recipe types: Mesa's enum quniform_contents at the validated commit
 * (v3dp/FORMAT.md), those the runtime implements; the built-in image's own
 * from 0x100. */
#define OGLES3_U_CONSTANT          0
#define OGLES3_U_UNIFORM           1
#define OGLES3_U_VIEWPORT_X_SCALE  2
#define OGLES3_U_VIEWPORT_Y_SCALE  3
#define OGLES3_U_VIEWPORT_Z_OFFSET 4
#define OGLES3_U_VIEWPORT_Z_SCALE  5
#define OGLES3_U_TMU_CONFIG_P0     6  /* data: texture index << 24 | config bits */
#define OGLES3_U_TMU_CONFIG_P1     7
#define OGLES3_U_IMAGE_TMU_CONFIG_P0 8 /* ES 3.1: data: image index << 24 | config bits */
#define OGLES3_U_TEXTURE_WIDTH     10 /* textureSize (GLSL ES 3.00): the base level's size; data: */
#define OGLES3_U_TEXTURE_HEIGHT    11 /* the texture index */
#define OGLES3_U_TEXTURE_DEPTH     12
#define OGLES3_U_TEXTURE_ARRAY_SIZE 13 /* the layers */
#define OGLES3_U_UBO_ADDR          16 /* data: unit << 24 | byte offset; unit 0 the stage's constant
                                       * buffer, unit i + 1 its uniform block i (format 2) */
#define OGLES3_U_GET_UBO_SIZE      21 /* data: the stage's uniform block index */
#define OGLES3_U_LINE_WIDTH        26
#define OGLES3_U_AA_LINE_WIDTH     27
#define OGLES3_U_IMAGE_WIDTH       22 /* ES 3.1: imageSize; data: the image index */
#define OGLES3_U_IMAGE_HEIGHT      23
#define OGLES3_U_IMAGE_DEPTH       24
#define OGLES3_U_IMAGE_ARRAY_SIZE  25
#define OGLES3_U_SSBO_OFFSET       19 /* ES 3.1: storage buffer data's address; data: the stage's buffer */
#define OGLES3_U_GET_SSBO_SIZE     20 /* its size in bytes */
#define OGLES3_U_NUM_WORK_GROUPS   28 /* compute: the dispatch's work groups; data: the dimension */
#define OGLES3_U_SPILL_OFFSET      31 /* the context's spill area (register spilling, draw.c) */
#define OGLES3_U_SPILL_SIZE_PER_THREAD 32
#define OGLES3_U_SHARED_OFFSET     33 /* compute: the dispatch's shared memory's address */
#define OGLES3_U_BLEND_CONSTANT_R  42 /* to _A, 45 */
#define OGLES3_U_CLIP_Z_COEFF      46 /* Mesa patch 0013 (EXT_clip_control): z lerped towards w */
#define OGLES3_U_PISTORM_VP_WIDTH  0x100 /* PiStorm3D's scale_p: viewport width in pixels */
#define OGLES3_U_PISTORM_VP_HEIGHT 0x101 /* PiStorm3D's scale_p_y */

/* Mesa's gl_state_index values at the validated commit, for STAGE_PARAMS. */
#define OGLES3_STATE_DEPTH_RANGE        39
#define OGLES3_STATE_FB_WPOS_Y_TRANSFORM 63
#define OGLES3_STATE_FB_PNTC_Y_TRANSFORM 64

#define OGLES3_VARIANT_VS 0 /* .v3dp VARIANT stages */
#define OGLES3_VARIANT_CS 1 /* binning (coordinate) shader */
#define OGLES3_VARIANT_FS 4
#define OGLES3_VARIANT_COMPUTE 5 /* ES 3.1's compute shader (docs/GLES3_PLAN.md stage D item 1) */

/* One compiled shader (a .v3dp VARIANT): its key fields the runtime selects
 * on, its prog_data, its code and its uniform recipe. */
struct OGLES3Variant
{
    UBYTE stage;                  /* OGLES3_VARIANT_* */
    UBYTE threads, single_seg;
    /* vertex and binning */
    UBYTE per_vertex_point_size;  /* key */
    UBYTE separate_segments, vcm_cache_size, uses_vid, uses_iid, writes_psiz;
    UBYTE vattr_sizes[16];
    ULONG vpm_input_size, vpm_output_size;
    /* fragment */
    UBYTE is_points, is_lines;    /* key */
    UBYTE num_inputs, writes_z, writes_z_from_fep, disable_ez, uses_center_w;
    UBYTE uses_implicit_point_line_varyings, lock_scoreboard_on_first_thrsw, uses_pid;
    ULONG flat_shade_flags[3], noperspective_flags[3], centroid_flags[3];
    /* compute: the work group's shared memory and size; supergroups allowed */
    ULONG shared_size;
    UWORD local_size[3];
    UBYTE can_use_supergroups;
    ULONG spill_size;
    /* code, little-endian QPU instructions; its offset in the image's code */
    const UBYTE *code;
    ULONG code_bytes, code_offset;
    UWORD nrecipe;
    const ULONG *rtype, *rdata;
    UBYTE uses_ubo;               /* the recipe has OGLES3_U_UBO_ADDR (v3dp.c; 0 in the built-in image) */
    /* v3dp.c, for draw.c's uniform streams (NULL in the built-in image):
     * each entry's constant buffer word (0 for the others), and the
     * entries that are not constant buffer words. */
    const UWORD *uidx;
    const UWORD *special;
    UWORD nspecial;
    UBYTE no_textures;            /* no TMU or UBO entries: its stream depends on no texture */
    UBYTE cbufs;                  /* fragment key: the render targets it writes (bit i: target i;
                                   * 0 in the built-in image: target 0) */
    UBYTE uses_blocks;            /* OGLES3_U_UBO_ADDR of a uniform block (unit 1 or up: format 2) */
    /* Its whole key as the file has it (v3dp/FORMAT.md's field order,
     * little-endian; OGLES3_VS_KEY_BYTES or OGLES3_FS_KEY_BYTES), for the
     * draw-time variants (docs/GLES3_PLAN.md stage A item 5); NULL in the
     * built-in image. */
    const UBYTE *key;
    const UBYTE *input_slots;     /* fragment: prog data's 64 input slots (a vertex key's used outputs) */
    UBYTE uses_storage;           /* the recipe has OGLES3_U_SSBO_OFFSET (ES 3.1; v3dp.c) */
    UBYTE uses_images;            /* the recipe has OGLES3_U_IMAGE_TMU_CONFIG_P0 (ES 3.1) */
};
#define OGLES3_VS_KEY_BYTES 88  /* struct v3d_vs_key as the file writes it */
#define OGLES3_FS_KEY_BYTES 296 /* struct v3d_fs_key */

struct OGLES3ImageAttrib
{
    const char *name;
    GLenum type;          /* GL_FLOAT_VEC3, ... (glGetActiveAttrib) */
    UBYTE slot;           /* the slot of its first column read; 0xff: none read */
    UBYTE columns;        /* 1, or 2-4 for a matrix */
    UBYTE colmask;        /* the columns read, each in the next slot (bit k: column k); 0: from a
                           * compiler that did not say, every column from `slot` on */
    UBYTE location;       /* format 2: layout(location), which glBindAttribLocation cannot move;
                           * 0xff: none (the runtime numbers it) */
};

/* Format 2 (v3dp/FORMAT.md): a uniform block (UNIFORM_BLOCKS). */
struct OGLES3ImageBlock
{
    const char *name;
    ULONG binding;        /* initial binding (layout(binding), or 0) */
    ULONG size;           /* data size in bytes */
    UBYTE stages;         /* bit per mesa stage that references it */
    UBYTE packing;        /* 0 std140, 1 shared, 2 packed, 3 std430 */
    UWORD nmembers;
    const UWORD *members; /* their indices in the image's uniforms */
    const LONG *top_level; /* a storage block's: each member's top-level array size and stride */
};

/* Format 2, ES 3.1: an atomic counter buffer of the program (ATOMIC_BUFFERS). */
struct OGLES3ImageAtomic
{
    ULONG binding;        /* its GL_ATOMIC_COUNTER_BUFFER binding (layout(binding)) */
    ULONG size;           /* the bytes its counters need */
    UBYTE stages;         /* bit per mesa stage that references it */
    UWORD ncounters;
    const UWORD *counters; /* their indices in the image's uniforms */
};

/* Format 2: one input or output of the program (INTERFACE). */
struct OGLES3ImageVar
{
    const char *name;
    GLenum iface;         /* GL_PROGRAM_INPUT, GL_PROGRAM_OUTPUT */
    GLenum type;          /* of one element */
    UWORD array;          /* elements, 0 if not an array */
    WORD location;        /* -1: a built-in */
    UBYTE interpolation, precision, flags, index, stages;
};
/* Attribute names of a program: 16 locations, more names with aliasing
 * (GLES 2.0 2.10.4: two names bound to one location). */
#define OGLES3_MAX_IMAGE_ATTRIBS 64

/* Where glUniform* writes one uniform in one stage's constant buffer. */
struct OGLES3Place
{
    UBYTE stage;          /* 0 vertex, 1 fragment: the constant buffer */
    UBYTE elem_stride;    /* 32-bit words between array elements */
    UBYTE vec_stride;     /* 32-bit words between matrix columns */
    UBYTE int_as_float;   /* Mesa's uniform_int_float format */
    UWORD offset;         /* 32-bit words */
};

#define OGLES3_UNIFORM_SAMPLER 1
#define OGLES3_UNIFORM_BUILTIN 2
#define OGLES3_UNIFORM_HIDDEN  4
#define OGLES3_UNIFORM_BUFFER  8 /* ES 3.1: a storage block's member (GL_BUFFER_VARIABLE), no uniform */
#define OGLES3_UNIFORM_ATOMIC  16 /* ES 3.1: an atomic counter (block: its atomic counter buffer), no location */
#define OGLES3_UNIFORM_IMAGE   32 /* ES 3.1: an image (sampler: its image index), its unit layout(binding)'s */

struct OGLES3ImageUniform
{
    const char *name;     /* arrays without [0] (Mesa's naming) */
    GLenum type;          /* of one element */
    ULONG array;          /* elements, 0 if not an array (past 65535 a buffer variable's only) */
    UBYTE flags;          /* OGLES3_UNIFORM_* */
    UBYTE nplaces;
    const struct OGLES3Place *places;
    ULONG sampler[2];     /* texture index in the vertex and fragment stage, ~0: none */
    /* Format 2: a uniform block member's place in its block (block -1: none). */
    WORD block;
    UBYTE row_major;
    LONG offset, array_stride, matrix_stride;
};

/* A state variable (STAGE_PARAMS kind 2): four values the runtime computes. */
struct OGLES3StateVar
{
    UBYTE stage;          /* 0 vertex, 1 fragment */
    UWORD offset;
    UWORD state;          /* OGLES3_STATE_* */
};

struct v3d_mem;

/* A program's transform feedback (format 2's TRANSFORM_FEEDBACK section;
 * docs/GLES3_PLAN.md stage C item 1): its varyings as linked, and the V3D
 * driver's layout of them for the binning shader. */
struct OGLES3ImageXfbVar
{
    const char *name;
    GLenum type;          /* of one element */
    UWORD size;           /* elements */
    UBYTE buffer;         /* the buffer it is written to */
    UWORD offset;         /* in a vertex's record there, bytes */
};

struct OGLES3ImageXfb
{
    GLenum mode;          /* GL_INTERLEAVED_ATTRIBS or GL_SEPARATE_ATTRIBS */
    UWORD nvaryings;
    const struct OGLES3ImageXfbVar *varyings;
    ULONG stride[4];      /* a vertex's record in each buffer, 32-bit words (0: unused) */
    UBYTE nspecs;
    UWORD specs[16];      /* TRANSFORM_FEEDBACK_OUTPUT_DATA_SPECs, packed */
    UWORD specs_psiz[16]; /* the same for points from a vertex shader that writes gl_PointSize */
};

struct OGLES3Image
{
    const char *name;     /* for the log */
    UBYTE hash[32];       /* the program hash (loaded images) */
    UWORD nvariants;
    const struct OGLES3Variant *variants;
    UBYTE nattribs;
    const struct OGLES3ImageAttrib *attribs;
    UWORD nuniforms;
    const struct OGLES3ImageUniform *uniforms;
    UWORD cbuf_words[2];                  /* constant buffers, vertex and fragment */
    const ULONG *cbuf_template[2];
    UWORD nstatevars;
    const struct OGLES3StateVar *statevars;
    ULONG code_total;                     /* bytes of code, all variants */
    BOOL clip_z_coeff;                    /* its vertex shaders read OGLES3_U_CLIP_Z_COEFF (compiled
                                             with Mesa patch 0013): ZERO_TO_ONE clips (draw.c) */
    struct v3d_mem *code_mem;             /* the code in GPU memory; NULL: built in */
    BOOL code_borrowed;                   /* code_mem and the variants a .v3dx's of it (a pipeline's) */
    APTR blocks;                          /* v3dp.c's allocations, to free */
    UBYTE *binary;                        /* the .v3dp file it was loaded from (AllocVec'd; NULL:
                                             built in): OES_get_program_binary's binary */
    ULONG binary_len;
    /* The variants for samplers returning 32 bits (float and depth
     * textures): the program's .v3dw, loaded or compiled at the first draw
     * that needs them (program.c, ogles2_image_wide); only its fragment
     * variants are used. The sources it is compiled from (NULL: a program
     * from glProgramBinaryOES, which can only find a .v3dw). */
    struct OGLES3Image *wide;
    BOOL wide_tried;
    /* The alpha-to-coverage variants (Mesa's fragment key msaa and
     * sample_alpha_to_coverage): the program's .v3da, the same way, for a
     * draw into a 4x MSAA default framebuffer with GL_SAMPLE_ALPHA_TO_COVERAGE. */
    struct OGLES3Image *a2c;
    BOOL a2c_tried;
    char *vs_src, *fs_src;
    struct OGLES3Image *next;             /* the context's program cache (program.c) */
    /* Format 2 (an OpenGL ES 3 program; v3dp/FORMAT.md): its uniform blocks,
     * each stage's map from its block i (QUNIFORM_UBO_ADDR unit i + 1) to
     * them, its inputs and outputs. */
    BOOL format2;
    UWORD nublocks;
    const struct OGLES3ImageBlock *ublocks;
    UBYTE stage_nblocks[2];               /* vertex, fragment */
    const UWORD *stage_block[2];
    UWORD ninterface;
    const struct OGLES3ImageVar *interface;
    /* Draw-time variants (docs/GLES3_PLAN.md stage A item 5). A program
     * file's layout CRC (its STAGE_PARAMS, UNIFORMS and ATTRIBUTES: a .v3dx
     * of it must carry it); for a .v3dx image, v3dx and its VARIANT_OF. */
    ULONG layout_crc;
    BOOL v3dx;
    ULONG variant_of;
    /* A program's .v3dx images: shipped (PROGDIR:shaders/ or PROGDIR:) and
     * the cache's (compiled here), loaded at the first draw that misses a
     * variant (program.c, ogles2_image_x); its link's bindings, for those
     * compiles (NULL: none). */
    struct OGLES3Image *xs, *xc;
    BOOL xs_tried, xc_tried, x_failed;
    char *bind_src;
    const struct OGLES3ImageXfb *xfb; /* transform feedback varyings (NULL: none) */
    /* ES 3.1 (stage D): a compute program (its one stage in slot 0 of the
     * per-stage arrays: constant buffer, block maps); its shader storage
     * blocks and each stage's map from its storage buffer i
     * (QUNIFORM_SSBO_OFFSET data i) to them, as the uniform blocks'. */
    BOOL compute;
    BOOL separable;                   /* ES 3.1: GL_PROGRAM_SEPARABLE (stage D item 5) */
    UBYTE stages;                     /* PROGRAM's: bit 0 vertex, bit 4 fragment, bit 5 compute */
    UWORD nsblocks;
    const struct OGLES3ImageBlock *sblocks;
    UBYTE stage_nsblocks[2];
    const UWORD *stage_sblock[2];       /* 0x8000 | b: atomic counter buffer binding b (FORMAT.md) */
    UWORD nabuffers;                    /* atomic counter buffers (ES 3.1) */
    const struct OGLES3ImageAtomic *abuffers;
    /* Format 2's UNIFORM_LOCATIONS (explicit locations, ES 3.1): per
     * location its uniform's index, OGLES3_LOC_INACTIVE or OGLES3_LOC_NONE;
     * NULL: the runtime numbers the visible uniforms in order. */
    UWORD nremap;
    const UWORD *remap;
    /* Format 2's OPAQUE_UNITS: nopaque entries, each the uniform's index,
     * its elements, then each one's first unit (layout(binding)). */
    UWORD nopaque;
    const UWORD *opaque_units;
    /* Format 2's INTERFACE_TYPES: per interface entry the identity of the
     * structure it is a member of (0 none; NULL: not in the file) */
    const ULONG *interface_types;
};
#define OGLES3_LOC_NONE     0xFFFF /* a location of no uniform (GL_INVALID_OPERATION) */
#define OGLES3_LOC_INACTIVE 0xFFFE /* an inactive uniform's explicit location (ignored) */

/* GL shader and program objects share one name space (GLES 2.0 section 2.10). */
#define OGLES3_OBJ_SHADER  1
#define OGLES3_OBJ_PROGRAM 2

struct OGLES3Shader
{
    UBYTE kind;           /* OGLES3_OBJ_SHADER */
    BOOL delete_pending;
    GLuint name;
    GLenum type;          /* GL_VERTEX_SHADER, GL_FRAGMENT_SHADER */
    char *source;         /* AllocVec'd, NUL-terminated */
    char *info_log;       /* AllocVec'd or NULL: the compiler's messages (plan 8.4) */
    GLboolean compiled;
    UWORD attached;       /* programs it is attached to */
};

#define OGLES3_MAX_BINDINGS 64 /* glBindAttribLocation names a program keeps (dEQP binds 32) */
/* ES 3.1's program pipeline object (program.c): a separable program for
 * each stage (vertex, fragment, compute; NULL none), the program glUniform*
 * sets when none is in use, and its last validation. */
struct OGLES3Pipeline
{
    GLuint name;
    BOOL bound_once;                  /* an object (glIsProgramPipeline) once bound or used */
    struct OGLES3Program *stage[3];
    struct OGLES3Program *active;
    GLboolean validated;
    char *info_log;                   /* AllocVec'd or NULL */
    /* Its draws' program when the vertex and fragment stages are two
     * programs (program.c's pair_program): their shaders linked as one, the
     * images it was linked from, per location of it the stage and location
     * of the sampler or image it takes its unit from (0xFFFFFFFF none), per
     * uniform block of it the stage program's block (0xFFFF none), and the
     * stage programs' constant buffer versions last copied. */
    struct OGLES3Program *pair;
    const struct OGLES3Image *pair_img[2];
    struct OGLES3Program *pair_src[2];
    ULONG *pair_loc;
    UWORD *pair_blk;
    ULONG pair_ver[2];
};

struct OGLES3Program
{
    UBYTE kind;           /* OGLES3_OBJ_PROGRAM */
    BOOL delete_pending;
    GLuint name;
    struct OGLES3Shader *vs, *fs;
    struct OGLES3Shader *cs;          /* ES 3.1: a compute shader (a program of it alone) */
    GLboolean linked;
    const struct OGLES3Image *image;  /* after a successful link */
    UBYTE attrib_index[OGLES3_MAX_IMAGE_ATTRIBS]; /* image attribute i <- generic attribute */
    ULONG *cbuf[2];                   /* constant buffers, vertex and fragment */
    ULONG *cbuf_le[2];                /* the same in the GPU's byte order, for the draws'
                                         uniform streams: written with ogles2_cbuf_set */
    UWORD nlocations;                 /* uniform locations: one per array element */
    UWORD *loc_uniform, *loc_element; /* location -> uniform, element */
    UBYTE *sampler_unit;              /* per location: a sampler's (an array element's)
                                         texture unit (glUniform1i) */
    UWORD *uniform_loc;               /* per uniform of the image: its first location,
                                         0xFFFF for a hidden one */
    char bind_name[OGLES3_MAX_BINDINGS][32]; /* glBindAttribLocation, applied at link */
    GLuint bind_index[OGLES3_MAX_BINDINGS];
    UBYTE nbindings;
    char *info_log;                   /* AllocVec'd or NULL */
    /* draw.c: its last GL shader state record without the addresses that
     * change per draw, for the variants and code it was made from. */
    APTR rec_tmpl;                    /* AllocVec'd or NULL; freed at unlink */
    const struct OGLES3Variant *rec_var[3];
    ULONG rec_code[3];
    BOOL rec_points;
    /* draw.c: the variants and code addresses for the last primitive mode
     * drawn, and the image's sampler uniforms (0xff: not listed yet). */
    BOOL var_ok;
    GLenum var_mode, var_fsmode;      /* the draw's primitive, the fragment shader's (glPolygonMode) */
    const struct OGLES3Image *var_fim; /* the fragment shader's image: the program's, or its .v3dw */
    UBYTE var_nrt;                    /* the render targets drawn into (EXT_draw_buffers) */
    const struct OGLES3Variant *var[3];
    ULONG var_code[3];
    UBYTE nsamplers;
    UWORD samplers[16];
    /* draw.c: the coordinate and vertex shaders' last uniform streams,
     * reused while the vertex constant buffer and the state they read are
     * the same (cbuf_ver[0], the context's ustate_ver), in the same
     * context's job (a shared program draws in several). */
    ULONG cbuf_ver[2];                /* bumped by every change to cbuf[k] */
    const struct OGLES3Variant *us_var[2];
    ULONG us_gen[2], us_cbuf_ver[2], us_state_ver[2], us_addr[2];
    struct OGLES3Context *us_ctx[2];
    UBYTE nslots;                     /* vertex input slots (0xff: not mapped yet) */
    UBYTE slot_attrib[16];            /* the generic attribute of each (0xff: none) */
    UBYTE var_cbufs;                  /* draw.c: the colour buffers drawn into at the last choice */
    BOOL var_logged;                  /* a draw-time variant miss logged (draw.c) */
    /* An OpenGL ES 3 program's last choice also by these (draw.c's
     * es3_variants): each stage's 32-bit samplers, alpha to coverage. */
    ULONG var_bits[2];
    BOOL var_a2c;
    struct OGLES3VkTargets var_tg;  /* and the render targets' key fields (variant_key.h) */
    /* ES 3.0: each uniform block's buffer binding (glUniformBlockBinding;
     * from the image's at link), one per image block. */
    ULONG *block_binding;
    /* ES 3.0's glTransformFeedbackVaryings, applied at the next link
     * (docs/GLES3_PLAN.md stage C item 1): the buffer mode (0: never set,
     * GL_INTERLEAVED_ATTRIBS) and the names, "name\n" each, in order. */
    GLenum xfb_mode;
    UWORD nxfb;
    char *xfb_names;                  /* AllocVec'd; NULL: none */
    /* ES 3.0's GL_PROGRAM_BINARY_RETRIEVABLE_HINT: glProgramParameteri's
     * value, in effect from the next link or glProgramBinary (as Mesa). The
     * binary is the .v3dp file either way. */
    UBYTE binary_hint, binary_hint_pending;
    UBYTE separable;                  /* ES 3.1's GL_PROGRAM_SEPARABLE, for the next link */
};

struct OGLES3Context;
/* program.c: one word of a constant buffer and its little-endian copy. */
void ogles2_cbuf_set(struct OGLES3Program *p, int stage, ULONG at, ULONG w);
/* program.c: a program on the built-in image, outside the name table
 * (draw.c's clears); its storage freed with ogles2_program_builtin_free. */
BOOL ogles2_program_builtin(struct OGLES3Program *p);
BOOL ogles2_program_image(struct OGLES3Program *p, const struct OGLES3Image *im);
void ogles2_program_builtin_free(struct OGLES3Program *p);

/* v3dp.c: a .v3dp file's image, or NULL with the reason in why[]. */
struct OGLES3Image *ogles2_v3dp_load(const UBYTE *file, ULONG len, char *why, ULONG whylen);
/* ES 3.1: a program pipeline's image, a's vertex stage and b's fragment
 * stage, its hash and its compiles' link state line (v3dp.c). */
struct OGLES3Image *ogles2_v3dp_compose(const struct OGLES3Image *a, const struct OGLES3Image *b,
                                        const UBYTE hash[32], const char *pipe_line);
void ogles2_image_free(struct OGLES3Image *im, struct OGLES3Context *ctx);
/* The image's variant for `stage` and a draw of primitive `mode` (GL_POINTS ...). */
const struct OGLES3Variant *ogles2_image_variant(const struct OGLES3Image *im, UBYTE stage, UBYTE cbufs,
                                                 GLenum mode);
/* program.c: the image's 32-bit-sampler variants (im->wide), found or
 * compiled the first time; NULL if there are none (logged once). */
const struct OGLES3Image *ogles2_image_wide(struct OGLES3Image *im);
/* program.c: its alpha-to-coverage variants (im->a2c), the same way. */
const struct OGLES3Image *ogles2_image_a2c(struct OGLES3Image *im);
/* program.c: the same files as shipped or cached, never compiled: an OpenGL
 * ES 3 program's draws compile the variants they need by key (draw.c). */
const struct OGLES3Image *ogles2_image_wide_file(struct OGLES3Image *im);
const struct OGLES3Image *ogles2_image_a2c_file(struct OGLES3Image *im);

/* The default framebuffer's surface (display.c). */
struct OGLES3Display
{
    struct Screen *screen;          /* fullscreen: ours; window: the window's */
    struct Window *window;          /* window mode: the window we ClipBlit into */
    struct Window *input_window;    /* fullscreen: our backdrop window */
    struct BitMap *bitmap;          /* what the GPU renders into */
    struct RastPort rastport;       /* window mode: ClipBlit source over bitmap */
    UWORD width, height;
    BOOL own_screen, own_window, own_bitmap, vsync;
    BOOL direct;                    /* window mode, the window its private screen's whole size:
                                     * bitmap is the screen's, no copy (display.c) */
    int rb_swap;                    /* 1: BGRA32 target, 0: RGBA32 */
    APTR lock;                      /* cybergraphics bitmap lock, one frame long */
    ULONG bprow, base;              /* valid while locked */
    /* Vsync (display.c): the task raised from the wait for the refresh to
     * its frame's submission, and its own priority. */
    struct Task *boosted;
    BYTE boost_pri;
};

struct V3DContext; /* v3dcore */

/* ---- framebuffer objects (fbo.c) ----
 * One image a job's tile buffer loads from and stores to (Mesa's
 * load_general/store_general fields). */
struct OGLES3Surface
{
    ULONG addr;            /* the image's first byte */
    ULONG bytes;           /* its extent, for the CPU cache */
    UBYTE mem_format;      /* V3D_MEMORY_FORMAT_* (the level's own tiling) */
    UBYTE format;          /* V3D_OUTPUT_IMAGE_FORMAT_* */
    UBYTE force_alpha1;    /* load: an RGB format */
    UBYTE cpp;             /* bytes per pixel of the image */
    ULONG ub;              /* UIF: padded height in UIF blocks; else 0 */
};

/* A colour render target's format (texture.c, ogles2_texture_rt; GLES3
 * plan stage B item 10): what the tile buffer, the store, clears, reads
 * and the fragment key need of it (Mesa's v3dx_format_table.c). */
struct OGLES3RtFormat
{
    UBYTE out;             /* V3D_OUTPUT_IMAGE_FORMAT_* */
    UBYTE type;            /* V3D_INTERNAL_TYPE_* in the tile buffer */
    UBYTE bpp;             /* V3D_INTERNAL_BPP_* */
    UBYTE clamp;           /* V3D_RENDER_TARGET_CLAMP_* */
    UBYTE kind;            /* OGLES3_RT_* */
    UBYTE comps;           /* its channels, 1 to 4 */
    UBYTE bits;            /* bits of its red channel */
    UBYTE no_alpha;        /* its alpha reads 1 (Mesa's blend_dst_alpha_one) */
    UBYTE swz[4];          /* PIPE_SWIZZLE_*: the fragment key's color_fmt */
    ULONG pipe;            /* Mesa's enum pipe_format: the same */
};
#define OGLES3_RT_NONE  0
#define OGLES3_RT_UNORM 1  /* fixed point and sRGB: blended in the tile buffer */
#define OGLES3_RT_F16   2  /* half and small floats: blended in the tile buffer */
#define OGLES3_RT_F32   3  /* 32-bit floats: written as such, blended in the shader */
#define OGLES3_RT_INT   4  /* signed integers: written as such, never blended */
#define OGLES3_RT_UINT  5  /* unsigned integers: the same */

/* What a framebuffer object's job renders into (ogles2_fbo_target). */
struct OGLES3Texture;
/* EXT_draw_buffers (M7 step 5): colour attachments 0..3, each a render
 * target of the tile buffer. */
#define OGLES3_MAX_DRAW_BUFFERS 4

struct OGLES3Target
{
    UWORD w, h;
    UBYTE colors;          /* bit i: colour attachment i is drawn (a draw buffer) and has an image */
    UBYTE nrt;             /* the tile buffer's render targets: the highest drawn + 1 (1 with none) */
    UBYTE tile_w, tile_h;  /* the tile size nrt and bpp give (Mesa's v3d_choose_tile_size) */
    UBYTE bpp;             /* V3D_INTERNAL_BPP_*: the widest drawn target's */
    BOOL msaa;             /* multisampled renderbuffers: 4x, every image with all four samples */
    BOOL has_z, has_s, packed_zs; /* packed: one D24S8 image for both */
    struct OGLES3Surface color[OGLES3_MAX_DRAW_BUFFERS], z, s;
    struct OGLES3RtFormat rt[OGLES3_MAX_DRAW_BUFFERS]; /* the drawn targets' formats */
    UBYTE depth_type;      /* V3D_INTERNAL_TYPE_DEPTH* for the tile buffer */
    ULONG ta_addr, ta_size, ts_addr; /* own tile pools, 4096-aligned; 0: the context's fit */
};

struct OGLES3Context
{
    ULONG magic;
    /* At offset 4: the dispatch table's wrappers test it in assembly
     * (gen/gen_ogles2.py, share.c): the group's semaphore while it has two
     * members or more, else 0. */
    struct SignalSemaphore *share_lock;
    GLenum error;        /* first error since the last glGetError (sticky) */
    ULONG width, height; /* default framebuffer size; 0 without a display */
    struct OGLES3State state;
    struct OGLES3Display display;   /* zeroed for a state-only (test) context */
    struct V3DContext *v3d;         /* GPU pools and control lists; NULL state-only */
    UBYTE depth_bits, stencil_bits; /* of the default framebuffer */
    UBYTE depth_format;             /* OGLES3_DEPTH_*: the tile buffer's depth type */
    BOOL msaa;                      /* 4x MSAA on the default framebuffer (OGLES3_CCT_PISTORM_MSAA) */
    UBYTE es_version;               /* OGLES3_CCT_PISTORM_ES_VERSION: 20, 30 or 31 (ogles2_es3, ogles2_es31) */
    UBYTE job_clear_stencil;        /* the open job's stencil clear value (job.c) */
    /* The open job's colour clear values, a target each: four floats' bits,
     * or (is_int, glClearBufferiv/uiv) four integers; packed per target
     * when the job renders (job.c). */
    struct { ULONG v[4]; UBYTE is_int; } job_clear_rt[4];
    UBYTE job_clear_targets;        /* the targets the open job clears (glClear: all) */
    UBYTE copy_logged;              /* glCopyTex* on the GPU: 1 from the default framebuffer, 2 from
                                     * an object, logged (copy.c) */
    GLint viewport[4];              /* x, y, width, height */
    GLint scissor[4];
    ULONG frames, frames_dropped;   /* statistics, logged at destroy */
    /* Timing (microseconds, ogles2_clock_us; logged at destroy): per job the
     * CPU's part (job begun to flush) and the waits for binning and render;
     * per frame the time between swaps and the presentation. */
    ULONG t_job_begin, t_last_swap;
    double sum_build, sum_bin, sum_render, sum_present, sum_frame;
    double sum_list, sum_pre, sum_post; /* the flush's own: render list, cache before, after */
    ULONG n_jobs, n_swaps;
    ULONG n_renames;                /* textures renamed (texture.c) */
    ULONG n_draws;                  /* draws issued (draw.c) */
    double sum_draw, sum_bufdata, sum_teximage; /* microseconds in draws, buffer data, texture images */
    double p_draw, p_bufdata, p_teximage;
    /* Probe P: microseconds in each stage of a draw (draw.c's DRAW_STAGES). */
    ULONG t_stage;
    double stage[8], p_stage[8];
    double n_uwords, n_vbytes, n_indices; /* probe P: uniform stream words, vertex bytes, indices copied */
    /* draw.c: the state the uniform recipes read besides the constant
     * buffers, as at the last draw, and its version. */
    ULONG ustate[7], ustate_ver;
    /* Bumped by every change to the state draw.c's emit_state reads (the
     * setters in state.c, glViewport and glScissor, glClear's override):
     * a draw with the same version as the last skips emit_state. */
    ULONG state_ver;
    ULONG p_draws;
    ULONG n_deletes;                /* glDelete* calls (probe T) */
    double p_frame, p_build, p_render, p_list, p_pre; /* the sums at the last probe P report */
    ULONG p_jobs, p_renames;
    ULONG probe;                    /* OGLES3_PROBE_*, from ENV:OGLES3/Probe (context.c) */
    ULONG probe_addr;               /* OGLES3_PROBE_ADDR's buffer (4 KB, the test's) */
    struct OGLES3Array array[OGLES3_MAX_VERTEX_ATTRIBS];
    struct OGLES3Share *share;      /* the objects (alone in the group without OGLES3_CCT_SHARE_WITH) */
    struct OGLES3Context *share_next; /* the group's next member */
    struct OGLES3Program *program;  /* glUseProgram */
    /* ES 3.1's program pipelines (stage D item 5): the context's own (not
     * shared), by name; the bound one (NULL: none). */
    struct OGLES3Pipeline **pipes;
    GLuint npipes;
    struct OGLES3Pipeline *pipe;
    struct OGLES3Draw *draw;        /* draw.c's per-context state, NULL until the first draw */
    struct OGLES3Compute *compute;  /* compute.c's, NULL until the first dispatch (ES 3.1) */
    struct OGLES3TexState *tex;     /* texture.c's, NULL until first used */
    struct OGLES3PixelStore unpack, pack; /* glPixelStorei */
    ULONG job_serial;               /* the open job's, unique across contexts (job.c); objects note the
                                     * last job that used them */
    struct OGLES3BufState *buf;     /* buffer.c's, NULL until first used */
    BOOL debug_shader_log;          /* OGLES3_CCT_DEBUG_SHADER_LOG: link logs describe the variants */
    ULONG chained_blocks;           /* binning-list blocks added (job.c); the first is logged */
    void *zs_save;                  /* job.c: memory for depth/stencil across a mid-frame flush */
    BOOL zs_saved;                  /* the next job of this frame loads depth/stencil from it */
    BOOL zs_logged;
    /* Framebuffer objects (fbo.c, plan 7.7). width/height/depth_bits/
     * stencil_bits/depth_format above are the default framebuffer's; the
     * fb_* fields describe the framebuffer drawn into now (ogles2_fb_update). */
    struct OGLES3FboState *fbo;     /* fbo.c's, NULL until first used */
    struct OGLES3Framebuffer *draw_fb; /* GL_FRAMEBUFFER (ES 3.0: GL_DRAW_FRAMEBUFFER) binding; NULL: the default */
    struct OGLES3Framebuffer *read_fb; /* ES 3.0's GL_READ_FRAMEBUFFER binding (GLES2: draw_fb's) */
    struct OGLES3Framebuffer *job_fb;  /* what the open job renders into */
    BOOL job_msaa;                  /* the open job is 4x MSAA (the default framebuffer's, or multisampled renderbuffers', job.c) */
    UWORD fb_w, fb_h;
    BOOL fb_flip;                   /* the default framebuffer: drawn upside down (plan 7.11) */
    UBYTE fb_depth_bits, fb_stencil_bits, fb_depth_format;
    UBYTE fb_draw_mask;             /* EXT_draw_buffers: bit i, output i is drawn into an image */
    UBYTE fb_nrt;                   /* the tile buffer's render targets for it: 1 to 4 */
    struct OGLES3RtFormat fb_rt[OGLES3_MAX_DRAW_BUFFERS]; /* the drawn targets' formats (kind NONE: none) */
    UBYTE fb_soft_blend;            /* a drawn target the tile buffer cannot blend: the shader blends (Mesa) */
    UBYTE draw_rts;                 /* the draw buffers a draw writes (ogles2_draw_clear: those it clears) */
    GLenum default_draw_buffer;     /* glDrawBuffersEXT on the default framebuffer: GL_BACK, GL_NONE */
    /* M6 overlap (job.c): a render submitted and not waited for yet. The
     * submission lock went at the submission; the next submitter waits for
     * the render on the GPU (take_gpu) and, from another task, marks it
     * done; ogles2_job_finish does the rest. */
    BOOL pend;
    BOOL pend_done;                 /* another task saw it finish (under OGLES3Library.lock) */
    UBYTE pend_frame;               /* v3d_submit_render's count */
    ULONG pend_serial;              /* its job_serial */
    struct OGLES3Framebuffer *pend_fb; /* NULL: the default framebuffer */
    struct OGLES3Target pend_tg;
    UBYTE marker_depth;             /* EXT_debug_marker: groups pushed (debug.c) */
    struct OGLES3Debug *dbg;        /* KHR_debug's state (debug_khr.c), NULL until first used */
    UBYTE gles2_ext;                /* a GLES2 extension's entry is calling its ES 3.0 one (gles2_ext.c) */
    UBYTE job_discard;              /* EXT_discard_framebuffer: OGLES3_DISCARD_* the open job does not store */
    UBYTE job_noload;               /* and those it does not load (discarded before it began) */
    struct OGLES3VaoState *vao;     /* vao.c's (ES 3.0's vertex array objects), NULL until first used */
    struct OGLES3QueryState *query; /* query.c's (ES 3.0's query objects), NULL until first used */
    ULONG oq_addr;                  /* the active occlusion query's counter (0: none): job.c points new jobs at it */
    struct OGLES3XfbState *xfb;     /* xfb.c's (ES 3.0's transform feedback objects), NULL until first used */
    BOOL job_xfb;                   /* the open job's binning list fed back: its end turns that off (xfb.c) */
    BOOL job_storage;               /* a draw of the open job stores into storage buffers (ES 3.1, draw.c) */
};

#define OGLES3_DISCARD_COLOR   1
#define OGLES3_DISCARD_DEPTH   2
#define OGLES3_DISCARD_STENCIL 4


/* EXT_blend_minmax (gl2ext.h) */
#ifndef GL_MIN_EXT
#define GL_MIN_EXT 0x8007
#define GL_MAX_EXT 0x8008
#endif

#define OGLES3_DEPTH_16  0
#define OGLES3_DEPTH_24  1 /* with 8 stencil bits */
#define OGLES3_DEPTH_32F 2

/* Hardware probes for the A1200 tests, one letter each in ENV:OGLES3/Probe.
 * Not for applications: they change what the library does. */
#define OGLES3_PROBE_STORE_DITHER 1UL /* D: dithered RGBA8 store */
#define OGLES3_PROBE_LOG_CLEAR   2UL  /* C: log the packed clear colour of each job */
/* The M1 hardware probes (plan section 9, M1): */
/* 4UL was F (GL_FIXED as the hardware's fixed type): it works and is the normal path now. */
#define OGLES3_PROBE_FLIP_Y     8UL  /* Y: colour stored and loaded with Flip Y */
#define OGLES3_PROBE_SEMAPHORE 16UL  /* S: render queued behind binning (semaphore packets) */
#define OGLES3_PROBE_ADDR      32UL  /* A: positions copied to ENV:OGLES3/ProbeAddr (hex) */
/* M6 diagnosis (a program that hangs or slows down): */
#define OGLES3_PROBE_TRACE     64UL  /* T: log every glDelete* call and each step of aglDestroyContext */
#define OGLES3_PROBE_PERIODIC 128UL  /* P: every 256 swaps, the timing, GPU memory and renames since the last */
#define OGLES3_PROBE_CPU_MIPMAP 256UL /* M: glGenerateMipmap on the CPU, not the TFU (texture.c) */
#define OGLES3_PROBE_CPU_UPLOAD 512UL /* U: texture uploads tiled by the CPU, not the TFU (texture.c) */
#define OGLES3_PROBE_CPU_COPY 1024UL /* K: glCopyTex* through the CPU, not drawn by the GPU (copy.c) */
#define OGLES3_PROBE_NO_ZS_SAVE 2048UL /* Z: depth and stencil not kept across a mid-frame flush (job.c) */

void ogles2_state_init(struct OGLES3Context *ctx);
BOOL ogles2_state_dither(struct OGLES3Context *ctx); /* GL_DITHER enabled */

/* ---- display (display.c) ---- */
BOOL ogles2_display_libs_open(void);
void ogles2_display_libs_close(void);
BOOL ogles2_display_open(struct OGLES3Display *d, struct TagItem *tags, ULONG *why);
void ogles2_display_close(struct OGLES3Display *d);
BOOL ogles2_display_set_window(struct OGLES3Display *d, struct Window *w);
void ogles2_display_drop(struct OGLES3Display *d);
BOOL ogles2_display_set_bitmap(struct OGLES3Display *d, struct BitMap *bitmap);
/* Locked: the CPU cache around the GPU's write of the bitmap (before: clean;
 * after: invalidate), where the CPU reads it. */
void ogles2_display_dma(struct OGLES3Display *d, BOOL after);
BOOL ogles2_display_lock(struct OGLES3Display *d);
void ogles2_display_unlock(struct OGLES3Display *d);
void ogles2_display_present(struct OGLES3Display *d);
/* aglSetParams2's VSYNC. */
void ogles2_display_set_vsync(struct OGLES3Display *d, BOOL on);
/* Vsync: the wait for the display's refresh before the frame goes out
 * (raising the task's priority), and the end of it after the submission. */
void ogles2_display_wait_refresh(struct OGLES3Display *d);
void ogles2_display_wait_done(struct OGLES3Display *d);
/* glReadPixels: w x h pixels from (x, y) (GL's origin, the bottom-left),
 * clipped by the caller, as RGBA bytes, bottom row first, `stride` apart. */
void ogles2_display_read(struct OGLES3Display *d, UWORD x, UWORD y, UWORD w, UWORD h, UBYTE *dst,
                         ULONG stride);

/* ---- programs (program.c) and drawing (draw.c) ---- */
void *ogles2_object(struct OGLES3Context *ctx, GLuint name, UBYTE kind); /* NULL if not that kind */
void ogles2_objects_free(struct OGLES3Context *ctx); /* aglDestroyContext: programs, shaders, cache */
void ogles2_draw_free(struct OGLES3Context *ctx);    /* aglDestroyContext, after the last job */
void ogles2_log_hold(BOOL on); /* TRUE: no log file written until the matching FALSE (inside a bitmap lock) */
void ogles2_compute_free(struct OGLES3Context *ctx); /* the same */
const struct OGLES3Image *ogles2_builtin_image(void);
/* texture.c */
void ogles2_textures_free(struct OGLES3Context *ctx);
/* for fbo.c */
struct OGLES3Texture *ogles2_texture_get(struct OGLES3Context *ctx, GLuint name); /* NULL: none */
GLuint ogles2_texture_name(const struct OGLES3Texture *t);
GLenum ogles2_texture_target(const struct OGLES3Texture *t); /* 0 until first bound */
/* An attachment point refers to it, or no longer does (a deleted one that
 * was attached goes with its last attachment; `now`: at aglDestroyContext). */
void ogles2_texture_attach(struct OGLES3Texture *t);
void ogles2_texture_detach(struct OGLES3Context *ctx, struct OGLES3Texture *t, BOOL now);
/* The image of face k, level l: FALSE if not defined. */
BOOL ogles2_texture_image(const struct OGLES3Texture *t, int k, int l, UWORD *w, UWORD *h,
                          GLenum *format, GLenum *type);
/* Colour-renderable (plan 7.7; ES 3.0's formats, stage B item 10), and the
 * image's format as a render target. */
BOOL ogles2_texture_renderable(struct OGLES3Texture *t, int k, int l);
BOOL ogles2_texture_rt(struct OGLES3Texture *t, int k, int l, struct OGLES3RtFormat *rf);
UWORD ogles2_float_to_half(GLfloat f); /* IEEE half precision, by the bits (texture.c) */
/* glCopyTex* on the GPU (copy.c): the default framebuffer's region tiled by
 * the TFU into a scratch texture (0: could not); unit 0's 2D texture swapped
 * for the copy's draw; a framebuffer's colour texture sampled level 0,
 * nearest and clamped for it, and its parameters put back. */
struct OGLES3Texture *ogles2_texture_scratch(struct OGLES3Context *ctx, UWORD w, UWORD h, ULONG src, ULONG stride,
                                             BOOL bgra);
struct OGLES3Texture *ogles2_texture_unit0(struct OGLES3Context *ctx, struct OGLES3Texture *t);
/* The parameters a sampler object holds (ES 3.0 3.8.2), a texture's own
 * too: a sampler object bound to a unit is sampled with instead of its
 * texture's (texture.c). */
struct OGLES3SamplerParams
{
    GLenum min_filter, mag_filter, wrap_s, wrap_t, wrap_r;
    GLfloat max_aniso;              /* EXT_texture_filter_anisotropic: 1 = off */
    GLfloat min_lod, max_lod;       /* SGIS_texture_lod */
    GLenum compare_mode, compare_func; /* ES 3.0: GL_TEXTURE_COMPARE_MODE and _FUNC */
};
struct OGLES3TexParams
{
    struct OGLES3SamplerParams s;
    GLint base_level, max_level;
    GLfloat lod_bias;
    GLenum gl_swizzle[4];
};
void ogles2_texture_params_copy(struct OGLES3Texture *t, struct OGLES3TexParams *saved);
void ogles2_texture_params_restore(struct OGLES3Texture *t, const struct OGLES3TexParams *saved);
BOOL ogles2_copy_gpu(struct OGLES3Context *ctx, struct OGLES3Texture *t, int k, int level, GLint xoffset,
                     GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
BOOL ogles2_draw_copy(struct OGLES3Context *ctx, struct OGLES3Texture *src, const GLfloat *tc, GLint x, GLint y,
                      GLsizei w, GLsizei h);
struct OGLES3Framebuffer;
struct OGLES3Framebuffer *ogles2_fbo_copy_target(struct OGLES3Context *ctx, struct OGLES3Texture *t, int face,
                                                 int level);
BOOL ogles2_fbo_color_image(struct OGLES3Context *ctx, struct OGLES3Texture **t, int *face, int *level);
BOOL ogles2_fbo_default_read_none(struct OGLES3Context *ctx);
/* The image as a render target, its GPU storage made first; FALSE: none. */
BOOL ogles2_texture_surface(struct OGLES3Context *ctx, struct OGLES3Texture *t, int k, int l,
                            struct OGLES3Surface *s);
void ogles2_texture_rendered(struct OGLES3Texture *t, int k, int l); /* the GPU's copy is newer */
/* ES 3.1: a multisample texture's GL_TEXTURE_SAMPLES (0 another texture, 1,
 * or 4: its level twice the size, every sample kept) and fixed locations. */
UBYTE ogles2_texture_samples(const struct OGLES3Texture *t, GLboolean *fixed);
/* fbo.c: a sized format a multisample texture may have; *none: no samples. */
BOOL ogles2_fbo_renderable_format(struct OGLES3Context *ctx, GLenum f, BOOL *none);
/* OES_depth_texture: level l is a depth image (bits 16 or 24; stencil: DEPTH_STENCIL_OES). */
BOOL ogles2_texture_depth(const struct OGLES3Texture *t, int l, UBYTE *bits, BOOL *stencil);
/* That image as a job's depth/stencil surface, its GPU storage made first; FALSE: none. */
BOOL ogles2_texture_s8_surface(struct OGLES3Context *ctx, struct OGLES3Texture *t, int l,
                               struct OGLES3Surface *s);
BOOL ogles2_fbo_zs_alloc(UWORD w, UWORD h, ULONG cpp, ULONG layers, struct v3d_mem *mem, ULONG *base, ULONG *ub, ULONG *bytes);
BOOL ogles2_texture_zs_surface(struct OGLES3Context *ctx, struct OGLES3Texture *t, int l,
                               struct OGLES3Surface *s);
/* The texture kinds a sampler reads (its uniform's type). */
#define OGLES3_TEX_2D   0
#define OGLES3_TEX_CUBE 1
#define OGLES3_TEX_RECT 2 /* ARB_texture_rectangle */
#define OGLES3_TEX_NONE 3 /* a sampler of no kind the library has: reads the incomplete texture's (0, 0, 0, 1) */
#define OGLES3_TEX_3D    4 /* ES 3.0 */
#define OGLES3_TEX_ARRAY 5 /* a 2D array */
#define OGLES3_TEX_2DMS  6 /* ES 3.1: GL_TEXTURE_2D_MULTISAMPLE (texelFetch only) */
#define OGLES3_TEX_INT   0x10 /* ORed in: an integer sampler (isampler*, usampler*): its incomplete
                               * texture's (0, 0, 0, 1) integers */
static inline int ogles2_sampler_kind(GLenum type)
{
    return type == GL_SAMPLER_2D || type == GL_SAMPLER_2D_SHADOW ? OGLES3_TEX_2D
         : type == GL_SAMPLER_CUBE || type == GL_SAMPLER_CUBE_SHADOW ? OGLES3_TEX_CUBE
         : type == GL_SAMPLER_2D_RECT ? OGLES3_TEX_RECT
         : type == GL_SAMPLER_3D ? OGLES3_TEX_3D
         : type == GL_SAMPLER_2D_ARRAY || type == GL_SAMPLER_2D_ARRAY_SHADOW ? OGLES3_TEX_ARRAY
         : type == GL_INT_SAMPLER_2D || type == GL_UNSIGNED_INT_SAMPLER_2D ? OGLES3_TEX_2D | OGLES3_TEX_INT
         : type == GL_INT_SAMPLER_CUBE || type == GL_UNSIGNED_INT_SAMPLER_CUBE ? OGLES3_TEX_CUBE | OGLES3_TEX_INT
         : type == GL_INT_SAMPLER_3D || type == GL_UNSIGNED_INT_SAMPLER_3D ? OGLES3_TEX_3D | OGLES3_TEX_INT
         : type == GL_INT_SAMPLER_2D_ARRAY || type == GL_UNSIGNED_INT_SAMPLER_2D_ARRAY ? OGLES3_TEX_ARRAY | OGLES3_TEX_INT
         : type == GL_SAMPLER_2D_MULTISAMPLE ? OGLES3_TEX_2DMS
         : type == GL_INT_SAMPLER_2D_MULTISAMPLE || type == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE
               ? OGLES3_TEX_2DMS | OGLES3_TEX_INT
                                                                                    : OGLES3_TEX_NONE;
}
/* textureSize's values for unit's texture of that kind: what 0 width, 1
 * height, 2 depth, 3 layers (texture.c). */
ULONG ogles2_texture_size(struct OGLES3Context *ctx, int unit, int kind, int what);
/* ES 3.1's images: image unit `unit`'s texture state, written at dst (32
 * bytes, 32-byte aligned; NULL: into the job's state buffer), its address
 * (0: out of memory); an unusable unit's reads the fallback (its sizes 0).
 * The sizes imageSize reads (0 width, 1 height, 2 depth, 3 layers). A
 * shader that may store there wrote it: the level is newer on the GPU
 * (serial: the job that does it, not finished yet; 0 finished). */
ULONG ogles2_image_state(struct OGLES3Context *ctx, int unit, void *dst);
ULONG ogles2_image_size(struct OGLES3Context *ctx, int unit, int what);
void ogles2_image_written(struct OGLES3Context *ctx, int unit, ULONG serial);
/* draw.c: the texture a sampler of that kind on `unit` reads returns 32 bits. */
BOOL ogles2_texture_wide(struct OGLES3Context *ctx, int unit, int kind);
extern ULONG ogles2_wide_seen; /* such a texture was ever given an image */
/* A box of the image as RGBA8 rows (row 0 = GL's bottom), from the GPU's copy. */
BOOL ogles2_texture_read(struct OGLES3Texture *t, int k, int l, int x, int y, int w, int h,
                         UBYTE *rgba, ULONG stride);
/* The same of a float or integer image as 16-byte texels: RGBA floats, or
 * four 32-bit integers (ES 3.0's glReadPixels of such targets). */
BOOL ogles2_texture_read_wide(struct OGLES3Texture *t, int k, int l, int x, int y, int w, int h, UBYTE *out,
                              ULONG stride);
/* A renderbuffer's colour storage: a texture of its own, in no name table;
 * of an ES 3.0 sized format (0: not one the library stores). */
struct OGLES3Texture *ogles2_texture_hidden(struct OGLES3Context *ctx, UWORD w, UWORD h,
                                            GLenum format, GLenum type);
struct OGLES3Texture *ogles2_texture_hidden_sized(struct OGLES3Context *ctx, UWORD w, UWORD h, GLenum ifmt);
void ogles2_texture_hidden_free(struct OGLES3Context *ctx, struct OGLES3Texture *t);
/* frame.c: GL_IMPLEMENTATION_COLOR_READ_FORMAT and _TYPE of the read buffer */
void ogles2_read_format(struct OGLES3Context *ctx, GLenum *format, GLenum *type);
/* glBlitFramebuffer's depth and stencil (fbo.c): the renderbuffer formats
 * compared, and the blit of one part on the CPU. */
UBYTE ogles2_fbo_samples(const struct OGLES3Framebuffer *fb);
BOOL ogles2_fbo_blit_same_format(struct OGLES3Context *ctx, UBYTE draw);
GLenum ogles2_fbo_zs_format(struct OGLES3Context *ctx, struct OGLES3Framebuffer *fb, BOOL stencil);
void ogles2_fbo_blit_zs(struct OGLES3Context *ctx, BOOL stencil, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                        GLint dx0, GLint dy0, GLint dx1, GLint dy1, LONG ox0, LONG oy0, LONG ox1, LONG oy1);
/* glBlitFramebuffer's writes (blit.c): a box of a draw buffer, of a texture
 * level, of the default framebuffer's bitmap. */
BOOL ogles2_fbo_write(struct OGLES3Context *ctx, int rt, int x, int y, int w, int h, const UBYTE *src, ULONG stride,
                      const UBYTE *mask);
BOOL ogles2_texture_write(struct OGLES3Texture *t, int k, int l, int x, int y, int w, int h, const UBYTE *src,
                          ULONG stride, const UBYTE *mask);
void ogles2_display_write(struct OGLES3Display *d, UWORD x, UWORD y, UWORD w, UWORD h, const UBYTE *src,
                          ULONG stride);
/* fbo.c: reads (glReadPixels, glCopyTex*) work on the draw binding's
 * framebuffer: the read binding's stands in for it meanwhile (begin returns
 * what end puts back). */
struct OGLES3Framebuffer *ogles2_fbo_read_begin(struct OGLES3Context *ctx);
void ogles2_fbo_read_end(struct OGLES3Context *ctx, struct OGLES3Framebuffer *saved);
BOOL ogles2_fbo_read_format(struct OGLES3Context *ctx, struct OGLES3RtFormat *rf); /* the read buffer's */
BOOL ogles2_fbo_read_wide(struct OGLES3Context *ctx, int x, int y, int w, int h, UBYTE *out, ULONG stride);
void ogles2_fb_update(struct OGLES3Context *ctx);       /* the fb_* fields from the binding */
GLenum ogles2_fb_status(struct OGLES3Context *ctx);     /* of the draw framebuffer */
BOOL ogles2_fb_ready(struct OGLES3Context *ctx);        /* complete; else GL_INVALID_FRAMEBUFFER_OPERATION */
BOOL ogles2_fbo_read_ready(struct OGLES3Context *ctx);  /* and, read from, not multisampled (GL_INVALID_OPERATION) */
/* glGetIntegeri_v's state beyond the buffer bindings (state.c), and the
 * image units' (texture.c): -1 no such query, 0 an error set, 1 *v. */
int ogles2_indexed_query(struct OGLES3Context *ctx, GLenum pname, GLuint index, GLint64 *v);
int ogles2_image_query(struct OGLES3Context *ctx, GLenum pname, GLuint index, GLint64 *v);
int ogles2_vab_query(struct OGLES3Context *ctx, GLenum pname, GLuint index, GLint64 *v); /* draw.c's bindings */
struct OGLES3Buffer *ogles2_buffer_get(struct OGLES3Context *ctx, GLuint name); /* NULL: no such buffer */
BOOL ogles2_fbo_target(struct OGLES3Context *ctx, struct OGLES3Framebuffer *fb, BOOL surfaces,
                       struct OGLES3Target *t);
void ogles2_fbo_rendered(struct OGLES3Context *ctx, struct OGLES3Framebuffer *fb);
UBYTE ogles2_fbo_take_discarded(struct OGLES3Framebuffer *fb); /* OGLES3_DISCARD_* since its last job */
/* glReadPixels from the bound framebuffer object: RGBA8 rows, GL's order. */
BOOL ogles2_fbo_read(struct OGLES3Context *ctx, int x, int y, int w, int h, UBYTE *rgba, ULONG stride);
void ogles2_fbo_texture_changing(struct OGLES3Context *ctx, struct OGLES3Texture *t);
void ogles2_fbo_texture_deleted(struct OGLES3Context *ctx, struct OGLES3Texture *t);
BOOL ogles2_fbo_query(struct OGLES3Context *ctx, GLenum pname, GLint *v);
GLenum ogles2_fbo_draw_buffer(struct OGLES3Context *ctx, int i); /* GL_DRAW_BUFFERi_EXT */
void ogles2_fbo_free(struct OGLES3Context *ctx);
BOOL ogles2_texture_query(struct OGLES3Context *ctx, GLenum pname, GLint *v);
/* The texture and sampler state of texture unit `unit` (its binding of
 * OGLES3_TEX_* kind), written into the open job's state buffer; FALSE: no memory. */
BOOL ogles2_texture_state(struct OGLES3Context *ctx, int unit, int kind, ULONG *tex_state,
                          ULONG *sampler_state);
ULONG ogles2_format(char *buf, ULONG size, const char *fmt, ...); /* log.c: RawDoFmt, bounded */
/* compiler.c: the on-target shader compiler (plan 8.4) */
BOOL ogles2_cache_assigned(void);   /* OGLES3Cache: exists */
const char *ogles2_cache_dir(void); /* where compiled programs go: OGLES3Cache: or T: */
BOOL ogles2_compiler_installed(void);
ULONG ogles2_compiler_id(void); /* the installed compiler's size and date, hashed; 0: none */
BOOL ogles2_compile_program(const char *vs, const char *fs, const char *path, char *log,
                            ULONG log_size);
/* The same program's variants for samplers returning 32 bits (its .v3dw). */
BOOL ogles2_compile_program_bound(const char *vs, const char *fs, const char *bindings, const char *path,
                                  char *log, ULONG log_size);
BOOL ogles2_compile_program_wide(const char *vs, const char *fs, const char *path, char *log,
                                 ULONG log_size);
BOOL ogles2_compile_program_a2c(const char *vs, const char *fs, const char *path, char *log,
                                ULONG log_size);
/* A program's draw-time variants into its .v3dx beside path: 1 written, 0
 * not, -1 no compiler that knows the request (stage A item 5). */
int ogles2_compile_variants(const char *vs, const char *fs, const char *bindings, const UBYTE *keys,
                            ULONG keys_len, ULONG layout_crc, const char *path, char *log, ULONG log_size);
/* program.c: the program's .v3dx image shipped with it (cache FALSE) or
 * compiled here (TRUE), found the first time; NULL if none. And the keys'
 * variants compiled into the cache's (it is read again): FALSE if not. */
const struct OGLES3Image *ogles2_image_x(struct OGLES3Image *im, BOOL cache);
BOOL ogles2_image_compile_variants(struct OGLES3Context *ctx, struct OGLES3Image *im, const UBYTE *keys,
                                   ULONG keys_len);
/* One shader through the front end: 1 compiled, 0 GLSL errors (in log),
 * -1 the compiler could not be run. */
int ogles2_compile_shader(BOOL fragment, const char *src, char *log, ULONG log_size);
/* ES 3.1's compute programs and shaders (V3DGLSL_LINK_COMPUTE, _COMPILE_COMPUTE). */
BOOL ogles2_compile_compute(const char *cs, const char *path, char *log, ULONG log_size);
/* ES 3.1: a separable program's .v3dp, its vertex or fragment shader (or
 * both; the other NULL), with link state `bindings` (or NULL). */
BOOL ogles2_pipeline_ready(struct OGLES3Context *ctx, BOOL compute); /* program.c: the bound pipeline */
/* The program a draw (compute: a dispatch) with the bound pipeline runs, its
 * state the stage programs'; NULL: none (an invalid pipeline, no memory, a
 * link that failed: logged). */
struct OGLES3Program *ogles2_pipeline_program(struct OGLES3Context *ctx, BOOL compute);
BOOL ogles2_compile_pipeline(const char *vs, const char *fs, const char *bindings, ULONG layout_crc, const char *path,
                             char *log, ULONG log_size);
BOOL ogles2_compile_separable(const char *vs, const char *fs, const char *bindings, const char *path, char *log,
                              ULONG log_size);
int ogles2_compile_compute_shader(const char *src, char *log, ULONG log_size);
void ogles2_compiler_stop(void);
/* buffer.c */
void ogles2_buffers_free(struct OGLES3Context *ctx);
BOOL ogles2_buffer_query(struct OGLES3Context *ctx, GLenum pname, GLint *v);
/* ES 3.0's pixel pack (`pack`) or unpack buffer's bytes at an application's
 * pixel offset; NULL: none bound; *bad: GL_INVALID_OPERATION. */
UBYTE *ogles2_buffer_pixels(struct OGLES3Context *ctx, BOOL pack, ULONG offset, ULONG bytes, BOOL *bad);
/* The GPU's copy of a buffer (32-bit words in its byte order), and uniform
 * buffer binding `index`'s address and bytes (0: none bound). */
ULONG ogles2_buffer_words(struct OGLES3Context *ctx, struct OGLES3Buffer *b);
ULONG ogles2_buffer_uniform(struct OGLES3Context *ctx, ULONG index, ULONG *size);
ULONG ogles2_buffer_storage(struct OGLES3Context *ctx, ULONG index, ULONG *size);
ULONG ogles2_buffer_atomic(struct OGLES3Context *ctx, ULONG index, ULONG *size);
ULONG ogles2_buffer_storage_draw(struct OGLES3Context *ctx, BOOL atomic, ULONG index, ULONG *size);
GLenum ogles2_buffer_dispatch_indirect(struct OGLES3Context *ctx, GLintptr offset, ULONG *groups);
GLenum ogles2_buffer_draw_indirect(struct OGLES3Context *ctx, GLintptr offset, ULONG bytes, ULONG *words);
struct OGLES3Buffer *ogles2_buffer_array(struct OGLES3Context *ctx);
/* The GL_ELEMENT_ARRAY_BUFFER binding, which a vertex array object holds (vao.c). */
struct OGLES3Buffer *ogles2_buffer_element(struct OGLES3Context *ctx);
void ogles2_buffer_set_element(struct OGLES3Context *ctx, struct OGLES3Buffer *b);
/* A reference to b dropped (an attribute array's or an element binding's):
 * a deleted buffer that nothing uses any more is freed. */
void ogles2_buffer_release(struct OGLES3Context *ctx, struct OGLES3Buffer *b);
/* The CPU reads or writes b next: what transform feedback wrote into it
 * waited for and taken into its data (never while a draw builds its job);
 * and the address transform feedback writes it at in the open job. */
void ogles2_buffer_cpu(struct OGLES3Context *ctx, struct OGLES3Buffer *b);
ULONG ogles2_buffer_xfb(struct OGLES3Context *ctx, struct OGLES3Buffer *b);
/* xfb.c, ES 3.0's transform feedback (docs/GLES3_PLAN.md stage C item 1):
 * the bound object's indexed bindings (NULL: no memory); whether it is
 * active, and active and not paused (drawing); whether an active object of
 * ctx's uses program p, or one of its objects holds buffer b; draws: the
 * checks (errors set), the packets around a draw's primitives, the job's
 * end; freed with the context. */
struct OGLES3XfbBinding *ogles2_xfb_bindings(struct OGLES3Context *ctx);
BOOL ogles2_xfb_active(const struct OGLES3Context *ctx);
BOOL ogles2_xfb_drawing(const struct OGLES3Context *ctx);
BOOL ogles2_xfb_program_busy(const struct OGLES3Context *ctx, const struct OGLES3Program *p);
BOOL ogles2_xfb_uses_buffer(const struct OGLES3Context *ctx, const struct OGLES3Buffer *b);
BOOL ogles2_xfb_draw_ok(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLsizei instances);
BOOL ogles2_xfb_before_draw(struct OGLES3Context *ctx, GLenum mode, BOOL psiz);
void ogles2_xfb_after_draw(struct OGLES3Context *ctx, GLenum mode, GLsizei count, GLsizei instances);
void ogles2_xfb_job_end(struct OGLES3Context *ctx);
void ogles2_xfbs_free(struct OGLES3Context *ctx);
GLuint ogles2_xfb_bound_name(const struct OGLES3Context *ctx); /* GL_TRANSFORM_FEEDBACK_BINDING */
BOOL ogles2_xfb_paused(const struct OGLES3Context *ctx);
/* program.c: a transform feedback object let go of p */
void ogles2_program_unref(struct OGLES3Context *ctx, struct OGLES3Program *p);
/* query.c: primitives transform feedback wrote, for GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN */
void ogles2_query_xfb_add(struct OGLES3Context *ctx, ULONG primitives);
/* vao.c: the bound vertex array object's name (0: object 0); whether an
 * object not bound uses b; freed with the context. */
GLuint ogles2_vao_bound(const struct OGLES3Context *ctx);
BOOL ogles2_vao_uses_buffer(struct OGLES3Context *ctx, const struct OGLES3Buffer *b);
void ogles2_vaos_free(struct OGLES3Context *ctx);
void ogles2_syncs_free(struct OGLES3Context *ctx); /* sync.c, with the group's last member */
/* debug_khr.c: KHR_debug */
void ogles2_debug_error(struct OGLES3Context *ctx, GLenum error); /* every GL error, as a message */
BOOL ogles2_debug_cap(struct OGLES3Context *ctx, GLenum cap, int on, GLboolean *state);
BOOL ogles2_debug_query(struct OGLES3Context *ctx, GLenum pname, GLint *v, BOOL *boolean);
void ogles2_debug_free(struct OGLES3Context *ctx); /* and the context's own objects' labels */
void ogles2_labels_free(struct OGLES3Share *sh);
void ogles2_label_forget(struct OGLES3Context *ctx, GLenum identifier, ULONG name); /* a deleted name's label */
void ogles2_labels_forget(struct OGLES3Context *ctx, GLenum identifier, GLsizei n, const GLuint *names); /* glDelete*'s */
/* query.c: the occlusion counter off around the library's own draws; freed with the context */
void ogles2_query_pause(struct OGLES3Context *ctx, BOOL pause);
void ogles2_queries_free(struct OGLES3Context *ctx);
GLuint ogles2_buffer_name(const struct OGLES3Buffer *b);
ULONG ogles2_buffer_size(const struct OGLES3Buffer *b);
const void *ogles2_buffer_indices(struct OGLES3Context *ctx, ULONG offset, ULONG bytes, BOOL *bad);
const UBYTE *ogles2_buffer_stream(const struct OGLES3Buffer *b);
ULONG ogles2_buffer_copy(struct OGLES3Context *ctx, struct OGLES3Buffer *b, ULONG offset, GLint size,
                         GLenum type, GLboolean normalized, GLsizei stride, ULONG elem_bytes, ULONG out_stride,
                         ULONG *dst_stride, ULONG *vertices,
                         void (*convert)(UBYTE *dst, ULONG dst_stride, const UBYTE *src,
                                         ULONG src_stride, ULONG count, GLint size, GLenum type));
/* glClear's quad path (draw.c): for clears the tile buffer cannot do. */
/* A clear drawn as a rectangle: the colour of draw buffers rts (the
 * others left as they are), depth and stencil, with these values (draw.c). */
void ogles2_draw_clear(struct OGLES3Context *ctx, GLbitfield mask, UBYTE rts, const GLfloat *color, GLfloat depth,
                       GLint stencil);
/* A box of draw buffer rt, a 32-bit float or integer image, set by the CPU
 * to v (floats' bits, or integers) in the channels mask lets through: the
 * jobs that draw into it have rendered (fbo.c, texture.c). */
BOOL ogles2_fbo_fill(struct OGLES3Context *ctx, int rt, int x, int y, int w, int h, const ULONG *v, BOOL is_int,
                     const GLboolean *mask);
BOOL ogles2_texture_fill(struct OGLES3Texture *t, int k, int l, int x, int y, int w, int h, const ULONG *v,
                         BOOL is_int, const GLboolean *mask);
/* TRUE once the open job has a draw (its tile-buffer clear would come too early). */
BOOL ogles2_job_has_draws(struct OGLES3Context *ctx);

/* ---- jobs (job.c) ---- */
void ogles2_job_begin(struct OGLES3Context *ctx);
/* The tile buffer's clear: the colour of targets rts from cv (floats'
 * bits, or integers), depth and stencil (job.c). */
void ogles2_job_clear(struct OGLES3Context *ctx, GLbitfield mask, UBYTE rts, const ULONG *cv, BOOL is_int,
                      GLfloat depth, GLint stencil);
BOOL ogles2_job_flush(struct OGLES3Context *ctx, BOOL frame_end); /* FALSE: dropped. Mid-frame (frame_end FALSE): depth and stencil are kept for the next job */
void ogles2_job_free(struct OGLES3Context *ctx); /* aglDestroyContext: job.c's memory */
struct V3DTexture;
BOOL ogles2_job_zs_view(struct OGLES3Context *ctx, struct V3DTexture *t);
void ogles2_job_discard(struct OGLES3Context *ctx);
/* The submitted render waited for (M6 overlap): before the CPU reads what it
 * writes or changes what it reads. */
void ogles2_job_finish(struct OGLES3Context *ctx);
void ogles2_job_take_gpu(struct OGLES3Context *ctx); /* submission lock held: a render left running finishes */
/* A job not rendered yet (open, or submitted and running) of any context in
 * ctx's share group is job `serial`: that context, else NULL. Serials are
 * unique across contexts. */
struct OGLES3Context *ogles2_job_user(struct OGLES3Context *ctx, ULONG serial);
#define ogles2_job_uses(ctx, serial) (ogles2_job_user((ctx), (serial)) != 0)

/* ---- share groups (share.c) ---- */
/* ctx joins `with`'s group, or a new one of its own (with NULL). FALSE: no memory. */
BOOL ogles2_share_join(struct OGLES3Context *ctx, struct OGLES3Context *with);
/* ctx leaves its group; TRUE if it was the last member (the shared objects go). */
BOOL ogles2_share_leave(struct OGLES3Context *ctx);
/* The semaphore a call into ctx takes (the dispatch table's wrappers): its
 * group's while it has other members, else none. An invalid ctx: none (the
 * entry itself refuses it). */
static inline struct SignalSemaphore *ogles2_share_lock_of(struct OGLES3Context *ctx)
{
    return ctx && ctx->magic == OGLES3_CONTEXT_MAGIC ? ctx->share_lock : 0;
}
/* The deferred free of a GPU block a job not rendered yet may read: on the
 * retire list of the member whose job last used it (serial), else freed now. */
struct v3d_mem;
void ogles2_share_defer_free(struct OGLES3Context *ctx, ULONG serial, struct v3d_mem *m);
/* Name tables (share.c): the slot of an object's name in its table, the
 * name itself below OGLES3_NEAR_NAMES; a larger name's (create: a new one,
 * the table then grows to it) through the table's list. OGLES3_NO_SLOT:
 * none, or no memory. */
#define OGLES3_NEAR_NAMES 65536UL
#define OGLES3_NO_SLOT    0xFFFFFFFFUL
GLuint ogles2_name_slot(struct OGLES3FarName **far, GLuint name, BOOL create);
void ogles2_name_forget(struct OGLES3FarName **far, GLuint name);   /* the name's slot is free again */
void ogles2_names_free(struct OGLES3FarName **far);

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct OGLES3Library *OGLES3LibBase;

/* Sets the sticky error if none is pending (GLES2 section 2.5). */
void ogles2_error(struct OGLES3Context *ctx, GLenum error);
/* A context pointer the library handed out and that is still alive. Inline:
 * every entry tests it, and a call costs more than the test under Emu68. */
static inline BOOL ogles2_context_valid(struct OGLES3Context *ctx)
{
    return ctx && ctx->magic == OGLES3_CONTEXT_MAGIC;
}
/* The highest OpenGL ES version contexts may have: 3.0 and 3.1 only in test
 * builds until each is complete (Makefile, ES3=1 and ES31=1;
 * docs/GLES3_PLAN.md §3). */
#if defined(OGLES3_ES31_CONTEXTS)
#define OGLES3_ES_MAX 31
#elif defined(OGLES3_ES3_CONTEXTS)
#define OGLES3_ES_MAX 30
#else
#define OGLES3_ES_MAX 20
#endif
/* context.c: the extensions a context of its version offers (GL_NUM_EXTENSIONS). */
ULONG ogles2_num_extensions(const struct OGLES3Context *ctx);
/* An OpenGL ES 3.x context (OGLES3_CCT_PISTORM_ES_VERSION 30 or 31). */
static inline BOOL ogles2_es3(const struct OGLES3Context *ctx)
{
    return ctx->es_version >= 30;
}
/* An ES 3.0-only entry (docs/GLES3_PLAN.md §3): a valid ES3 context, else
 * FALSE, with GL_INVALID_OPERATION in a GLES2 context, as an entry it lacks;
 * but for a GLES2 extension's entry that stands for it (gles2_ext.c). */
static inline BOOL ogles2_es3_entry(struct OGLES3Context *ctx)
{
    if (!ogles2_context_valid(ctx))
        return FALSE;
    if (ctx->es_version < 30 && !ctx->gles2_ext) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    return TRUE;
}
/* An OpenGL ES 3.1 context (stage D). */
static inline BOOL ogles2_es31(const struct OGLES3Context *ctx)
{
    return ctx->es_version >= 31;
}
/* An ES 3.1-only entry: as ogles2_es3_entry, for a 3.1 context. */
static inline BOOL ogles2_es31_entry(struct OGLES3Context *ctx)
{
    if (!ogles2_context_valid(ctx))
        return FALSE;
    if (ctx->es_version < 31) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return FALSE;
    }
    return TRUE;
}

/* ---- device (device.c) ----
 * Initialises the V3D on first use (powers it on, reads its identity);
 * FALSE if there is none or it did not start. */
BOOL ogles2_device_open(void);
/* GL_RENDERER: "V3D 4.2.14 (PiStorm), 8 QPUs, 64 KB VPM" */
const char *ogles2_device_renderer(void);
/* [addr, addr+size) lies in RAM the GPU reaches (the device tree's memory node). */
BOOL ogles2_device_gpu_reachable(ULONG addr, ULONG size);
/* The public "V3D 4.2 submit" semaphore (v3dcore/include/v3d_gpu_lock.h):
 * held from a job's binning submit until its render has finished. */
struct SignalSemaphore *ogles2_device_gpu_lock(void);
/* TRUE once the device initialised (a state-only context may have none). */
BOOL ogles2_device_ready(void);
/* The Pi's 1 MHz system timer: microseconds, wrapping every 71 minutes
 * (take differences as ULONG). */
ULONG ogles2_clock_us(void);
ULONG ogles2_device_bytes(void); /* GPU memory in use */
void ogles2_trace(struct OGLES3Context *ctx, const char *what, LONG n, ULONG first); /* probe T */
void ogles2_device_reset(const char *why); /* the GPU did not finish a job: reset it (lock held) */
/* v3dcore's device (struct V3DDevice); valid after ogles2_device_open. */
struct V3DDevice *ogles2_device(void);
/* Allocates a context's GPU pools and control lists for a w x h default
 * framebuffer with depth_bits 16 (D16) or 32 (D32F); NULL if out of memory. */
struct V3DContext *ogles2_device_context_new(UWORD w, UWORD h, int depth_bits, BOOL msaa);
void ogles2_device_context_free(struct V3DContext *v);
/* Sizes the pools for a new w x h (no frame open); FALSE if out of memory,
 * the old size kept where it can be. */
BOOL ogles2_device_context_resize(struct V3DContext *v, UWORD w, UWORD h);

/* ---- log (log.c; plan section 10) ----
 * Lines go to a RAM ring buffer, appended to the log file (T:ogles3.log, or
 * the path in ENV:OGLES3/LogFile) when a context is destroyed, when the
 * library is closed and when the OGLES3Log command asks. Builds with
 * OGLES3_LOG_SYNC write every line at once, so a hang loses nothing.
 * Formats are exec RawDoFmt's: %ld/%lu/%lx for 32-bit values, %s, %c. */
BOOL ogles2_log_init(void);
void ogles2_log_term(void);
void ogles2_log(const char *fmt, ...);
/* Appends the buffered lines to the log file. Needs a DOS process outside
 * Forbid(); returns FALSE (and keeps the lines) when called from a plain task.
 * Never call it from the library's Open/Close/Expunge vectors. */
BOOL ogles2_log_flush(void);

#endif
