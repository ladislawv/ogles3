/*
 * Mesa's V3D driver on a fake DRM device, no window system.
 *   usage: v3dglslc [-d <V3D_DEBUG flags>] [-o <dir>] [-a] [-x <keys>] vs.vert fs.frag [vs2 fs2 ...]
 *          (-x: the variants of the keys in the file, into a .v3dx: write_program;
 *          -a: the alpha-to-coverage variants, a .v3da, as V3DGLSL_LINK_A2C writes it)
 *          v3dglslc SERVER   (AmigaOS: the on-target compiler, v3dp/v3dglsl_port.h)
 *          v3dglslc -L       (the limits, as the context reports them: print_limits)
 * Creates a V3D 4.2 screen (compat/fake_drm.c), a surfaceless GLES2 context
 * through the state tracker, and calls Mesa's GL entry points directly.
 * -o writes each program as <dir>/<vs>-<fs>.v3dp (dev/v3dp/FORMAT.md), with
 * the variants the driver compiles for draws in the default key states.
 *
 * Built for m68k-amigaos (build_m68k.sh; the seed of the S2 compiler module)
 * and natively on the PC (build_host.sh, the offline compiler), so the two can be compared
 * byte for byte.
 */
#include <stdlib.h>
#include "main/mtypes.h"
#include "main/context.h"
#include "main/shaderobj.h"
#include "api_exec_decl.h" /* generated: prototypes of the _mesa_* GL entry points */
#include "state_tracker/st_context.h"
#include "state_tracker/st_manager.h"
#include "frontend/api.h"
#include "pipe/p_screen.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/macros.h"
#include "v3d_screen.h"
#include "compiler/v3d_compiler.h" /* the variant keys (-x) */
#include "xf86drm.h"
#include "v3dp_writer.h"

#define glCreateShader       _mesa_CreateShader
#define glShaderSource       _mesa_ShaderSource
#define glCompileShader      _mesa_CompileShader
#define glGetShaderiv        _mesa_GetShaderiv
#define glGetShaderInfoLog   _mesa_GetShaderInfoLog
#define glCreateProgram      _mesa_CreateProgram
#define glAttachShader       _mesa_AttachShader
#define glLinkProgram        _mesa_LinkProgram
#define glGetProgramiv       _mesa_GetProgramiv
#define glGetProgramInfoLog  _mesa_GetProgramInfoLog
#define glDeleteShader       _mesa_DeleteShader
#define glDeleteProgram      _mesa_DeleteProgram
#define glBindAttribLocation _mesa_BindAttribLocation
#define V3DC_WRITER
static void program_path(char *out, size_t size, const char *vs_path, const char *fs_path);
static int write_program(GLuint p, const char *path, const char *vs_src, const char *fs_src);
#include "compile_common.c"
#ifdef __amigaos__
#include <proto/exec.h>
#include "v3dglsl_port.h"
#endif

int fake_drm_open(void);

/* The build's stamp (build_m68k.sh: its time and the commits), which
 * ogles2.library reads from the file's tail to tell compilers apart
 * (ogles2_compiler_id: a size alone matched two different builds). Writable,
 * so it goes into the data hunk, which follows the 18 MB of code and its
 * relocations, a few kilobytes from the end of the file. */
#ifndef V3DGLSLC_STAMP
#define V3DGLSLC_STAMP "unstamped"
#endif
__attribute__((used)) char v3dglslc_stamp[] = "$V3DGLSLC-STAMP:" V3DGLSLC_STAMP "$";

/* The release's version, for AmigaDOS' Version command: the library's
 * version.revision and date (ogles2/ogles2_internal.h), which build_m68k.sh
 * and build_host.sh pass. Install-ogles2 replaces an older LIBS:v3dglslc, or
 * one without it (1.x), whenever it installs the library (docs/GLES3_PLAN.md
 * section 3). Beside the stamp in the data hunk, so the file's tail still
 * holds that. */
#ifndef V3DGLSLC_VER
#define V3DGLSLC_VER "0.0 (unversioned)"
#endif
__attribute__((used)) char v3dglslc_ver[] = "$VER: v3dglslc " V3DGLSLC_VER " PiStorm V3D 4.2";

#ifdef __amigaos__
void os_amiga_set_option(const char *name, const char *value);
#define set_option os_amiga_set_option
#else
static void set_option(const char *name, const char *value)
{
    setenv(name, value, 1);
}
#endif

/* The driver's driconf options, as the pipe loader passes them. */
static const driOptionDescription v3d_driconf[] = {
#include "driinfo_v3d.h"
};

/* File name part of a path, without its extension, at most 31 characters. */
static void base_name(char *out, const char *path)
{
    const char *b = path, *p, *dot = NULL;
    size_t n;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':')
            b = p + 1;
    for (p = b; *p; p++)
        if (*p == '.')
            dot = p;
    n = dot ? (size_t)(dot - b) : strlen(b);
    if (n > 31)
        n = 31;
    memcpy(out, b, n);
    out[n] = 0;
}

/* The render target of the key-state draws: an RGBA8 colour texture and a
 * 16-bit depth buffer, created once; and the textures the samplers get:
 * RGBA8, or RGBA32F for the variants that sample 32-bit returns. */
static GLuint draw_fbo, draw_depth, tex_2d, tex_cube, tex_2d32, tex_cube32, tex_rect, tex_rect32;
/* EXT_draw_buffers: targets with 2, 3 and 4 RGBA8 colour images, all drawn
 * into (the fragment key's cbufs 0x3, 0x7, 0xF). */
static GLuint mrt_fbo[3];

/* What write_program writes (v3dp/FORMAT.md): the variants of the default
 * key states (.v3dp); and, for a program with samplers, those of the same
 * draws with every sampler returning 32 bits (float and depth textures),
 * which ogles2.library loads only when a draw needs them (.v3dw, the same
 * file format). The command line writes both; a server request one. */
#define WRITE_BOTH 0
#define WRITE_PLAIN 1
#define WRITE_WIDE 2
#define WRITE_A2C 3 /* the alpha-to-coverage fragment variants: a .v3da (the server's, -a) */
static int write_mode = WRITE_BOTH;
extern bool v3d_force_alpha_to_coverage; /* Mesa patch 0014 */
/* Mesa patch 0015: one variant for exactly `key` (the draw-time variants). */
extern bool v3d_compile_variant(struct pipe_context *pctx, mesa_shader_stage stage, struct v3d_key *key,
                                size_t key_size);
static struct pipe_context *the_pipe;
/* -x: the keys of the variants to compile (a .v3dx instead of the .v3dp):
 * each a u32 VARIANT stage and the key's bytes (v3dp_key_parse). */
static const uint8_t *variant_keys;
static size_t variant_keys_len;
static uint32_t variant_layout_crc; /* V3DGLSL_COMPILE_VARIANT's (0: not checked) */
#ifndef V3DGLSL_KEY_OUTPUTS_FROM_FS
#define V3DGLSL_KEY_OUTPUTS_FROM_FS 0x100 /* v3dp/v3dglsl_port.h (AmigaOS only): also for -x */
#endif

static int make_target(void)
{
    GLuint tex, depth;
    GLenum status;

    _mesa_GenRenderbuffers(1, &draw_depth);
    depth = draw_depth;

    _mesa_GenTextures(1, &tex);
    _mesa_BindTexture(GL_TEXTURE_2D, tex);
    _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    _mesa_BindTexture(GL_TEXTURE_2D, 0);
    _mesa_BindRenderbuffer(GL_RENDERBUFFER, depth);
    _mesa_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
    _mesa_GenFramebuffers(1, &draw_fbo);
    _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    status = _mesa_CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "v3dglslc: draw target incomplete (0x%04x)\n", (unsigned)status);
        return 0;
    }
    {
        static const GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                                        GL_COLOR_ATTACHMENT3 };
        int n, i;
        for (n = 2; n <= 4; n++) {
            _mesa_GenFramebuffers(1, &mrt_fbo[n - 2]);
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, mrt_fbo[n - 2]);
            for (i = 0; i < n; i++) {
                GLuint t;
                _mesa_GenTextures(1, &t);
                _mesa_BindTexture(GL_TEXTURE_2D, t);
                _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, t, 0);
            }
            _mesa_BindTexture(GL_TEXTURE_2D, 0);
            _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            _mesa_DrawBuffers(n, bufs);
            status = _mesa_CheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "v3dglslc: %d-target draw target incomplete (0x%04x)\n", n, (unsigned)status);
                return 0;
            }
        }
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    }
    _mesa_Viewport(0, 0, 64, 64);
    _mesa_Enable(GL_DEPTH_TEST);

    /* Complete 1x1 RGBA8 textures for the samplers (bind_samplers). */
    {
        static const GLubyte texel[4] = { 255, 255, 255, 255 };
        GLenum face;
        _mesa_GenTextures(1, &tex_2d);
        _mesa_BindTexture(GL_TEXTURE_2D, tex_2d);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_2D, 0);
        _mesa_GenTextures(1, &tex_cube);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, tex_cube);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    /* The same as RGBA32F (OES_texture_float; nearest: not filterable), whose
     * 32-bit return sets the key's sampler_is_32b. */
    {
        static const GLfloat texel[4] = { 1, 1, 1, 1 };
        GLenum face;
        _mesa_GenTextures(1, &tex_2d32);
        _mesa_BindTexture(GL_TEXTURE_2D, tex_2d32);
        _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_2D, 0);
        _mesa_GenTextures(1, &tex_cube32);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, tex_cube32);
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    /* Rectangle textures (ARB_texture_rectangle, patches/0011), RGBA8 and
     * RGBA32F. */
    {
        static const GLubyte texel8[4] = { 255, 255, 255, 255 };
        static const GLfloat texel32[4] = { 1, 1, 1, 1 };
        _mesa_GenTextures(1, &tex_rect);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, tex_rect);
        _mesa_TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel8);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_GenTextures(1, &tex_rect32);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, tex_rect32);
        _mesa_TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, texel32);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_BindTexture(GL_TEXTURE_RECTANGLE, 0);
    }
    if (_mesa_GetError() != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: the key-state textures could not be made\n");
        return 0;
    }
    return 1;
}

/* The sampler types beyond GLSL ES 1.00's three (2D, cube and rectangle,
 * whose textures make_target makes): OES_texture_3D's sampler3D, and GLSL ES
 * 3.00's (docs/GLES3_PLAN.md stage A item 2). Each gets a complete 1x1 (x1)
 * texture of its target and kind, made when first bound: f RGBA8, d
 * DEPTH_COMPONENT16 compared (shadow samplers), i RGBA8I, u RGBA8UI; and for
 * the variants of samplers returning 32 bits RGBA32F, DEPTH_COMPONENT32F,
 * RGBA32I, RGBA32UI, as real draws with such textures give the key's
 * sampler_is_32b. */
static struct { GLenum type, target; char kind; GLuint tex[2]; } more_samplers[] = {
    { GL_SAMPLER_3D, GL_TEXTURE_3D, 'f' },
    { GL_SAMPLER_2D_ARRAY, GL_TEXTURE_2D_ARRAY, 'f' },
    { GL_SAMPLER_2D_SHADOW, GL_TEXTURE_2D, 'd' },
    { GL_SAMPLER_CUBE_SHADOW, GL_TEXTURE_CUBE_MAP, 'd' },
    { GL_SAMPLER_2D_ARRAY_SHADOW, GL_TEXTURE_2D_ARRAY, 'd' },
    { GL_INT_SAMPLER_2D, GL_TEXTURE_2D, 'i' },
    { GL_INT_SAMPLER_3D, GL_TEXTURE_3D, 'i' },
    { GL_INT_SAMPLER_CUBE, GL_TEXTURE_CUBE_MAP, 'i' },
    { GL_INT_SAMPLER_2D_ARRAY, GL_TEXTURE_2D_ARRAY, 'i' },
    { GL_UNSIGNED_INT_SAMPLER_2D, GL_TEXTURE_2D, 'u' },
    { GL_UNSIGNED_INT_SAMPLER_3D, GL_TEXTURE_3D, 'u' },
    { GL_UNSIGNED_INT_SAMPLER_CUBE, GL_TEXTURE_CUBE_MAP, 'u' },
    { GL_UNSIGNED_INT_SAMPLER_2D_ARRAY, GL_TEXTURE_2D_ARRAY, 'u' },
};

static int more_sampler(GLenum type)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(more_samplers); i++)
        if (more_samplers[i].type == type)
            return (int)i;
    return -1;
}

static int is_sampler(GLenum type)
{
    return type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE || type == GL_SAMPLER_2D_RECT || more_sampler(type) >= 0;
}

static GLuint make_key_texture(GLenum target, char kind, int wide)
{
    static const GLuint zeros[4 * 6]; /* a texel of any of the formats, six cube faces' worth */
    GLenum ifmt, fmt, type;
    GLuint t;
    switch (kind) {
    case 'd': ifmt = wide ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT16; fmt = GL_DEPTH_COMPONENT;
              type = wide ? GL_FLOAT : GL_UNSIGNED_SHORT; break;
    case 'i': ifmt = wide ? GL_RGBA32I : GL_RGBA8I; fmt = GL_RGBA_INTEGER; type = wide ? GL_INT : GL_BYTE; break;
    case 'u': ifmt = wide ? GL_RGBA32UI : GL_RGBA8UI; fmt = GL_RGBA_INTEGER;
              type = wide ? GL_UNSIGNED_INT : GL_UNSIGNED_BYTE; break;
    default:  ifmt = wide ? GL_RGBA32F : GL_RGBA8; fmt = GL_RGBA; type = wide ? GL_FLOAT : GL_UNSIGNED_BYTE; break;
    }
    _mesa_GenTextures(1, &t);
    _mesa_BindTexture(target, t);
    if (target == GL_TEXTURE_CUBE_MAP) {
        GLenum face;
        for (face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; face++)
            _mesa_TexImage2D(face, 0, ifmt, 1, 1, 0, fmt, type, zeros);
    } else if (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY) {
        _mesa_TexImage3D(target, 0, ifmt, 1, 1, 1, 0, fmt, type, zeros);
    } else {
        _mesa_TexImage2D(target, 0, ifmt, 1, 1, 0, fmt, type, zeros);
    }
    _mesa_TexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    _mesa_TexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (kind == 'd')
        _mesa_TexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    _mesa_BindTexture(target, 0);
    return t;
}

/* The program has a sampler uniform. */
static int has_samplers(GLuint p)
{
    GLint n = 0, i, size;
    GLenum type;
    char name[256];
    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (is_sampler(type))
            return 1;
    }
    return 0;
}

/* The texture target and the key-state texture a sampler of `type` gets. */
static GLenum sampler_target(GLenum type, int wide, GLuint *tex)
{
    int k = more_sampler(type);
    if (k >= 0) {
        if (!more_samplers[k].tex[wide])
            more_samplers[k].tex[wide] = make_key_texture(more_samplers[k].target, more_samplers[k].kind, wide);
        *tex = more_samplers[k].tex[wide];
        return more_samplers[k].target;
    }
    if (type == GL_SAMPLER_CUBE) {
        *tex = wide ? tex_cube32 : tex_cube;
        return GL_TEXTURE_CUBE_MAP;
    }
    if (type == GL_SAMPLER_2D_RECT) {
        *tex = wide ? tex_rect32 : tex_rect;
        return GL_TEXTURE_RECTANGLE;
    }
    *tex = wide ? tex_2d32 : tex_2d;
    return GL_TEXTURE_2D;
}

/* Give every sampler of the current program its own texture unit with a
 * complete RGBA8 texture of its kind bound, or (restore) put them all back
 * on unit 0, the value a freshly linked program has and the .v3dp template
 * records. Needed because:
 *  - samplers of different kinds on one unit make the draw an error (GLES2);
 *  - a sampler with nothing bound makes Mesa build its fallback texture,
 *    which waits on a fence (glFinish) the fake device cannot give;
 *  - the key's sampler_is_32b comes from the bound texture's format (wide:
 *    RGBA32F textures, for the variants with every sampler 32-bit). */
static void bind_samplers(GLuint p, int restore, int wide)
{
    GLint n = 0, i, j, unit = 0;

    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, units[32];
        GLenum type;
        _mesa_GetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
        if (!is_sampler(type))
            continue;
        loc = _mesa_GetUniformLocation(p, name);
        if (loc < 0 || size > 32)
            continue;
        for (j = 0; j < size; j++) {
            units[j] = restore ? 0 : unit;
            if (!restore) {
                GLuint tex;
                GLenum target = sampler_target(type, wide, &tex);
                _mesa_ActiveTexture(GL_TEXTURE0 + unit);
                _mesa_BindTexture(target, tex);
                unit++;
            }
        }
        _mesa_Uniform1iv(loc, size, units);
    }
    _mesa_ActiveTexture(GL_TEXTURE0);
}

/* The columns of a matrix attribute (matCxR has C), 1 for the others. */
static int attrib_columns(GLenum type)
{
    switch (type) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: return 4;
    default: return 1;
    }
}

/* An integer attribute (GLSL ES 3.00), read through glVertexAttribIPointer. */
static int attrib_integer(GLenum type)
{
    switch (type) {
    case GL_INT: case GL_INT_VEC2: case GL_INT_VEC3: case GL_INT_VEC4:
    case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_VEC2: case GL_UNSIGNED_INT_VEC3: case GL_UNSIGNED_INT_VEC4:
        return 1;
    default:
        return 0;
    }
}

/* GLSL ES 3.x programs: a target whose colour attachments match the fragment
 * outputs' types at their locations (float RGBA8, int RGBA8I, unsigned
 * RGBA8UI; docs/GLES3_PLAN.md Appendix B.1), so the variants are those a real
 * draw into such a framebuffer uses: an integer output drawn into RGBA8 gives
 * one no draw uses. One target per combination, made when first needed; 0
 * for one float output at location 0, which draw_fbo is. */
static GLuint typed_target(GLuint p)
{
    static struct { char kinds[5]; GLuint fbo; } made[16];
    static unsigned nmade;
    static const GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                                    GL_COLOR_ATTACHMENT3 };
    char kinds[5] = "----";
    GLint n = 0, i, k;
    GLenum draw[4];
    GLuint fbo;

    _mesa_GetProgramInterfaceiv(p, GL_PROGRAM_OUTPUT, GL_ACTIVE_RESOURCES, &n);
    for (i = 0; i < n; i++) {
        static const GLenum props[3] = { GL_TYPE, GL_LOCATION, GL_ARRAY_SIZE };
        GLint v[3] = { 0, -1, 1 };
        char kind;
        _mesa_GetProgramResourceiv(p, GL_PROGRAM_OUTPUT, (GLuint)i, 3, props, 3, NULL, v);
        if (v[1] < 0)
            continue; /* gl_FragDepth */
        kind = attrib_integer((GLenum)v[0]) ? (v[0] == GL_INT || v[0] == GL_INT_VEC2 || v[0] == GL_INT_VEC3 ||
                                                v[0] == GL_INT_VEC4 ? 'i' : 'u')
                                             : 'f';
        for (k = v[1]; k < v[1] + (v[2] > 0 ? v[2] : 1) && k < 4; k++)
            kinds[k] = kind;
    }
    if (!strcmp(kinds, "f---"))
        return 0;
    for (i = 0; i < (GLint)nmade; i++)
        if (!strcmp(made[i].kinds, kinds))
            return made[i].fbo;
    if (nmade == ARRAY_SIZE(made))
        return 0;
    _mesa_GenFramebuffers(1, &fbo);
    _mesa_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (k = 0; k < 4; k++) {
        GLuint t;
        draw[k] = kinds[k] == '-' ? GL_NONE : bufs[k];
        if (kinds[k] == '-')
            continue;
        _mesa_GenTextures(1, &t);
        _mesa_BindTexture(GL_TEXTURE_2D, t);
        if (kinds[k] == 'f')
            _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        else if (kinds[k] == 'i')
            _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8I, 64, 64, 0, GL_RGBA_INTEGER, GL_BYTE, NULL);
        else
            _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 64, 64, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, NULL);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, bufs[k], GL_TEXTURE_2D, t, 0);
    }
    _mesa_BindTexture(GL_TEXTURE_2D, 0);
    _mesa_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, draw_depth);
    _mesa_DrawBuffers(4, draw);
    if (_mesa_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "v3dglslc: the target for outputs %s is incomplete\n", kinds);
        _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        return 0;
    }
    _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    memcpy(made[nmade].kinds, kinds, sizeof(kinds));
    made[nmade++].fbo = fbo;
    return fbo;
}

/* A zeroed buffer bound at every binding the program's uniform blocks name
 * (GLSL ES 3.00): a block without a buffer is an error to draw with. 64 KB,
 * the largest block (GL_MAX_UNIFORM_BLOCK_SIZE). */
static void bind_uniform_blocks(GLuint p)
{
    static GLuint ubo;
    GLint n = 0, i, binding;
    _mesa_GetProgramiv(p, GL_ACTIVE_UNIFORM_BLOCKS, &n);
    if (!n)
        return;
    if (!ubo) {
        void *zeros = calloc(1, 65536);
        _mesa_GenBuffers(1, &ubo);
        _mesa_BindBuffer(GL_UNIFORM_BUFFER, ubo);
        _mesa_BufferData(GL_UNIFORM_BUFFER, 65536, zeros, GL_STATIC_DRAW);
        free(zeros);
    }
    for (i = 0; i < n; i++) {
        binding = 0;
        _mesa_GetActiveUniformBlockiv(p, (GLuint)i, GL_UNIFORM_BLOCK_BINDING, &binding);
        _mesa_BindBufferBase(GL_UNIFORM_BUFFER, (GLuint)binding, ubo);
    }
}

/* Draw once per default key state (plan section 8.2) so the driver compiles
 * the variants real draws use: triangles, lines and points into an RGBA8
 * target with depth on. The fake device runs nothing; the draws only make
 * the driver compute its keys and compile. */
static int draw_key_states(GLuint p, int wide)
{
    static const GLenum modes[] = { GL_TRIANGLES, GL_LINES, GL_POINTS };
    static const GLfloat zeros[4 * 4 * 3]; /* 3 vertices of a mat4 attribute */
    GLint n = 0, i, err;
    unsigned m;

    if (!draw_fbo && !make_target())
        return 0;
    _mesa_UseProgram(p);
    bind_samplers(p, 0, wide);
    bind_uniform_blocks(p);
    _mesa_GetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &n);
    for (i = 0; i < n; i++) {
        char name[256];
        GLint size, loc, col, cols;
        GLenum type;
        _mesa_GetActiveAttrib(p, i, sizeof(name), NULL, &size, &type, name);
        loc = _mesa_GetAttribLocation(p, name);
        if (loc < 0)
            continue; /* built-in input such as gl_VertexID */
        cols = attrib_columns(type);
        for (col = 0; col < cols; col++) {
            _mesa_EnableVertexAttribArray(loc + col);
            if (attrib_integer(type))
                _mesa_VertexAttribIPointer(loc + col, 4, GL_INT, 0, zeros);
            else
                _mesa_VertexAttribPointer(loc + col, 4, GL_FLOAT, GL_FALSE, 0, zeros);
        }
    }
    {
        GET_CURRENT_CONTEXT(ctx);
        struct gl_shader_program *sp = _mesa_lookup_shader_program(ctx, p);
        struct gl_linked_shader *fsh = sp ? sp->_LinkedShaders[MESA_SHADER_FRAGMENT] : NULL;
        const uint64_t more = BITFIELD64_BIT(FRAG_RESULT_DATA1) | BITFIELD64_BIT(FRAG_RESULT_DATA2) |
                              BITFIELD64_BIT(FRAG_RESULT_DATA3);
        /* GLSL ES 3.x: one target with the outputs' locations and types. */
        GLuint typed = sp && sp->GLSL_Version >= 300 ? typed_target(p) : 0;
        if (typed)
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, typed);
        for (m = 0; m < ARRAY_SIZE(modes); m++)
            _mesa_DrawArrays(modes[m], 0, 3);
        if (typed)
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        /* EXT_draw_buffers (GLSL ES 1.00): a fragment shader that writes
         * gl_FragData[1] or up is also drawn into 2, 3 and 4 targets.
         * gl_FragColor would be broadcast to them too, but every program
         * would then compile nine more fragment shaders: ogles2 draws such a
         * program into the first target only. */
        if (sp && sp->GLSL_Version < 300 && fsh && fsh->Program && (fsh->Program->info.outputs_written & more)) {
            int n;
            for (n = 0; n < 3; n++) {
                _mesa_BindFramebuffer(GL_FRAMEBUFFER, mrt_fbo[n]);
                for (m = 0; m < ARRAY_SIZE(modes); m++)
                    _mesa_DrawArrays(modes[m], 0, 3);
            }
            _mesa_BindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        }
    }
    /* Submit the job (a no-op on the fake device) so jobs do not pile up.
     * Not glFinish: its fence needs a sync file the fake device cannot give. */
    _mesa_Flush();
    for (i = 0; i < 16; i++)
        _mesa_DisableVertexAttribArray(i);
    bind_samplers(p, 1, 0);
    _mesa_UseProgram(0);
    err = _mesa_GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: GL error 0x%04x in the key-state draws\n", (unsigned)err);
        return 0;
    }
    return 1;
}

/* -o: <dir>/<vs>-<fs>.v3dp. */
static void program_path(char *out, size_t size, const char *vs_path, const char *fs_path)
{
    char vs_base[32], fs_base[32];
    size_t n = strlen(out_dir);
    base_name(vs_base, vs_path);
    base_name(fs_base, fs_path);
    snprintf(out, size, "%s%s%s-%s.%s", out_dir,
             n && out_dir[n - 1] != '/' && out_dir[n - 1] != ':' ? "/" : "", vs_base, fs_base,
             write_mode == WRITE_A2C ? "v3da" : "v3dp");
}

static int write_program(GLuint p, const char *path, const char *vs_src, const char *fs_src)
{
    GET_CURRENT_CONTEXT(ctx);
    struct gl_shader_program *prog = _mesa_lookup_shader_program(ctx, p);
    /* The bindings of a V3DGLSL_LINK_BOUND request are hashed as a third
     * "source" (v3dp/FORMAT.md, PROGRAM). */
    static const unsigned stages[3] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, V3DP_HASH_BINDINGS };
    const char *sources[3];
    unsigned nsrc = link_bindings ? 3 : 2;
    int drawn;

    if (!prog)
        return 0;
    sources[0] = vs_src;
    sources[1] = fs_src;
    sources[2] = link_bindings;
    /* Draw-time variants (docs/GLES3_PLAN.md stage A item 5): one draw so
     * the driver has the program's shaders bound, then each key compiled
     * as it is (Mesa patch 0015), into the program's .v3dx. */
    if (variant_keys) {
        char xpath[512];
        size_t n = strlen(path), at = 0, used;
        int ok = 1;
        if (n <= 5 || n >= sizeof(xpath) || !draw_key_states(p, 0))
            return 0;
        snprintf(xpath, sizeof(xpath), "%.*s.v3dx", (int)(n - 5), path);
        v3dp_capture_begin();
        while (ok && at < variant_keys_len) {
            struct v3d_vs_key vk;
            struct v3d_fs_key fk;
            unsigned vstage;
            /* A vertex key whose outputs follow the fragment variant before
             * it (V3DGLSL_KEY_OUTPUTS_FROM_FS): read without the flag. */
            uint8_t one[4 + 512];
            const uint8_t *key = variant_keys + at;
            int from_fs = variant_keys_len - at >= 4 && (key[1] & (V3DGLSL_KEY_OUTPUTS_FROM_FS >> 8));
            if (from_fs) {
                size_t n = variant_keys_len - at < sizeof(one) ? variant_keys_len - at : sizeof(one);
                memcpy(one, key, n);
                one[1] &= ~(V3DGLSL_KEY_OUTPUTS_FROM_FS >> 8);
                used = v3dp_key_parse(one, n, &vstage, &vk, &fk);
            } else {
                used = v3dp_key_parse(key, variant_keys_len - at, &vstage, &vk, &fk);
            }
            if (!used || (from_fs && vstage == 4)) {
                fprintf(stderr, "v3dglslc: unreadable variant key at byte %u\n", (unsigned)at);
                ok = 0;
                break;
            }
            if (from_fs) {
                uint8_t slots[64];
                unsigned i;
                if (!v3dp_last_fs_inputs(&vk.num_used_outputs, slots)) {
                    fprintf(stderr, "v3dglslc: the vertex key at byte %u follows no fragment variant\n",
                            (unsigned)at);
                    ok = 0;
                    break;
                }
                for (i = 0; i < 64; i++) /* as Mesa copies them: the rest zeroed */
                    vk.used_outputs[i].slot_and_component = i < vk.num_used_outputs ? slots[i] : 0;
            }
            ok = vstage == 4 ? v3d_compile_variant(the_pipe, MESA_SHADER_FRAGMENT, &fk.base, sizeof(fk))
                             : v3d_compile_variant(the_pipe, MESA_SHADER_VERTEX, &vk.base, sizeof(vk));
            if (!ok)
                fprintf(stderr, "v3dglslc: the variant at byte %u did not compile\n", (unsigned)at);
            at += used;
        }
        v3dp_capture_end();
        return ok && v3dp_write_variants(xpath, prog, nsrc, stages, sources, variant_layout_crc) == 0;
    }
    /* The alpha-to-coverage variants: the same draws with the fragment key's
     * msaa and sample_alpha_to_coverage set (patch 0014). Only the fragment
     * shaders compile again, so the file holds fragment variants only; the
     * runtime takes only those from it. */
    if (write_mode == WRITE_A2C) {
        v3dp_capture_begin();
        v3d_force_alpha_to_coverage = true;
        drawn = draw_key_states(p, 0);
        v3d_force_alpha_to_coverage = false;
        v3dp_capture_end();
        return drawn && v3dp_write(path, prog, nsrc, stages, sources) == 0;
    }
    if (write_mode != WRITE_WIDE) {
        v3dp_capture_begin();
        drawn = draw_key_states(p, 0);
        v3dp_capture_end();
        if (!drawn || v3dp_write(path, prog, nsrc, stages, sources) != 0)
            return 0;
    }
    /* The 32-bit-sampler variants. After the plain draws the driver
     * compiles only the fragment shaders again (the vertex shaders' keys are
     * the same), so a .v3dw may hold fragment variants only: the runtime
     * takes only those from it. The command line's goes next to the .v3dp. */
    if (write_mode != WRITE_PLAIN && has_samplers(p)) {
        char wpath[512];
        size_t n = strlen(path);
        if (write_mode == WRITE_WIDE)
            snprintf(wpath, sizeof(wpath), "%s", path);
        else if (n > 5 && !strcmp(path + n - 5, ".v3dp") && n < sizeof(wpath))
            snprintf(wpath, sizeof(wpath), "%.*s.v3dw", (int)(n - 5), path);
        else
            return 0;
        v3dp_capture_begin();
        drawn = draw_key_states(p, 1);
        v3dp_capture_end();
        if (!drawn || v3dp_write(wpath, prog, nsrc, stages, sources) != 0)
            return 0;
    } else if (write_mode == WRITE_WIDE) {
        return 0; /* nothing samples: no such file */
    }
    return 1;
}

/* -L: the ES 3.0 and 3.1 limits as Mesa's context answers them, after the
 * limits main() sets: GLSL's gl_Max* constants and the compiler's and
 * linker's checks come from the same ctx->Const, so ogles2.library's queries
 * must answer these values (docs/GLES3_PLAN.md, Appendix B.1). One line
 * each: the enum's name and its values. */
static int print_limits(void)
{
#define L(e, n) { e, #e, n }
    static const struct { GLenum e; const char *name; int n; } limits[] = {
        /* ES 2.0 */
        L(GL_MAX_VERTEX_ATTRIBS, 1), L(GL_MAX_VERTEX_UNIFORM_VECTORS, 1),
        L(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 1), L(GL_MAX_VARYING_VECTORS, 1),
        L(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 1), L(GL_MAX_TEXTURE_IMAGE_UNITS, 1),
        L(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 1), L(GL_MAX_TEXTURE_SIZE, 1),
        L(GL_MAX_CUBE_MAP_TEXTURE_SIZE, 1), L(GL_MAX_RENDERBUFFER_SIZE, 1), L(GL_MAX_VIEWPORT_DIMS, 2),
        L(GL_ALIASED_POINT_SIZE_RANGE, 2), L(GL_ALIASED_LINE_WIDTH_RANGE, 2), L(GL_SUBPIXEL_BITS, 1),
        /* ES 3.0 */
        L(GL_MAX_3D_TEXTURE_SIZE, 1), L(GL_MAX_ARRAY_TEXTURE_LAYERS, 1), L(GL_MAX_TEXTURE_LOD_BIAS, 1),
        L(GL_MAX_DRAW_BUFFERS, 1), L(GL_MAX_COLOR_ATTACHMENTS, 1), L(GL_MAX_SAMPLES, 1),
        L(GL_MAX_ELEMENT_INDEX, 1), L(GL_MAX_ELEMENTS_INDICES, 1), L(GL_MAX_ELEMENTS_VERTICES, 1),
        L(GL_MAX_SERVER_WAIT_TIMEOUT, 1),
        L(GL_MAX_VERTEX_UNIFORM_COMPONENTS, 1), L(GL_MAX_VERTEX_UNIFORM_BLOCKS, 1),
        L(GL_MAX_VERTEX_OUTPUT_COMPONENTS, 1), L(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, 1),
        L(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, 1), L(GL_MAX_FRAGMENT_INPUT_COMPONENTS, 1),
        L(GL_MIN_PROGRAM_TEXEL_OFFSET, 1), L(GL_MAX_PROGRAM_TEXEL_OFFSET, 1),
        L(GL_MAX_UNIFORM_BUFFER_BINDINGS, 1), L(GL_MAX_UNIFORM_BLOCK_SIZE, 1),
        L(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 1), L(GL_MAX_COMBINED_UNIFORM_BLOCKS, 1),
        L(GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, 1), L(GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS, 1),
        L(GL_MAX_VARYING_COMPONENTS, 1), L(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, 1),
        L(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, 1), L(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, 1),
        /* ES 3.1 */
        L(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, 1), L(GL_MAX_COMPUTE_UNIFORM_BLOCKS, 1),
        L(GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS, 1), L(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, 1),
        L(GL_MAX_COMPUTE_UNIFORM_COMPONENTS, 1), L(GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS, 1),
        L(GL_MAX_COMPUTE_ATOMIC_COUNTERS, 1), L(GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS, 1),
        L(GL_MAX_COMPUTE_IMAGE_UNIFORMS, 1), L(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, 1),
        L(GL_MAX_UNIFORM_LOCATIONS, 1), L(GL_MAX_FRAMEBUFFER_WIDTH, 1), L(GL_MAX_FRAMEBUFFER_HEIGHT, 1),
        L(GL_MAX_FRAMEBUFFER_SAMPLES, 1), L(GL_MAX_VERTEX_ATOMIC_COUNTER_BUFFERS, 1),
        L(GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS, 1), L(GL_MAX_COMBINED_ATOMIC_COUNTER_BUFFERS, 1),
        L(GL_MAX_VERTEX_ATOMIC_COUNTERS, 1), L(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, 1),
        L(GL_MAX_COMBINED_ATOMIC_COUNTERS, 1), L(GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, 1),
        L(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, 1), L(GL_MAX_VERTEX_IMAGE_UNIFORMS, 1),
        L(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, 1), L(GL_MAX_COMBINED_IMAGE_UNIFORMS, 1),
        L(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, 1), L(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, 1),
        L(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, 1), L(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, 1),
        L(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, 1), L(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, 1),
        L(GL_MAX_IMAGE_UNITS, 1), L(GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES, 1),
        L(GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET, 1), L(GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET, 1),
        L(GL_MAX_SAMPLE_MASK_WORDS, 1), L(GL_MAX_COLOR_TEXTURE_SAMPLES, 1),
        L(GL_MAX_DEPTH_TEXTURE_SAMPLES, 1), L(GL_MAX_INTEGER_SAMPLES, 1),
        L(GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET, 1), L(GL_MAX_VERTEX_ATTRIB_BINDINGS, 1),
        L(GL_MAX_VERTEX_ATTRIB_STRIDE, 1),
    };
#undef L
    static const GLenum indexed[] = { GL_MAX_COMPUTE_WORK_GROUP_COUNT, GL_MAX_COMPUTE_WORK_GROUP_SIZE };
    unsigned i, k;
    for (i = 0; i < ARRAY_SIZE(limits); i++) {
        GLint64 v[2] = { 0, 0 };
        _mesa_GetInteger64v(limits[i].e, v);
        printf("%s", limits[i].name);
        for (k = 0; k < (unsigned)limits[i].n; k++)
            printf(" %lld", (long long)v[k]);
        printf("\n");
    }
    for (i = 0; i < ARRAY_SIZE(indexed); i++) {
        printf("%s", i ? "GL_MAX_COMPUTE_WORK_GROUP_SIZE" : "GL_MAX_COMPUTE_WORK_GROUP_COUNT");
        for (k = 0; k < 3; k++) {
            GLint64 v = 0;
            _mesa_GetInteger64i_v(indexed[i], k, &v);
            printf(" %lld", (long long)v);
        }
        printf("\n");
    }
    if (_mesa_GetError() != GL_NO_ERROR) {
        fprintf(stderr, "v3dglslc: a limit query failed\n");
        return 20;
    }
    return 0;
}

#ifdef __amigaos__
/* A request goes back to its sender; one without a reply port (a QUIT sent
 * from ogles2.library's expunge, which cannot wait) is ours to free. */
static void done(struct V3DGLSLRequest *r)
{
    if (r->msg.mn_ReplyPort)
        ReplyMsg(&r->msg);
    else
        FreeVec(r);
}

/* The on-target compiler (plan 8.4): serve V3DGLSL_LINK requests on a
 * public port until V3DGLSL_QUIT. One context for all of them; each request
 * deletes its GL objects again. A second server started by a race finds the
 * port taken and leaves. */
static int serve(void)
{
    struct MsgPort *port;
    struct V3DGLSLRequest *r;
    int quit = 0, served = 0;

    Forbid();
    if (FindPort((CONST_STRPTR)V3DGLSL_PORT_NAME)) {
        Permit();
        fprintf(stderr, "v3dglslc: a compiler server is already running\n");
        return 0;
    }
    if (!(port = CreateMsgPort())) {
        Permit();
        return 20;
    }
    port->mp_Node.ln_Name = (char *)V3DGLSL_PORT_NAME;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();
    fprintf(stderr, "v3dglslc: serving on \"%s\" (%s)\n", V3DGLSL_PORT_NAME, v3dglslc_stamp);

    while (!quit) {
        WaitPort(port);
        while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
            if (r->log && r->log_size)
                r->log[0] = 0;
            if (r->version != V3DGLSL_VERSION ||
                (r->op != V3DGLSL_LINK && r->op != V3DGLSL_QUIT && r->op != V3DGLSL_COMPILE &&
                 r->op != V3DGLSL_LINK32 && r->op != V3DGLSL_LINK_BOUND && r->op != V3DGLSL_LINK_A2C &&
                 r->op != V3DGLSL_COMPILE_VARIANT) ||
                ((r->op == V3DGLSL_LINK || r->op == V3DGLSL_LINK32 || r->op == V3DGLSL_LINK_BOUND ||
                  r->op == V3DGLSL_LINK_A2C || r->op == V3DGLSL_COMPILE_VARIANT) &&
                 (!r->vs || !r->fs || !r->out_path)) ||
                (r->op == V3DGLSL_LINK_BOUND && !r->bindings) ||
                (r->op == V3DGLSL_COMPILE_VARIANT && (!r->keys || !r->keys_len)) ||
                (r->op == V3DGLSL_COMPILE && !r->vs == !r->fs)) {
                log_add(r->log, r->log_size, "the compiler does not understand this request", NULL);
                r->result = 3;
            } else if (r->op == V3DGLSL_QUIT) {
                r->result = 0;
                quit = 1;
            } else if (r->op == V3DGLSL_COMPILE) {
                /* The front end only: GL_COMPILE_STATUS and the shader's info log. */
                int ok;
                GLuint s = compile_src(r->vs ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER,
                                       r->vs ? "vertex shader" : "fragment shader",
                                       r->vs ? r->vs : r->fs, &ok, NULL, 0);
                char info[2048];
                info[0] = 0;
                glGetShaderInfoLog(s, sizeof(info), NULL, info);
                log_add(r->log, r->log_size, info, NULL);
                glDeleteShader(s);
                r->result = ok ? 0 : 1;
            } else {
                write_mode = r->op == V3DGLSL_LINK32 ? WRITE_WIDE : r->op == V3DGLSL_LINK_A2C ? WRITE_A2C
                                                                                  : WRITE_PLAIN;
                link_bindings = r->op == V3DGLSL_LINK_BOUND || r->op == V3DGLSL_COMPILE_VARIANT ? r->bindings
                                                                                              : NULL;
                /* The draw-time variants: write_program compiles the keys. */
                if (r->op == V3DGLSL_COMPILE_VARIANT) {
                    variant_keys = r->keys;
                    variant_keys_len = r->keys_len;
                    variant_layout_crc = r->layout_crc;
                }
                r->result = link_pair("vertex shader", "fragment shader", r->vs, r->fs, r->out_path,
                                      r->log, r->log_size) ? 0 : 1;
                link_bindings = NULL;
                variant_keys = NULL;
                variant_keys_len = 0;
                variant_layout_crc = 0;
                served++;
            }
            done(r);
        }
    }
    /* Gone from the list first, so nobody queues more; then fail what came. */
    Forbid();
    RemPort(port);
    Permit();
    while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
        log_add(r->log, r->log_size, "the compiler is shutting down", NULL);
        r->result = 2;
        done(r);
    }
    DeleteMsgPort(port);
    fprintf(stderr, "v3dglslc: server done, %d program(s)\n", served);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    int first = 1;
    const char *debug = NULL;
    char debug_flags[256];
    driOptionCache info, cache;
    struct pipe_screen_config config;
    struct pipe_screen *screen;
    struct pipe_frontend_screen fscreen;
    struct st_context_attribs attribs;
    enum st_context_error err = ST_CONTEXT_SUCCESS;
    struct st_context *st;

#ifdef __amigaos__
    /* libnix sends stderr to the console, not into the shell's ">file"
     * redirection (seen on the A1200; vamos merges them). Everything here and
     * Mesa's own log (the QPU listing) is written to stderr, so make it the
     * stdout stream: one ordered stream that the redirection captures. */
    __sF[2] = __sF[1];
    /* Line-buffered, so a failed assertion's message (abort() skips the
     * final flush) and everything before it reach the file. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#else
    /* Every compile must reach the driver (and the .v3dp hook). */
    set_option("MESA_SHADER_CACHE_DISABLE", "true");
#endif

#ifdef __amigaos__
    int server = argc == 2 && !strcmp(argv[1], "SERVER");
#endif
    int limits = argc == 2 && !strcmp(argv[1], "-L");
    while (first + 1 < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "-d"))
            debug = argv[first + 1];
        else if (!strcmp(argv[first], "-o"))
            out_dir = argv[first + 1];
        else if (!strcmp(argv[first], "-a")) { /* the .v3da instead (no argument) */
            write_mode = WRITE_A2C;
            first++;
            continue;
        } else if (!strcmp(argv[first], "-x")) {
            FILE *kf = fopen(argv[first + 1], "rb");
            long kn;
            uint8_t *kb;
            if (!kf) {
                fprintf(stderr, "v3dglslc: cannot read %s\n", argv[first + 1]);
                return 20;
            }
            fseek(kf, 0, SEEK_END);
            kn = ftell(kf);
            fseek(kf, 0, SEEK_SET);
            kb = malloc(kn > 0 ? (size_t)kn : 1);
            if (!kb || fread(kb, 1, (size_t)kn, kf) != (size_t)kn) {
                fprintf(stderr, "v3dglslc: cannot read %s\n", argv[first + 1]);
                return 20;
            }
            fclose(kf);
            variant_keys = kb;
            variant_keys_len = (size_t)kn;
        } else
            break;
        first += 2;
    }
    if (debug) {
        snprintf(debug_flags, sizeof(debug_flags), "%s", debug);
        set_option("V3D_DEBUG", debug_flags);
    }

    memset(&info, 0, sizeof(info));
    memset(&cache, 0, sizeof(cache));
    driParseOptionInfo(&info, v3d_driconf, ARRAY_SIZE(v3d_driconf));
    memset(&config, 0, sizeof(config));
    config.options = &cache;
    config.options_info = &info;

    screen = v3d_screen_create(fake_drm_open(), &config, NULL);
    if (!screen) {
        fprintf(stderr, "v3dglslc: v3d_screen_create failed\n");
        return 20;
    }

    memset(&fscreen, 0, sizeof(fscreen));
    fscreen.screen = screen;
    memset(&attribs, 0, sizeof(attribs));
    attribs.profile = API_OPENGLES2;
    attribs.major = 2;
    attribs.minor = 0;
    st = st_api_create_context(&fscreen, &attribs, &err, NULL);
    if (!st) {
        fprintf(stderr, "v3dglslc: st_api_create_context failed (%d)\n", (int)err);
        return 20;
    }
    if (!st_api_make_current(st, NULL, NULL)) {
        fprintf(stderr, "v3dglslc: st_api_make_current failed\n");
        return 20;
    }
    the_pipe = st->pipe;
    /* ogles2.library's limits, which its GL queries report too (GLSL's
     * gl_Max* constants must equal them: dEQP-GLES2 builtin_variable): four
     * draw buffers (EXT_draw_buffers, M7 step 5; OGLES2_MAX_DRAW_BUFFERS), 16
     * texture units in each stage (OGLES2_MAX_STAGE_SAMPLERS; the driver
     * offers 24) and 48 in all (OGLES2_MAX_TEXTURE_UNITS: ES 3.1's minimum,
     * in every context version, so a GLSL ES 1.00 program's file is the same
     * in both, docs/GLES3_PLAN.md §2; the driver offers 96). */
    {
        GET_CURRENT_CONTEXT(ctx);
        ctx->Const.MaxDrawBuffers = 4;
        ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 16;
        ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 16;
        ctx->Const.Program[MESA_SHADER_COMPUTE].MaxTextureImageUnits = 16;
        ctx->Const.MaxCombinedTextureImageUnits = 48;
        /* Not GLSL's, but kept equal so -L lists exactly what ogles2.library
         * answers: indices up to 2^24 - 1 (ES 3.0's minimum; the draws rebase
         * indices on the CPU), no server-side wait (glWaitSync waits for
         * nothing the CPU must), an LOD bias up to 15 (texture.c's
         * MAX_LOD_BIAS; the driver offers 16). */
        ctx->Const.MaxElementIndex = 0xffffff;
        ctx->Const.MaxServerWaitTimeout = 0;
        ctx->Const.MaxTextureLodBias = 15.0f;
    }
    if (limits)
        return print_limits();
    fprintf(stderr, "v3dglslc: renderer %s\n", (const char *)_mesa_GetString(GL_RENDERER));
#ifdef __amigaos__
    if (server)
        return serve();
#endif
    return run_pairs(argc, argv, first);
}
