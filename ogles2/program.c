/*
 * Shader and program objects (GLES 2.0 section 2.10), attribute and uniform
 * locations, and uniform storage (plan section 7.8).
 *
 * glCompileShader keeps the source and succeeds (S1, plan 7.8). glLinkProgram
 * hashes the sources (v3dp/FORMAT.md, PROGRAM: the vertex shader, then the
 * fragment shader) and loads <hash>.v3dp from PROGDIR:shaders/, PROGDIR: or OGLES2Cache:, the
 * name being the hash's first 24 hex digits (FFS allows 30 characters); the
 * full hash in the file must match. A
 * program with no binary fails to link; its sources go to
 * T:ogles2_missing_shaders.txt for the offline compiler; with the on-target
 * compiler present (LIBS:v3dglslc or beside the program, compiler.c) the program is compiled
 * into the disk cache first. Loaded images stay
 * in the context's program cache, by hash, until the context goes: a
 * relink, or another program with the same sources, reuses them (plan 7.8).
 *
 * Uniform storage is one constant buffer per stage, as in Mesa: glUniform*
 * writes each value into every stage that uses it (the .v3dp's storage
 * places), and the uniform recipes read from there at draw time.
 *
 * One image is built in: PiStorm3D's smooth-shaded shaders (builtin_shaders.h)
 * with a hand-written description, used for clears drawn as rectangles.
 *
 * Licence: this file adapts PiStorm3D's code, so it is under the Hyperion
 * MiniGL Open Source License (LICENSE, section 1).
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "ogles2_internal.h"
#include "builtin_shaders.h"
#include "blake3.h"
#include "../v3dp/v3dglsl_port.h" /* V3DGLSL_SEPA_MARK */

/* ---- the built-in image ---- */

static const struct OGLES2ImageAttrib smooth_attribs[] = {
    {"a_position", GL_FLOAT_VEC3, 0, 1, 0, 0xff}, /* VPM inputs 0-2, both shaders; no location */
    {"a_color", GL_FLOAT_VEC4, 1, 1, 0, 0xff},    /* VPM inputs 3-6, vertex shader only */
};

static const struct OGLES2Place smooth_mvp_place[] = {{0, 16, 4, 0, 0}};

static const struct OGLES2ImageUniform smooth_uniforms[] = {
    {"u_mvp", GL_FLOAT_MAT4, 0, 0, 1, smooth_mvp_place, {~0UL, ~0UL}, -1, 0, 0, 0, 0}, /* in no block */
};

/* The vertex and coordinate shaders read the same stream (v3d_assembler.c:
 * scale_p, scale_p_y, the matrix, then the depth range's scale and offset,
 * which the coordinate shader does not reach). The matrix goes row by row:
 * the shader forms x' = x M00 + y M10 + z M20 + w M30 from the first four
 * words, so they are row 0 of the column-major GL matrix (words 0, 4, 8, 12). */
static const ULONG smooth_rtype[20] = {
    OGLES2_U_PISTORM_VP_WIDTH, OGLES2_U_PISTORM_VP_HEIGHT,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    OGLES2_U_VIEWPORT_Z_SCALE, OGLES2_U_VIEWPORT_Z_OFFSET,
};
static const ULONG smooth_rdata[20] = {0, 0, 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15, 0, 0};

/* The record values PiStorm3D's d_BuildShaderRecord uses for this pairing,
 * expressed as prog_data: 4 threads, the vertex and coordinate shaders start
 * in their final thread section, VPM output segments of 1 (coordinate) and 2
 * (vertex), a VCM cache of 4 batches, four varyings using the pixel centre w. */
static const struct OGLES2Variant smooth_variants[3] = {
    {OGLES2_VARIANT_CS, 4, 1, 0, 0, 4, 0, 0, 0, {3}, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     {0}, {0}, {0}, 0, {0}, 0, 0, builtin_coordinate, sizeof(builtin_coordinate), 0, 20, smooth_rtype,
     smooth_rdata, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {OGLES2_VARIANT_VS, 4, 1, 0, 0, 4, 0, 0, 0, {3, 4}, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     {0}, {0}, {0}, 0, {0}, 0, 0, builtin_vertex_smooth, sizeof(builtin_vertex_smooth),
     sizeof(builtin_coordinate), 20, smooth_rtype, smooth_rdata, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {OGLES2_VARIANT_FS, 4, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0,
     {0}, {0}, {0}, 0, {0}, 0, 0, builtin_fragment_smooth, sizeof(builtin_fragment_smooth),
     sizeof(builtin_coordinate) + sizeof(builtin_vertex_smooth), 0, 0, 0, 0, 0, 0, 0, 0,
     1 /* render target 0 */, 0, 0, 0, 0, 0},
};

static const struct OGLES2Image builtin_smooth = {
    "built-in smooth colour (PiStorm3D VERTEX_SMOOTH, FRAGMENT_UNTEXTURED_SMOOTH)",
    {0},
    3, smooth_variants,
    2, smooth_attribs,
    1, smooth_uniforms,
    {16, 0}, {0, 0},
    0, 0,
    sizeof(builtin_coordinate) + sizeof(builtin_vertex_smooth) + sizeof(builtin_fragment_smooth),
    0,          /* no OGLES2_U_CLIP_Z_COEFF: clears are drawn NEGATIVE_ONE_TO_ONE */
    0, 0, 0,    /* code memory (draw.c uploads it per context), not borrowed, allocations */
    0, 0,       /* no binary */
    0, 0,       /* no 32-bit-sampler variants */
    0, 0,       /* no alpha-to-coverage variants */
    0, 0,       /* no sources */
    0,          /* in no program cache */
    0, 0, 0, {0, 0}, {0, 0}, 0, 0, /* format 1.0: no uniform blocks or interface */
    0, 0, 0, 0, 0, 0, 0, 0, 0,     /* no draw-time variants */
    0,                             /* no transform feedback */
    0, 0, 0, 0, 0, {0, 0}, {0, 0}, /* not a compute or separable program, no storage blocks */
    0, 0,                          /* no atomic counter buffers */
    0, 0,                          /* locations numbered by the runtime */
    0, 0,                          /* samplers start at unit 0 */
    0,                             /* no interface types */
};

const struct OGLES2Image *ogles2_builtin_image(void)
{
    return &builtin_smooth;
}

/* ---- small string helpers (the library has no C library) ---- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* <0, 0, >0 as strcmp (bytes unsigned). */
static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (int)(UBYTE)*a - (int)(UBYTE)*b;
}

static ULONG str_len(const char *s)
{
    ULONG n = 0;
    while (s[n])
        n++;
    return n;
}

/* Copies src into dst[size] like glGet*InfoLog: truncated, NUL-terminated. */
static GLsizei copy_out(char *dst, GLsizei size, const char *src)
{
    GLsizei n = 0;
    if (!dst || size <= 0)
        return 0;
    while (src && src[n] && n < size - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

/* ---- the name table ---- */

void *ogles2_object(struct OGLES2Context *ctx, GLuint name, UBYTE kind)
{
    UBYTE *o;
    if (name == 0 || name >= ctx->share->nobjects)
        return 0;
    o = ctx->share->objects[name];
    return (o && *o == kind) ? o : 0;
}

static BOOL name_exists(struct OGLES2Context *ctx, GLuint name)
{
    return name && name < ctx->share->nobjects && ctx->share->objects[name];
}

static GLuint new_name(struct OGLES2Context *ctx, void *obj)
{
    GLuint i;
    for (i = 1; i < ctx->share->nobjects; i++)
        if (!ctx->share->objects[i]) {
            ctx->share->objects[i] = obj;
            return i;
        }
    {
        GLuint n = ctx->share->nobjects ? ctx->share->nobjects * 2 : 16;
        void **t = AllocVec(n * sizeof(void *), MEMF_ANY | MEMF_CLEAR);
        if (!t)
            return 0;
        for (i = 0; i < ctx->share->nobjects; i++)
            t[i] = ctx->share->objects[i];
        if (ctx->share->objects)
            FreeVec(ctx->share->objects);
        i = ctx->share->nobjects ? ctx->share->nobjects : 1;
        ctx->share->objects = t;
        ctx->share->nobjects = n;
        ctx->share->objects[i] = obj;
        return i;
    }
}

static void free_shader(struct OGLES2Context *ctx, struct OGLES2Shader *s)
{
    ctx->share->objects[s->name] = 0;
    if (s->source)
        FreeVec(s->source);
    if (s->info_log)
        FreeVec(s->info_log);
    FreeVec(s);
}

/* A shader marked for deletion goes once no program holds it. */
static void release_shader(struct OGLES2Context *ctx, struct OGLES2Shader *s)
{
    if (!s)
        return;
    s->attached--;
    if (s->delete_pending && !s->attached)
        free_shader(ctx, s);
}

static void unlink_program(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    int k;
    for (k = 0; k < 2; k++) {
        if (p->cbuf[k])
            FreeVec(p->cbuf[k]);
        if (p->cbuf_le[k])
            FreeVec(p->cbuf_le[k]);
        p->cbuf[k] = p->cbuf_le[k] = 0;
    }
    if (p->loc_uniform)
        FreeVec(p->loc_uniform);
    if (p->sampler_unit)
        FreeVec(p->sampler_unit);
    if (p->uniform_loc)
        FreeVec(p->uniform_loc);
    p->uniform_loc = 0;
    if (p->block_binding)
        FreeVec(p->block_binding);
    p->block_binding = 0;
    p->loc_uniform = p->loc_element = 0;
    p->sampler_unit = 0;
    p->nlocations = 0;
    if (p->rec_tmpl)
        FreeVec(p->rec_tmpl);
    p->rec_tmpl = 0;
    p->var_ok = FALSE;
    p->nsamplers = 0xff;
    p->image = 0; /* images belong to the context's cache */
    p->linked = GL_FALSE;
    (void)ctx;
}

void ogles2_cbuf_set(struct OGLES2Program *p, int stage, ULONG at, ULONG w)
{
    p->cbuf_ver[stage]++; /* draw.c's kept uniform streams are made again */
    p->cbuf[stage][at] = w;
    p->cbuf_le[stage][at] = (w >> 24) | ((w >> 8) & 0xff00UL) | ((w << 8) & 0xff0000UL) | (w << 24);
}

static void set_log(struct OGLES2Program *p, const char *a, const char *b)
{
    ULONG la = str_len(a), lb = b ? str_len(b) : 0;
    if (p->info_log)
        FreeVec(p->info_log);
    p->info_log = AllocVec(la + lb + 1, MEMF_ANY);
    if (p->info_log) {
        memcpy(p->info_log, a, la);
        if (lb)
            memcpy(p->info_log + la, b, lb);
        p->info_log[la + lb] = 0;
    }
}

static void free_program(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    ctx->share->objects[p->name] = 0;
    release_shader(ctx, p->vs);
    release_shader(ctx, p->fs);
    release_shader(ctx, p->cs);
    unlink_program(ctx, p);
    if (p->info_log)
        FreeVec(p->info_log);
    if (p->xfb_names)
        FreeVec(p->xfb_names);
    FreeVec(p);
}

/* Is p the current program of a member of ctx's share group? */
static BOOL in_use(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    struct OGLES2Context *m;
    GLuint i;
    int k;
    for (m = ctx->share->first; m; m = m->share_next) {
        if (m->program == p || ogles2_xfb_program_busy(m, p)) /* or an active transform feedback's */
            return TRUE;
        for (i = 0; i < m->npipes; i++) /* or a program pipeline's (ES 3.1) */
            if (m->pipes[i]) {
                for (k = 0; k < 3; k++)
                    if (m->pipes[i]->stage[k] == p)
                        return TRUE;
                if (m->pipes[i]->active == p)
                    return TRUE;
            }
    }
    return FALSE;
}

static void pipelines_free(struct OGLES2Context *ctx);

/* A transform feedback object let go of p (xfb.c): freed if deleted and
 * nothing else uses it. */
void ogles2_program_unref(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    if (p && p->delete_pending && !in_use(ctx, p))
        free_program(ctx, p);
}

/* aglDestroyContext (its jobs finished): its current program let go of; the
 * shaders, programs and program cache with the share group's last member. */
void ogles2_objects_free(struct OGLES2Context *ctx)
{
    struct OGLES2Program *old = ctx->program;
    GLuint i;
    UBYTE *o;
    ctx->program = 0;
    pipelines_free(ctx); /* the programs they hold let go of, then as the current one */
    if (ctx->share->members > 1) {
        if (old && old->delete_pending && !in_use(ctx, old))
            free_program(ctx, old);
        return;
    }
    for (i = 1; i < ctx->share->nobjects; i++) /* programs first: they hold shaders */
        if ((o = ctx->share->objects[i]) && *o == OGLES2_OBJ_PROGRAM)
            free_program(ctx, (struct OGLES2Program *)o);
    for (i = 1; i < ctx->share->nobjects; i++)
        if ((o = ctx->share->objects[i]))
            free_shader(ctx, (struct OGLES2Shader *)o);
    if (ctx->share->objects)
        FreeVec(ctx->share->objects);
    ctx->share->objects = 0;
    ctx->share->nobjects = 0;
    while (ctx->share->images) {
        struct OGLES2Image *im = ctx->share->images;
        ctx->share->images = im->next;
        ogles2_image_free(im, ctx);
    }
}

/* Looks a name up for a call that wants `kind`: GL_INVALID_VALUE for no such
 * object, GL_INVALID_OPERATION for an object of the other kind. */
static void *lookup(struct OGLES2Context *ctx, GLuint name, UBYTE kind)
{
    void *o = ogles2_object(ctx, name, kind);
    if (!o)
        ogles2_error(ctx, name_exists(ctx, name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
    return o;
}

/* ---- shaders ---- */

GLuint ogles2_glCreateShader(struct OGLES2Context *ctx, GLenum type)
{
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx))
        return 0;
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER &&
        !(type == GL_COMPUTE_SHADER && ogles2_es31(ctx))) { /* ES 3.1's compute shaders */
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (!s || !(s->name = new_name(ctx, s))) {
        if (s)
            FreeVec(s);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return 0;
    }
    s->kind = OGLES2_OBJ_SHADER;
    s->type = type;
    return s->name;
}

void ogles2_glDeleteShader(struct OGLES2Context *ctx, GLuint shader)
{
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx) || shader == 0)
        return;
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    s->delete_pending = TRUE;
    if (!s->attached)
        free_shader(ctx, s);
}

GLboolean ogles2_glIsShader(struct OGLES2Context *ctx, GLuint shader)
{
    if (!ogles2_context_valid(ctx))
        return GL_FALSE;
    return ogles2_object(ctx, shader, OGLES2_OBJ_SHADER) ? GL_TRUE : GL_FALSE;
}

void ogles2_glShaderSource(struct OGLES2Context *ctx, GLuint shader, GLsizei count,
                           const GLchar *const *string, const GLint *length)
{
    struct OGLES2Shader *s;
    ULONG total = 0, n;
    GLsizei i;
    char *src;
    if (!ogles2_context_valid(ctx))
        return;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    for (i = 0; i < count; i++)
        if (string[i])
            total += (length && length[i] >= 0) ? (ULONG)length[i] : str_len(string[i]);
    if (!(src = AllocVec(total + 1, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    total = 0;
    for (i = 0; i < count; i++) {
        if (!string[i])
            continue;
        n = (length && length[i] >= 0) ? (ULONG)length[i] : str_len(string[i]);
        memcpy(src + total, string[i], n);
        total += n;
    }
    src[total] = 0;
    if (s->source)
        FreeVec(s->source);
    s->source = src;
}

static void shader_record_path(char *path, const char *dir, const struct OGLES2Shader *s);
static int read_record(const char *path, ULONG compiler, char **log);
static void write_record(const char *path, const char *log);
static void write_fail_record(const char *path, ULONG compiler, const char *log);
static char *dup_text(const char *t, ULONG n);

/* The GLSL ES version a source asks for: its #version line, which only
 * whitespace and comments may precede (GLSL ES 3.00 3.4), as major x 100 +
 * minor (300 for "300 es"); 100 without one; 0 for a line this does not
 * understand (the compiler then says what is wrong). */
static ULONG glsl_version(const char *p)
{
    ULONG v = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
        } else if (p[0] == '/' && p[1] == '*') {
            for (p += 2; *p && !(p[0] == '*' && p[1] == '/'); p++)
                ;
            if (*p)
                p += 2;
        } else {
            break;
        }
    }
    if (*p != '#')
        return 100;
    for (p++; *p == ' ' || *p == '\t'; p++)
        ;
    if (strncmp(p, "version", 7) || (p[7] != ' ' && p[7] != '\t'))
        return 100; /* another directive first: no #version */
    for (p += 7; *p == ' ' || *p == '\t'; p++)
        ;
    while (*p >= '0' && *p <= '9' && v < 10000)
        v = v * 10 + (ULONG)(*p++ - '0');
    return v;
}

/* With the on-target compiler (plan 8.4) a shader is compiled here, for
 * its GL_COMPILE_STATUS and info log; its record in the disk cache
 * (shader_record) answers for it next time without the compiler. Without
 * the compiler (S1, plan 7.8) the source is only kept and linking does the
 * real work. */
void ogles2_glCompileShader(struct OGLES2Context *ctx, GLuint shader)
{
    struct OGLES2Shader *s;
    char path[96], *log;
    ULONG compiler, version;
    int rc;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    if (s->info_log)
        FreeVec(s->info_log);
    s->info_log = 0;
    s->compiled = GL_FALSE;
    if (!s->source)
        return;
    /* The GLSL ES versions the context's version allows (docs/GLES3_PLAN.md
     * §1, §3): 1.00 in every context, 3.00 in ES 3 contexts, 3.10 in ES 3.1
     * ones. The compiler's Mesa context accepts them all, so this is the
     * library's own check, before any record or compiler, and so also
     * without the compiler. */
    version = glsl_version(s->source);
    if ((version == 300 && !ogles2_es3(ctx)) || (version == 310 && ctx->es_version < 31)) {
        static const char *const msg[2] = {"ERROR: 0:1: GLSL ES 3.00 needs an OpenGL ES 3 context\n",
                                           "ERROR: 0:1: GLSL ES 3.10 needs an OpenGL ES 3.1 context\n"};
        const char *m = msg[version == 310];
        s->info_log = dup_text(m, str_len(m));
        return;
    }
    if (!(compiler = ogles2_compiler_id())) {
        s->compiled = GL_TRUE;
        return;
    }
    /* A record: compiled before, or failed with this compiler (gl4es tries
     * shaders for optional features at every start that fail by design: the
     * compiler need not be started for them, about a second). A program may
     * ship its records beside its binaries, in its shaders/ drawer; the
     * compiler's go to the disk cache. */
    shader_record_path(path, "PROGDIR:shaders/", s);
    rc = read_record(path, compiler, &s->info_log);
    if (rc < 0) {
        shader_record_path(path, ogles2_cache_dir(), s);
        rc = read_record(path, compiler, &s->info_log);
    }
    if (rc >= 0) {
        s->compiled = rc ? GL_TRUE : GL_FALSE;
        return;
    }
    if (!(log = AllocVec(4096, MEMF_ANY | MEMF_CLEAR))) {
        s->compiled = GL_TRUE; /* checked again at link */
        return;
    }
    rc = s->type == GL_COMPUTE_SHADER ? ogles2_compile_compute_shader(s->source, log, 4096)
                                      : ogles2_compile_shader(s->type == GL_FRAGMENT_SHADER, s->source, log, 4096);
    if (rc < 0) {
        s->compiled = GL_TRUE; /* no compiler after all: as S1, the link decides */
    } else {
        s->compiled = rc ? GL_TRUE : GL_FALSE;
        if (log[0])
            s->info_log = dup_text(log, str_len(log));
        if (rc)
            write_record(path, log);
        else
            write_fail_record(path, compiler, log);
    }
    FreeVec(log);
}

void ogles2_glGetShaderiv(struct OGLES2Context *ctx, GLuint shader, GLenum pname, GLint *params)
{
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    switch (pname) {
    case GL_SHADER_TYPE:          *params = (GLint)s->type; break;
    case GL_DELETE_STATUS:        *params = s->delete_pending ? GL_TRUE : GL_FALSE; break;
    case GL_COMPILE_STATUS:       *params = s->compiled; break;
    case GL_INFO_LOG_LENGTH:      *params = s->info_log ? (GLint)str_len(s->info_log) + 1 : 0; break;
    case GL_SHADER_SOURCE_LENGTH: *params = s->source ? (GLint)str_len(s->source) + 1 : 0; break;
    default:                      ogles2_error(ctx, GL_INVALID_ENUM); break;
    }
}

void ogles2_glGetShaderInfoLog(struct OGLES2Context *ctx, GLuint shader, GLsizei bufSize,
                               GLsizei *length, GLchar *infoLog)
{
    struct OGLES2Shader *s;
    GLsizei n;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    n = copy_out(infoLog, bufSize, s->info_log ? s->info_log : "");
    if (length)
        *length = n;
}

void ogles2_glGetShaderSource(struct OGLES2Context *ctx, GLuint shader, GLsizei bufSize,
                              GLsizei *length, GLchar *source)
{
    struct OGLES2Shader *s;
    GLsizei n;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    n = copy_out(source, bufSize, s->source ? s->source : "");
    if (length)
        *length = n;
}

/* ---- programs ---- */

GLuint ogles2_glCreateProgram(struct OGLES2Context *ctx)
{
    struct OGLES2Program *p;
    if (!ogles2_context_valid(ctx))
        return 0;
    p = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR);
    if (!p || !(p->name = new_name(ctx, p))) {
        if (p)
            FreeVec(p);
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return 0;
    }
    p->kind = OGLES2_OBJ_PROGRAM;
    return p->name;
}

void ogles2_glDeleteProgram(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p;
    if (!ogles2_context_valid(ctx) || program == 0)
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    p->delete_pending = TRUE;
    if (!in_use(ctx, p)) /* a current program goes when it stops being current, in every member */
        free_program(ctx, p);
}

GLboolean ogles2_glIsProgram(struct OGLES2Context *ctx, GLuint program)
{
    if (!ogles2_context_valid(ctx))
        return GL_FALSE;
    return ogles2_object(ctx, program, OGLES2_OBJ_PROGRAM) ? GL_TRUE : GL_FALSE;
}

void ogles2_glAttachShader(struct OGLES2Context *ctx, GLuint program, GLuint shader)
{
    struct OGLES2Program *p;
    struct OGLES2Shader *s, **slot;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)) ||
        !(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    slot = s->type == GL_VERTEX_SHADER ? &p->vs : s->type == GL_COMPUTE_SHADER ? &p->cs : &p->fs;
    if (*slot) { /* already attached, or one of that type already is */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    *slot = s;
    s->attached++;
}

void ogles2_glDetachShader(struct OGLES2Context *ctx, GLuint program, GLuint shader)
{
    struct OGLES2Program *p;
    struct OGLES2Shader *s;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)) ||
        !(s = lookup(ctx, shader, OGLES2_OBJ_SHADER)))
        return;
    if (p->vs == s)
        p->vs = 0;
    else if (p->fs == s)
        p->fs = 0;
    else if (p->cs == s)
        p->cs = 0;
    else {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    release_shader(ctx, s);
}

void ogles2_glGetAttachedShaders(struct OGLES2Context *ctx, GLuint program, GLsizei maxCount,
                                 GLsizei *count, GLuint *shaders)
{
    struct OGLES2Program *p;
    GLsizei n = 0;
    if (!ogles2_context_valid(ctx))
        return;
    if (maxCount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (p->vs && n < maxCount)
        shaders[n++] = p->vs->name;
    if (p->fs && n < maxCount)
        shaders[n++] = p->fs->name;
    if (p->cs && n < maxCount)
        shaders[n++] = p->cs->name;
    if (count)
        *count = n;
}

void ogles2_glBindAttribLocation(struct OGLES2Context *ctx, GLuint program, GLuint index,
                                 const GLchar *name)
{
    struct OGLES2Program *p;
    int i;
    if (!ogles2_context_valid(ctx) || !name)
        return;
    if (index >= OGLES2_MAX_VERTEX_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    for (i = 0; i < p->nbindings; i++)
        if (str_eq(p->bind_name[i], name))
            break;
    if (i == p->nbindings) {
        if (p->nbindings == OGLES2_MAX_BINDINGS || str_len(name) >= sizeof(p->bind_name[0])) {
            ogles2_log("glBindAttribLocation: \"%s\" not kept (too many or too long)", (ULONG)name);
            return;
        }
        p->nbindings++;
    }
    copy_out(p->bind_name[i], sizeof(p->bind_name[i]), name);
    p->bind_index[i] = index;
}

/* ---- linking ---- */

/* GLES 2.0 2.10.4: two attribute names bound to one location (aliasing),
 * which links only when Mesa's linker applies the bindings (else it gives
 * every active attribute a location of its own, and more than 16 fail). The
 * program's bindings then, "<index> <name>\n" lines sorted by name
 * (AllocVec'd); 0 for a program without aliasing (its bindings are applied
 * at runtime, by name: the binary does not depend on them). */
static char *aliased_bindings(const struct OGLES2Program *p)
{
    UBYTE order[OGLES2_MAX_BINDINGS];
    int i, j, n = p->nbindings;
    BOOL alias = FALSE;
    ULONG size = 1, at = 0;
    char *t;
    for (i = 0; i < n && !alias; i++)
        for (j = i + 1; j < n; j++)
            if (p->bind_index[i] == p->bind_index[j]) {
                alias = TRUE;
                break;
            }
    if (!alias)
        return 0;
    for (i = 0; i < n; i++) { /* insertion sort by name */
        for (j = i; j > 0 && str_cmp(p->bind_name[order[j - 1]], p->bind_name[i]) > 0; j--)
            order[j] = order[j - 1];
        order[j] = (UBYTE)i;
        size += 12 + str_len(p->bind_name[i]);
    }
    if (!(t = AllocVec(size, MEMF_ANY)))
        return 0;
    for (i = 0; i < n; i++)
        at += ogles2_format(t + at, size - at, "%lu %s\n", (ULONG)p->bind_index[order[i]],
                            (ULONG)p->bind_name[order[i]]);
    t[at] = 0;
    return t;
}

#define XFBV_MARK "XFBV\n"

/* The transform feedback part of link state `t` (after its "XFBV" line), or 0. */
static const char *xfbv_part(const char *t)
{
    const char *at = t;
    while (at && *at) {
        if (!strncmp(at, XFBV_MARK, 5))
            return at + 5;
        while (*at && *at != '\n')
            at++;
        if (*at)
            at++;
    }
    return 0;
}

static char *xfb_state(const struct OGLES2Program *p);

/* The link state the compiler applies before it links and the program hash
 * covers (v3dp/v3dglsl_port.h's bindings; FORMAT.md): ES 3.1's separable
 * program first, the line "SEPA" (V3DGLSL_SEPA_MARK, which the hash takes
 * as the pseudo-stage "SEPA" after the rest), then aliased bindings
 * (aliased_bindings), then with transform feedback varyings the line "XFBV"
 * and their text: the buffer mode as four lowercase hex digits and "\n",
 * each name and "\n", in the order given. AllocVec'd; 0: none. */
static char *link_state(const struct OGLES2Program *p)
{
    char *b = xfb_state(p), *t;
    ULONG n;
    if (!p->separable)
        return b;
    n = b ? str_len(b) : 0;
    if ((t = AllocVec(sizeof(V3DGLSL_SEPA_MARK) + n, MEMF_ANY))) {
        memcpy(t, V3DGLSL_SEPA_MARK, sizeof(V3DGLSL_SEPA_MARK) - 1);
        if (b)
            memcpy(t + sizeof(V3DGLSL_SEPA_MARK) - 1, b, n);
        t[sizeof(V3DGLSL_SEPA_MARK) - 1 + n] = 0;
    }
    if (b)
        FreeVec(b);
    return t;
}

/* The bindings and transform feedback part of the link state. */
static char *xfb_state(const struct OGLES2Program *p)
{
    static const char digits[] = "0123456789abcdef";
    char *b = aliased_bindings(p), *t;
    ULONG lb, lx;
    GLenum mode = p->xfb_mode ? p->xfb_mode : GL_INTERLEAVED_ATTRIBS;
    int k;
    if (!p->nxfb)
        return b;
    lb = b ? str_len(b) : 0;
    lx = str_len(p->xfb_names);
    if ((t = AllocVec(lb + 10 + lx + 1, MEMF_ANY))) {
        if (b)
            memcpy(t, b, lb);
        memcpy(t + lb, XFBV_MARK, 5);
        for (k = 0; k < 4; k++)
            t[lb + 5 + k] = digits[(mode >> (12 - 4 * k)) & 15];
        t[lb + 9] = '\n';
        memcpy(t + lb + 10, p->xfb_names, lx + 1);
    }
    if (b)
        FreeVec(b);
    return t;
}

/* The program hash (v3dp/FORMAT.md, PROGRAM), as hex: per shader, vertex
 * first, its stage and length as little-endian 32-bit words, then its text;
 * then link state's (link_state) aliased bindings the same way, as the
 * stage V3DP_HASH_BINDINGS, and its transform feedback text as the stage
 * "XFBV". */
static void program_hash(const struct OGLES2Program *p, const char *state, UBYTE out[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    struct blake3 h;
    const char *src[5], *x;
    ULONG stage[5], len[5];
    BOOL sepa = state && !strncmp(state, V3DGLSL_SEPA_MARK, sizeof(V3DGLSL_SEPA_MARK) - 1);
    int k, n = 0;
    if (sepa) {
        state += sizeof(V3DGLSL_SEPA_MARK) - 1;
        if (!*state)
            state = 0;
    }
    x = xfbv_part(state);
    if (p->cs) { /* ES 3.1: a compute program's one shader */
        src[n] = p->cs->source;
        stage[n++] = 5;
    }
    if (!p->cs && p->vs) { /* a separable program may have one stage (ES 3.1) */
        src[n] = p->vs->source;
        stage[n++] = 0;
    }
    if (!p->cs && p->fs) {
        src[n] = p->fs->source;
        stage[n++] = 4;
    }
    for (k = 0; k < n; k++)
        len[k] = str_len(src[k]);
    if (state && (len[n] = x ? (ULONG)(x - 5 - state) : str_len(state)) != 0) {
        src[n] = state;
        stage[n++] = 0x42494E44UL; /* V3DP_HASH_BINDINGS */
    }
    if (x) {
        src[n] = x;
        len[n] = str_len(x);
        stage[n++] = 0x58464256UL; /* "XFBV" */
    }
    if (sepa) {
        src[n] = "1";
        len[n] = 1;
        stage[n++] = 0x53455041UL; /* "SEPA" */
    }
    blake3_init(&h);
    for (k = 0; k < n; k++) {
        ULONG i;
        UBYTE w[8];
        for (i = 0; i < 4; i++) {
            w[i] = (UBYTE)(stage[k] >> (8 * i));
            w[4 + i] = (UBYTE)(len[k] >> (8 * i));
        }
        blake3_update(&h, w, 8);
        blake3_update(&h, src[k], len[k]);
    }
    blake3_final(&h, out);
    for (k = 0; k < 32; k++) {
        hex[2 * k] = digits[out[k] >> 4];
        hex[2 * k + 1] = digits[out[k] & 15];
    }
    hex[64] = 0;
}

/* Reads a whole file; NULL if it does not exist. */
static UBYTE *read_file(const char *path, ULONG *len)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    UBYTE *buf = 0;
    LONG size;
    if (!fh)
        return 0;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) > 0 &&
        size < 16L * 1024 * 1024 && (buf = AllocVec((ULONG)size, MEMF_ANY))) {
        if (Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = 0;
        }
        *len = (ULONG)size;
    }
    Close(fh);
    return buf;
}

/* ---- shader records (plan 8.4): <24 hex digits>.v3ds in the disk cache,
 * one per shader the compiler accepted, holding its info log, or refused,
 * holding its errors. The hash is BLAKE3 over the shader's stage and
 * source, as for a one-shader program; the first line tags the file (a
 * record with no messages is not empty): "v3ds 1", or "v3ds 0 <the
 * compiler's id, 8 hex digits>" (ogles2_compiler_id: another compiler
 * tries the shader again). */
#define RECORD_TAG "v3ds 1\n"
#define FAIL_TAG "v3ds 0 "

/* The record of shader s in drawer dir ("PROGDIR:shaders/", the cache). */
static void shader_record_path(char *path, const char *dir, const struct OGLES2Shader *s)
{
    static const char digits[] = "0123456789abcdef";
    struct blake3 h;
    UBYTE out[32], w[8];
    ULONG stage = s->type == GL_FRAGMENT_SHADER ? 4 : s->type == GL_COMPUTE_SHADER ? 5 : 0, len = str_len(s->source), i;
    char *q;
    blake3_init(&h);
    for (i = 0; i < 4; i++) {
        w[i] = (UBYTE)(stage >> (8 * i));
        w[4 + i] = (UBYTE)(len >> (8 * i));
    }
    blake3_update(&h, w, 8);
    blake3_update(&h, s->source, len);
    blake3_final(&h, out);
    strcpy(path, dir);
    q = path + str_len(path);
    for (i = 0; i < 12; i++) { /* 24 digits + ".v3ds": FFS's 30 characters */
        *q++ = digits[out[i] >> 4];
        *q++ = digits[out[i] & 15];
    }
    strcpy(q, ".v3ds");
}

static char *dup_text(const char *t, ULONG n)
{
    char *d = AllocVec(n + 1, MEMF_ANY);
    if (d) {
        memcpy(d, t, n);
        d[n] = 0;
    }
    return d;
}

static void hex8(char *d, ULONG x)
{
    static const char digits[] = "0123456789abcdef";
    int k;
    for (k = 7; k >= 0; k--, x >>= 4)
        d[k] = digits[x & 15];
    d[8] = 0;
}

/* 1: compiled (its log in *log), 0: refused by `compiler` (its errors), -1:
 * no record, or one of another compiler's refusals. */
static int read_record(const char *path, ULONG compiler, char **log)
{
    ULONG len = 0, tag = str_len(RECORD_TAG), ftag = str_len(FAIL_TAG) + 9;
    UBYTE *file = read_file(path, &len);
    char id[12];
    int rc = -1;
    *log = 0;
    if (!file)
        return -1;
    hex8(id, compiler);
    id[8] = '\n';
    if (len >= tag && !memcmp(file, RECORD_TAG, tag)) {
        rc = 1;
        *log = len > tag ? dup_text((const char *)file + tag, len - tag) : 0;
    } else if (len >= ftag && !memcmp(file, FAIL_TAG, ftag - 9) && !memcmp(file + ftag - 9, id, 9)) {
        rc = 0;
        *log = len > ftag ? dup_text((const char *)file + ftag, len - ftag) : 0;
    }
    FreeVec(file);
    return rc;
}

static void write_fail_record(const char *path, ULONG compiler, const char *log)
{
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    char id[12];
    if (!fh)
        return;
    hex8(id, compiler);
    FPuts(fh, (STRPTR)FAIL_TAG);
    FPuts(fh, (STRPTR)id);
    FPuts(fh, (STRPTR) "\n");
    if (log && log[0])
        FPuts(fh, (STRPTR)log);
    Close(fh);
}

static void write_record(const char *path, const char *log)
{
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh)
        return;
    FPuts(fh, (STRPTR)RECORD_TAG);
    if (log && log[0])
        FPuts(fh, (STRPTR)log);
    Close(fh);
}

static void mark_compiled(const struct OGLES2Shader *s)
{
    char path[96], *log;
    int rc;
    /* Recorded already: shipped with the program, or in the cache. */
    shader_record_path(path, "PROGDIR:shaders/", s);
    rc = read_record(path, ogles2_compiler_id(), &log);
    if (log)
        FreeVec(log);
    if (rc == 1)
        return;
    shader_record_path(path, ogles2_cache_dir(), s);
    rc = read_record(path, ogles2_compiler_id(), &log);
    if (log)
        FreeVec(log);
    if (rc == 1)
        return;
    write_record(path, s->info_log); /* also over a refusal: it linked after all */
}

/* Appends a program without a binary to T:ogles2_missing_shaders.txt, for
 * the offline compiler (plan 7.8). */
static void note_missing(const struct OGLES2Program *p, const char *hex)
{
    BPTR fh = Open((STRPTR) "T:ogles2_missing_shaders.txt", MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    FPuts(fh, (STRPTR) "--- program ");
    FPuts(fh, (STRPTR)hex);
    if (p->cs) {
        FPuts(fh, (STRPTR) "\n--- compute shader\n");
        FPuts(fh, (STRPTR)p->cs->source);
    } else {
        FPuts(fh, (STRPTR) "\n--- vertex shader\n");
        FPuts(fh, (STRPTR)p->vs->source);
        FPuts(fh, (STRPTR) "\n--- fragment shader\n");
        FPuts(fh, (STRPTR)p->fs->source);
    }
    FPuts(fh, (STRPTR) "\n--- end\n");
    Close(fh);
}

#define FILE_HASH_DIGITS 24 /* + ".v3dp": 29 characters, FFS's limit is 30 */

/* A program of GLSL ES 3.x shaders, whose file must be in format 2 (an
 * older compiler, or 1.3, made 1.0 files of such programs without their
 * ES3 parts: docs/GLES3_PLAN.md §3). */
static BOOL es3_program(const struct OGLES2Program *p)
{
    return p->cs || glsl_version(p->vs->source) >= 300 || glsl_version(p->fs->source) >= 300;
}

/* Reads and checks one .v3dp file: the image, or 0 (with *refused set and
 * the program's log when the file exists but is not usable). */
static struct OGLES2Image *try_file(struct OGLES2Program *p, const UBYTE *hash, const char *path,
                                    BOOL *refused)
{
    char why[120];
    struct OGLES2Image *im;
    UBYTE *file;
    ULONG len = 0;
    *refused = FALSE;
    if (!(file = read_file(path, &len)))
        return 0;
    im = ogles2_v3dp_load(file, len, why, sizeof(why));
    if (im) { /* kept: glGetProgramBinaryOES hands it out */
        im->binary = file;
        im->binary_len = len;
    } else {
        FreeVec(file);
    }
    if (im && memcmp(im->hash, hash, 32)) {
        /* The same first 24 digits, another program: not ours. */
        ogles2_log("glLinkProgram: %s is another program (hash differs)", (ULONG)path);
        ogles2_image_free(im, 0);
        return 0;
    }
    if (im && !im->format2 && es3_program(p)) {
        ogles2_image_free(im, 0);
        im = 0;
        strcpy(why, "a GLSL ES 3.x program in a format 1.0 file (made by an older compiler)");
    }
    if (im && im->compute != (p->cs != 0)) { /* the same hash, the other kind: cannot be, but checked */
        ogles2_image_free(im, 0);
        im = 0;
        strcpy(why, p->cs ? "not a compute program" : "a compute program");
    }
    if (im) {
        ogles2_log("glLinkProgram: %s (%ld variants, %ld bytes of code)", (ULONG)path,
                   (LONG)im->nvariants, im->code_total);
        return im;
    }
    ogles2_log("glLinkProgram: %s refused: %s", (ULONG)path, (ULONG)why);
    set_log(p, "the program binary was refused: ", why);
    *refused = TRUE;
    return 0;
}

static void file_path(char *path, const char *dir, const char *hex)
{
    strcpy(path, dir);
    strncat(path, hex, FILE_HASH_DIGITS);
    strcat(path, ".v3dp");
}

/* Plan 7.8: the program's drawer (its shaders/ drawer, then the drawer
 * itself), the disk cache (OGLES2Cache:, or T: when it is not assigned:
 * that is where compiled programs go then), the on-target compiler, else the
 * sources go to T:ogles2_missing_shaders.txt. A file the library cannot use
 * does not end the search (docs/GLES3_PLAN.md §3): the next place is tried,
 * and the compiler replaces the cache's. */
static struct OGLES2Image *load_image(struct OGLES2Program *p, const UBYTE *hash, const char *hex,
                                      const char *bindings)
{
    char path[96];
    struct OGLES2Image *im;
    BOOL refused, any_refused = FALSE;
    const char *dirs[3];
    int d;
    dirs[0] = "PROGDIR:shaders/";
    dirs[1] = "PROGDIR:";
    dirs[2] = ogles2_cache_dir();
    for (d = 0; d < 3; d++) {
        file_path(path, dirs[d], hex);
        if ((im = try_file(p, hash, path, &refused)))
            return im;
        any_refused |= refused;
    }
    if (ogles2_compiler_installed()) {
        char *log = AllocVec(4096, MEMF_ANY | MEMF_CLEAR);
        BOOL ok;
        if (!log) {
            set_log(p, "out of memory", 0);
            return 0;
        }
        file_path(path, dirs[2], hex);
        ok = p->cs        ? ogles2_compile_compute(p->cs->source, path, log, 4096)
           : p->separable ? ogles2_compile_separable(p->vs ? p->vs->source : 0, p->fs ? p->fs->source : 0,
                                                     bindings, path, log, 4096)
           : bindings     ? ogles2_compile_program_bound(p->vs->source, p->fs->source, bindings, path, log, 4096)
                          : ogles2_compile_program(p->vs->source, p->fs->source, path, log, 4096);
        if (ok && (im = try_file(p, hash, path, &refused))) {
            FreeVec(log);
            return im;
        }
        if (!ok) {
            ogles2_log("glLinkProgram: the compiler failed: %s", (ULONG)log);
            set_log(p, log[0] ? log : "the shader compiler could not be run", 0);
        }
        FreeVec(log);
        return 0;
    }
    if (any_refused)
        return 0; /* try_file said why, in the log and the program's log */
    ogles2_log("glLinkProgram: no binary for program %s (sources in T:ogles2_missing_shaders.txt)",
               (ULONG)hex);
    set_log(p, "no program binary for this program: ", hex);
    note_missing(p, hex);
    return 0;
}

/* A .v3dw file of the program `hash` (its 32-bit-sampler variants), or 0. */
static struct OGLES2Image *try_wide(const UBYTE *hash, const char *path)
{
    char why[120];
    struct OGLES2Image *w;
    UBYTE *file;
    ULONG len = 0;
    if (!(file = read_file(path, &len)))
        return 0;
    w = ogles2_v3dp_load(file, len, why, sizeof(why));
    FreeVec(file);
    if (w && memcmp(w->hash, hash, 32)) {
        ogles2_image_free(w, 0);
        return 0;
    }
    if (!w)
        ogles2_log("draw: %s refused: %s", (ULONG)path, (ULONG)why);
    return w;
}

/* The program's variants for samplers that return 32 bits (a float or
 * depth texture bound, draw.c): its .v3dw beside the .v3dp (PROGDIR:, the
 * disk cache), or compiled now by the on-target compiler into the cache.
 * Once per image: at the first draw that needs them, which waits for the
 * compile (about a second on the A1200). NULL if there are none: such draws
 * then use the 16-bit variants, which read those textures wrongly (logged). */
/* A second file of a program's variants (.v3dw, .v3da): where a .v3dp may be
 * (PROGDIR:shaders/, PROGDIR:, the disk cache), or compiled now by the on-target compiler into
 * the cache. Once per image: at the first draw that needs them, which waits
 * for the compile (about a second on the A1200). */
static struct OGLES2Image *extra_image(struct OGLES2Image *im, struct OGLES2Image **slot, BOOL *tried,
                                       const char *ext, const char *what,
                                       BOOL (*compile)(const char *, const char *, const char *, char *, ULONG))
{
    static const char digits[] = "0123456789abcdef";
    char hex[FILE_HASH_DIGITS + 1], path[96];
    const char *dirs[3];
    int d, k;
    if (*slot || *tried)
        return *slot;
    *tried = TRUE;
    for (k = 0; k < FILE_HASH_DIGITS / 2; k++) {
        hex[2 * k] = digits[im->hash[k] >> 4];
        hex[2 * k + 1] = digits[im->hash[k] & 15];
    }
    hex[FILE_HASH_DIGITS] = 0;
    dirs[0] = "PROGDIR:shaders/";
    dirs[1] = "PROGDIR:";
    dirs[2] = ogles2_cache_dir();
    for (d = 0; d < 3 && !*slot; d++) {
        strcpy(path, dirs[d]);
        strcat(path, hex);
        strcat(path, ext);
        *slot = try_wide(im->hash, path);
    }
    if (!compile) /* the files only (ogles2_image_wide_file): quiet either way */
        return *slot;
    if (!*slot && im->vs_src && im->fs_src && ogles2_compiler_installed()) {
        char *log = AllocVec(1024, MEMF_ANY | MEMF_CLEAR);
        if (log && compile(im->vs_src, im->fs_src, path, log, 1024))
            *slot = try_wide(im->hash, path);
        else
            ogles2_log("draw: the compiler made no %s for program %s: %s", (ULONG)what, (ULONG)hex,
                       (ULONG)(log ? log : "out of memory"));
        if (log)
            FreeVec(log);
    }
    if (*slot)
        ogles2_log("draw: program %s: its %s (%ld)", (ULONG)hex, (ULONG)what, (LONG)(*slot)->nvariants);
    else
        ogles2_log("draw: program %s has no %s (no %s%s): drawn with its plain variants", (ULONG)hex,
                   (ULONG)what, (ULONG)hex, (ULONG)ext);
    return *slot;
}

/* The program's variants for samplers that return 32 bits (a float or
 * depth texture bound, draw.c): its .v3dw. NULL if there are none: such
 * draws then use the 16-bit variants, which read those textures wrongly. */
const struct OGLES2Image *ogles2_image_wide(struct OGLES2Image *im)
{
    return extra_image(im, &im->wide, &im->wide_tried, ".v3dw", "32-bit sampler variants",
                       ogles2_compile_program_wide);
}

/* Its alpha-to-coverage variants: its .v3da. NULL if there are none: such
 * draws then cover every sample, as without GL_SAMPLE_ALPHA_TO_COVERAGE. */
const struct OGLES2Image *ogles2_image_a2c(struct OGLES2Image *im)
{
    return extra_image(im, &im->a2c, &im->a2c_tried, ".v3da", "alpha-to-coverage variants",
                       ogles2_compile_program_a2c);
}

/* An OpenGL ES 3 program's: shipped or cached files are read (their keys
 * serve where they match), nothing is compiled; the draw asks for the
 * variant of its key instead (draw.c, es3_variants). */
const struct OGLES2Image *ogles2_image_wide_file(struct OGLES2Image *im)
{
    return extra_image(im, &im->wide, &im->wide_tried, ".v3dw", "32-bit sampler variants", 0);
}

const struct OGLES2Image *ogles2_image_a2c_file(struct OGLES2Image *im)
{
    return extra_image(im, &im->a2c, &im->a2c_tried, ".v3da", "alpha-to-coverage variants", 0);
}

/* ---- draw-time variants (docs/GLES3_PLAN.md stage A item 5) ----
 * A program's .v3dx: the variants compiled for draw states its own file
 * lacks. Taken only when it is the program's (its hash), of the same
 * layout (its VARIANT_OF is the program file's layout CRC: a variant's
 * uniform recipe indexes the constant buffers as that link laid them out)
 * and its variants read only uniform blocks the program's stages have. */
static void image_hex(const struct OGLES2Image *im, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    int k;
    for (k = 0; k < FILE_HASH_DIGITS / 2; k++) {
        hex[2 * k] = digits[im->hash[k] >> 4];
        hex[2 * k + 1] = digits[im->hash[k] & 15];
    }
    hex[FILE_HASH_DIGITS] = 0;
}

static struct OGLES2Image *try_x(const struct OGLES2Image *im, const char *path)
{
    char why[120];
    struct OGLES2Image *x;
    UBYTE *file;
    ULONG len = 0, k, j;
    if (!(file = read_file(path, &len)))
        return 0;
    x = ogles2_v3dp_load(file, len, why, sizeof(why));
    FreeVec(file);
    if (!x) {
        ogles2_log("draw: %s refused: %s", (ULONG)path, (ULONG)why);
        return 0;
    }
    if (!x->v3dx || memcmp(x->hash, im->hash, 32) || x->variant_of != im->layout_crc) {
        ogles2_log("draw: %s is not this program's, or of another link of it (another compiler)", (ULONG)path);
        ogles2_image_free(x, 0);
        return 0;
    }
    for (k = 0; k < x->nvariants; k++) {
        const struct OGLES2Variant *v = &x->variants[k];
        ULONG si = v->stage == OGLES2_VARIANT_FS;
        for (j = 0; j < v->nrecipe; j++)
            if ((v->rtype[j] == OGLES2_U_UBO_ADDR && (v->rdata[j] >> 24) > im->stage_nblocks[si]) ||
                (v->rtype[j] == OGLES2_U_GET_UBO_SIZE && v->rdata[j] >= im->stage_nblocks[si])) {
                ogles2_log("draw: %s reads a uniform block the program does not have", (ULONG)path);
                ogles2_image_free(x, 0);
                return 0;
            }
    }
    ogles2_log("draw: %s (%ld variants)", (ULONG)path, (LONG)x->nvariants);
    return x;
}

const struct OGLES2Image *ogles2_image_x(struct OGLES2Image *im, BOOL cache)
{
    char hex[FILE_HASH_DIGITS + 1], path[96];
    image_hex(im, hex);
    if (cache) {
        if (!im->xc_tried) {
            im->xc_tried = TRUE;
            strcpy(path, ogles2_cache_dir());
            strcat(path, hex);
            strcat(path, ".v3dx");
            im->xc = try_x(im, path);
        }
        return im->xc;
    }
    if (!im->xs_tried) {
        static const char *const dirs[2] = {"PROGDIR:shaders/", "PROGDIR:"};
        int d;
        im->xs_tried = TRUE;
        for (d = 0; d < 2 && !im->xs; d++) {
            strcpy(path, dirs[d]);
            strcat(path, hex);
            strcat(path, ".v3dx");
            im->xs = try_x(im, path);
        }
    }
    return im->xs;
}

BOOL ogles2_image_compile_variants(struct OGLES2Context *ctx, struct OGLES2Image *im, const UBYTE *keys,
                                   ULONG keys_len)
{
    char hex[FILE_HASH_DIGITS + 1], path[96], *log;
    int rc;
    if (im->x_failed || !im->vs_src || !im->fs_src || !ogles2_compiler_installed())
        return FALSE;
    if (!(log = AllocVec(1024, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    image_hex(im, hex);
    file_path(path, ogles2_cache_dir(), hex);
    rc = ogles2_compile_variants(im->vs_src, im->fs_src, im->bind_src, keys, keys_len, im->layout_crc, path,
                                 log, 1024);
    if (rc < 0) {
        im->x_failed = TRUE; /* no compiler for these: asked no more */
        ogles2_log("draw: program %s: the compiler cannot compile draw-time variants (none, or too old)",
                   (ULONG)hex);
    } else if (!rc) {
        ogles2_log("draw: program %s: the compiler made no variant: %s", (ULONG)hex, (ULONG)log);
    }
    FreeVec(log);
    if (rc <= 0)
        return FALSE;
    /* The cache's .v3dx again: the compiler added to it. The old image
     * stays (programs drawing with this image may hold its variants, and
     * the open job its code), chained in the new one's own xc slot, which
     * a .v3dx image does not use otherwise; all go with the program's. */
    {
        struct OGLES2Image *old = im->xc;
        im->xc = 0;
        im->xc_tried = FALSE;
        if (!ogles2_image_x(im, TRUE)) {
            im->xc = old;
            return FALSE;
        }
        im->xc->xc = old;
    }
    (void)ctx;
    return TRUE;
}

/* After an image is set: attribute locations, uniform locations, constant
 * buffers. FALSE (with the log set) if the attributes do not fit. */
static BOOL bind_image(struct OGLES2Program *p, const struct OGLES2Image *im)
{
    ULONG used = 0;
    int a, i, k, n;
    UWORD u, nloc = 0;

    p->var_ok = FALSE;     /* draw.c's per-program caches start empty */
    p->us_var[0] = p->us_var[1] = 0;
    p->nsamplers = 0xff;
    p->nslots = 0xff;

    /* Attribute locations: a location of its own (layout(location), format
     * 2: glBindAttribLocation cannot move it), then glBindAttribLocation's,
     * then the lowest free run (a matrix takes one location per column). */
    for (a = 0; a < im->nattribs; a++) {
        p->attrib_index[a] = 0xff;
        if (im->attribs[a].location != 0xff) {
            p->attrib_index[a] = im->attribs[a].location;
            for (k = 0; k < im->attribs[a].columns; k++)
                used |= 1UL << (im->attribs[a].location + k);
            continue;
        }
        for (i = 0; i < p->nbindings; i++)
            if (str_eq(p->bind_name[i], im->attribs[a].name)) {
                p->attrib_index[a] = (UBYTE)p->bind_index[i];
                for (k = 0; k < im->attribs[a].columns; k++)
                    used |= 1UL << (p->bind_index[i] + k);
            }
    }
    for (a = 0; a < im->nattribs; a++) {
        if (p->attrib_index[a] != 0xff)
            continue;
        n = im->attribs[a].columns;
        for (i = 0; i + n <= OGLES2_MAX_VERTEX_ATTRIBS; i++) {
            ULONG run = ((1UL << n) - 1) << i;
            if (!(used & run))
                break;
        }
        if (i + n > OGLES2_MAX_VERTEX_ATTRIBS) {
            set_log(p, "too many vertex attributes", 0);
            return FALSE;
        }
        p->attrib_index[a] = (UBYTE)i;
        used |= ((1UL << n) - 1) << i;
    }

    /* Uniform locations: Mesa's (format 2's UNIFORM_LOCATIONS: explicit
     * ones among them, gaps and inactive ones), else one per array element
     * of every visible uniform. */
    if (im->remap)
        nloc = im->nremap;
    else for (u = 0; u < im->nuniforms; u++)
        if (!(im->uniforms[u].flags & (OGLES2_UNIFORM_BUILTIN | OGLES2_UNIFORM_HIDDEN | OGLES2_UNIFORM_BUFFER |
                                       OGLES2_UNIFORM_ATOMIC)))
            nloc += im->uniforms[u].array ? im->uniforms[u].array : 1;
    p->loc_uniform = AllocVec(nloc * 4 + 4, MEMF_ANY);
    /* A sampler array's elements are samplers of their own (consecutive
     * texture indices, as Mesa lowers them): a unit per location. */
    p->sampler_unit = AllocVec(nloc + 1, MEMF_ANY | MEMF_CLEAR);
    p->uniform_loc = AllocVec((im->nuniforms + 1) * sizeof(UWORD), MEMF_ANY);
    p->block_binding = AllocVec((im->nublocks + 1) * sizeof(ULONG), MEMF_ANY);
    for (k = 0; k < 2; k++) {
        p->cbuf[k] = AllocVec(im->cbuf_words[k] * 4 + 4, MEMF_ANY | MEMF_CLEAR);
        p->cbuf_le[k] = AllocVec(im->cbuf_words[k] * 4 + 4, MEMF_ANY | MEMF_CLEAR);
    }
    if (!p->loc_uniform || !p->sampler_unit || !p->uniform_loc || !p->block_binding || !p->cbuf[0] || !p->cbuf[1] ||
        !p->cbuf_le[0] || !p->cbuf_le[1]) {
        set_log(p, "out of memory", 0);
        return FALSE;
    }
    p->loc_element = p->loc_uniform + nloc;
    if (im->remap) {
        UWORD l, e;
        for (u = 0; u < im->nuniforms; u++)
            p->uniform_loc[u] = 0xFFFF;
        for (l = 0; l < nloc; l++) {
            p->loc_uniform[l] = u = im->remap[l];
            p->loc_element[l] = 0;
            if (u >= OGLES2_LOC_INACTIVE)
                continue;
            if (p->uniform_loc[u] == 0xFFFF)
                p->uniform_loc[u] = l; /* its first element's */
            e = (UWORD)(l - p->uniform_loc[u]);
            p->loc_element[l] = e;
        }
        nloc = im->nremap;
    } else for (u = 0, nloc = 0; u < im->nuniforms; u++) {
        const struct OGLES2ImageUniform *un = &im->uniforms[u];
        UWORD e, ne = un->array ? un->array : 1;
        p->uniform_loc[u] = 0xFFFF;
        if (un->flags & (OGLES2_UNIFORM_BUILTIN | OGLES2_UNIFORM_HIDDEN | OGLES2_UNIFORM_BUFFER | OGLES2_UNIFORM_ATOMIC))
            continue;
        p->uniform_loc[u] = (UWORD)nloc;
        for (e = 0; e < ne; e++) {
            p->loc_uniform[nloc] = u;
            p->loc_element[nloc] = e;
            nloc++;
        }
    }
    p->nlocations = nloc;
    /* Samplers' and images' first units (layout(binding), ES 3.1). */
    {
        const UWORD *o = im->opaque_units;
        UWORD k2, e;
        for (k2 = 0; o && k2 < im->nopaque; k2++) {
            UWORD un = o[0], ne = o[1];
            for (e = 0; e < ne; e++)
                if (un < im->nuniforms && p->uniform_loc[un] != 0xFFFF && p->uniform_loc[un] + e < nloc)
                    p->sampler_unit[p->uniform_loc[un] + e] = (UBYTE)o[2 + e];
            o += 2 + ne;
        }
    }
    for (u = 0; u < im->nublocks; u++)
        p->block_binding[u] = im->ublocks[u].binding;
    for (k = 0; k < 2; k++)
        if (im->cbuf_template[k]) {
            UWORD w;
            for (w = 0; w < im->cbuf_words[k]; w++)
                ogles2_cbuf_set(p, k, w, im->cbuf_template[k][w]);
        }
    p->image = im;
    return TRUE;
}

static void describe_variants(struct OGLES2Program *p, const struct OGLES2Image *im, const char *hex);

static void link_program(struct OGLES2Context *ctx, struct OGLES2Program *p, const char *state);

void ogles2_glLinkProgram(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (ogles2_xfb_program_busy(ctx, p)) { /* ES 3.0 2.15.2: an active transform feedback's */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    link_program(ctx, p, 0);
}

/* The link: p's own link state (link_state), or `state` given (a pipeline's
 * pair program: its vertex program's, as that was linked). */
static void link_program(struct OGLES2Context *ctx, struct OGLES2Program *p, const char *state)
{
    struct OGLES2Image *im;
    UBYTE hash[32];
    char hex[65], *bindings;
    /* A program in use keeps its old image in the open job until relinked:
     * the job's records already point at its code (ogles2_image_free defers). */
    unlink_program(ctx, p);
    p->binary_hint = p->binary_hint_pending;
    if (p->cs) { /* ES 3.1: a compute shader links alone (7.3: else a link error) */
        if (p->vs || p->fs) {
            set_log(p, "a compute shader cannot be linked with a vertex or fragment shader", 0);
            return;
        }
        if (!p->cs->compiled) {
            set_log(p, "the compute shader did not compile:\n", p->cs->info_log);
            return;
        }
    } else if (p->separable && (p->vs || p->fs)) { /* ES 3.1 7.3: one stage, or both */
        if ((p->vs && !p->vs->compiled) || (p->fs && !p->fs->compiled)) {
            set_log(p, p->vs && !p->vs->compiled ? "the vertex shader did not compile:\n"
                                                 : "the fragment shader did not compile:\n",
                    p->vs && !p->vs->compiled ? p->vs->info_log : p->fs->info_log);
            return;
        }
    } else if (!p->vs || !p->fs || !p->vs->compiled || !p->fs->compiled) {
        /* Say which shader failed, with its compiler messages. */
        if (p->vs && p->fs && !p->vs->compiled)
            set_log(p, "the vertex shader did not compile:\n", p->vs->info_log);
        else if (p->vs && p->fs && !p->fs->compiled)
            set_log(p, "the fragment shader did not compile:\n", p->fs->info_log);
        else
            set_log(p, "a compiled vertex shader and fragment shader must be attached", 0);
        return;
    }
    bindings = p->cs ? 0 : state ? dup_text(state, str_len(state)) : link_state(p); /* compute: none */
    program_hash(p, bindings, hash, hex);
    for (im = ctx->share->images; im && memcmp(im->hash, hash, 32); im = im->next)
        ;
    if (!im) {
        im = load_image(p, hash, hex, bindings);
        /* Kept for the draw-time variants' compiles (the same link). */
        if (im && bindings)
            im->bind_src = dup_text(bindings, str_len(bindings));
        if (bindings)
            FreeVec(bindings);
        bindings = 0;
        if (!im)
            return;
        im->next = ctx->share->images;
        ctx->share->images = im;
        /* Kept for its 32-bit-sampler variants, compiled when a draw first
         * needs them (ogles2_image_wide; a compute program draws not), and a
         * separable program's for a pipeline's pair program (ES 3.1). */
        if (!p->cs && p->vs)
            im->vs_src = dup_text(p->vs->source, str_len(p->vs->source));
        if (!p->cs && p->fs)
            im->fs_src = dup_text(p->fs->source, str_len(p->fs->source));
        /* A binary proves its shaders compile: record them, so the next
         * glCompileShader of one needs no compiler. */
        if (ogles2_compiler_installed()) {
            if (p->cs)
                mark_compiled(p->cs);
            if (!p->cs && p->vs)
                mark_compiled(p->vs);
            if (!p->cs && p->fs)
                mark_compiled(p->fs);
        }
    }
    if (bindings)
        FreeVec(bindings);
    if (!bind_image(p, im)) {
        unlink_program(ctx, p);
        return;
    }
    p->linked = GL_TRUE;
    if (ctx->debug_shader_log)
        describe_variants(p, im, hex);
    else
        set_log(p, "", 0);
}

/* OGLES2_CCT_DEBUG_SHADER_LOG (plan 8.4): the link log describes what the
 * program runs, one line per compiled variant, as Mesa's shader-db
 * statistics do; also in the library's log. */
static void describe_variants(struct OGLES2Program *p, const struct OGLES2Image *im, const char *hex)
{
    ULONG size = 80 + 96UL * im->nvariants, n;
    char *t = AllocVec(size, MEMF_ANY);
    UWORD i;
    if (!t) {
        set_log(p, "", 0);
        return;
    }
    n = ogles2_format(t, size, "program %.24s: %ld variants\n", (ULONG)hex, (LONG)im->nvariants);
    for (i = 0; i < im->nvariants; i++) {
        const struct OGLES2Variant *v = &im->variants[i];
        const char *what = v->stage == OGLES2_VARIANT_COMPUTE ? "compute"
                           : v->stage == OGLES2_VARIANT_FS
                               ? (v->is_points ? "fragment, points" : v->is_lines ? "fragment, lines"
                                                                                  : "fragment, triangles")
                               : v->stage == OGLES2_VARIANT_CS
                                     ? (v->per_vertex_point_size ? "binning, points" : "binning")
                                     : (v->per_vertex_point_size ? "vertex, points" : "vertex");
        n += ogles2_format(t + n, size - n, "  %s: %ld instructions, %ld threads, %ld spill bytes, %ld uniforms\n",
                           (ULONG)what, (LONG)(v->code_bytes / 8), (LONG)v->threads, (LONG)v->spill_size,
                           (LONG)v->nrecipe);
    }
    set_log(p, t, 0);
    ogles2_log("glLinkProgram: %s", (ULONG)t);
    FreeVec(t);
}

/* A program on an image the library carries (draw.c's clears): the
 * built-in one, or one loaded from the library's own data. */
BOOL ogles2_program_image(struct OGLES2Program *p, const struct OGLES2Image *im)
{
    memset(p, 0, sizeof(*p));
    p->kind = OGLES2_OBJ_PROGRAM;
    if (!bind_image(p, im)) {
        ogles2_program_builtin_free(p);
        return FALSE;
    }
    p->linked = GL_TRUE;
    return TRUE;
}

BOOL ogles2_program_builtin(struct OGLES2Program *p)
{
    return ogles2_program_image(p, &builtin_smooth);
}

void ogles2_program_builtin_free(struct OGLES2Program *p)
{
    unlink_program(0, p);
    if (p->info_log)
        FreeVec(p->info_log);
    p->info_log = 0;
}

void ogles2_glUseProgram(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p = 0, *old;
    if (!ogles2_context_valid(ctx))
        return;
    if (program) {
        if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
            return;
        if (!p->linked) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    if (ogles2_xfb_drawing(ctx)) { /* ES 3.0 2.15.2: not while transform feedback is active and not paused */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    old = ctx->program;
    ctx->program = p;
    if (old && old != p && old->delete_pending && !in_use(ctx, old))
        free_program(ctx, old);
}

void ogles2_glValidateProgram(struct OGLES2Context *ctx, GLuint program)
{
    if (!ogles2_context_valid(ctx))
        return;
    lookup(ctx, program, OGLES2_OBJ_PROGRAM); /* the status is LINK_STATUS's (glGetProgramiv) */
}

/* The name glGetActiveUniform reports: arrays get "[0]" (GLES 2.0 2.10.4). */
static ULONG active_name_len(const struct OGLES2ImageUniform *u)
{
    return str_len(u->name) + (u->array ? 3 : 0);
}

static BOOL visible(const struct OGLES2ImageUniform *u)
{
    return !(u->flags & (OGLES2_UNIFORM_BUILTIN | OGLES2_UNIFORM_HIDDEN | OGLES2_UNIFORM_BUFFER));
}

/* The index'th visible uniform, or NULL. */
static const struct OGLES2ImageUniform *active_uniform(const struct OGLES2Image *im, GLuint index)
{
    UWORD u;
    for (u = 0; u < im->nuniforms; u++)
        if (visible(&im->uniforms[u]) && index-- == 0)
            return &im->uniforms[u];
    return 0;
}

static const struct OGLES2ImageAttrib *active_attrib(const struct OGLES2Image *im, GLuint index)
{
    UWORD a;
    for (a = 0; a < im->nattribs; a++)
        if (im->attribs[a].slot != 0xff && index-- == 0)
            return &im->attribs[a];
    return 0;
}

void ogles2_glGetProgramiv(struct OGLES2Context *ctx, GLuint program, GLenum pname, GLint *params)
{
    struct OGLES2Program *p;
    const struct OGLES2Image *im;
    GLint n, i;
    if (!ogles2_context_valid(ctx) || !params)
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    im = p->image;
    switch (pname) {
    case GL_DELETE_STATUS:     *params = p->delete_pending ? GL_TRUE : GL_FALSE; break;
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:   *params = p->linked; break;
    case GL_INFO_LOG_LENGTH:
        *params = (p->info_log && p->info_log[0]) ? (GLint)str_len(p->info_log) + 1 : 0;
        break;
    case GL_ATTACHED_SHADERS:  *params = (p->vs ? 1 : 0) + (p->fs ? 1 : 0) + (p->cs ? 1 : 0); break;
    case GL_ACTIVE_ATTRIBUTES:
        for (n = 0; im && active_attrib(im, (GLuint)n); n++)
            ;
        *params = n;
        break;
    case GL_ACTIVE_UNIFORMS:
        for (n = 0; im && active_uniform(im, (GLuint)n); n++)
            ;
        *params = n;
        break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
        n = 0;
        for (i = 0; im && active_attrib(im, (GLuint)i); i++)
            if ((GLint)str_len(active_attrib(im, (GLuint)i)->name) + 1 > n)
                n = (GLint)str_len(active_attrib(im, (GLuint)i)->name) + 1;
        *params = n;
        break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH:
        n = 0;
        for (i = 0; im && active_uniform(im, (GLuint)i); i++)
            if ((GLint)active_name_len(active_uniform(im, (GLuint)i)) + 1 > n)
                n = (GLint)active_name_len(active_uniform(im, (GLuint)i)) + 1;
        *params = n;
        break;
    case GL_PROGRAM_BINARY_LENGTH_OES:
        *params = p->linked && im ? (GLint)im->binary_len : 0;
        break;
    case GL_PROGRAM_BINARY_RETRIEVABLE_HINT: /* ES 3.0 */
        if (!ogles2_es3(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        *params = p->binary_hint;
        break;
    case GL_PROGRAM_SEPARABLE: /* ES 3.1 */
        if (!ogles2_es31(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        *params = p->separable;
        break;
    case GL_COMPUTE_WORK_GROUP_SIZE: /* ES 3.1: a linked compute program's local size */
        if (!ogles2_es31(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        if (!p->linked || !im || !im->compute) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            break;
        }
        {
            UWORD k;
            for (k = 0; k < im->nvariants; k++)
                if (im->variants[k].stage == OGLES2_VARIANT_COMPUTE) {
                    params[0] = im->variants[k].local_size[0];
                    params[1] = im->variants[k].local_size[1];
                    params[2] = im->variants[k].local_size[2];
                    break;
                }
        }
        break;
    case GL_ACTIVE_ATOMIC_COUNTER_BUFFERS: /* ES 3.1 */
        if (!ogles2_es31(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        *params = im && p->linked ? im->nabuffers : 0;
        break;
    case GL_ACTIVE_UNIFORM_BLOCKS: /* ES 3.0 */
    case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH:
        if (!ogles2_es3(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        n = 0;
        for (i = 0; im && i < (GLint)im->nublocks; i++)
            if (pname == GL_ACTIVE_UNIFORM_BLOCKS)
                n++;
            else if ((GLint)str_len(im->ublocks[i].name) + 1 > n)
                n = (GLint)str_len(im->ublocks[i].name) + 1;
        *params = n;
        break;
    /* ES 3.0's transform feedback: as given (Mesa answers so), not as linked */
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
    case GL_TRANSFORM_FEEDBACK_VARYINGS:
    case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH:
        if (!ogles2_es3(ctx)) {
            ogles2_error(ctx, GL_INVALID_ENUM);
            break;
        }
        if (pname == GL_TRANSFORM_FEEDBACK_BUFFER_MODE) {
            *params = p->xfb_mode ? (GLint)p->xfb_mode : GL_INTERLEAVED_ATTRIBS;
        } else if (pname == GL_TRANSFORM_FEEDBACK_VARYINGS) {
            *params = p->nxfb;
        } else {
            const char *s = p->xfb_names;
            n = 0;
            while (s && *s) {
                for (i = 0; s[i] != '\n'; i++)
                    ;
                if (i + 1 > n)
                    n = i + 1;
                s += i + 1;
            }
            *params = n;
        }
        break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

/* ---- ES 3.0's transform feedback varyings (docs/GLES3_PLAN.md stage C item 1) ---- */

void ogles2_glTransformFeedbackVaryings(struct OGLES2Context *ctx, GLuint program, GLsizei count,
                                        const GLchar *const *varyings, GLenum bufferMode)
{
    struct OGLES2Program *p;
    ULONG size = 1, at = 0;
    GLsizei k;
    char *t = 0;
    if (!ogles2_es3_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (bufferMode != GL_INTERLEAVED_ATTRIBS && bufferMode != GL_SEPARATE_ATTRIBS) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (count < 0 || (bufferMode == GL_SEPARATE_ATTRIBS && count > OGLES2_MAX_XFB_BUFFERS)) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ogles2_xfb_program_busy(ctx, p)) { /* an active transform feedback's (ES 3.0 2.15.2) */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    for (k = 0; k < count; k++) {
        if (!varyings || !varyings[k]) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
        size += str_len(varyings[k]) + 1;
    }
    if (count && !(t = AllocVec(size, MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (k = 0; k < count; k++) {
        ULONG n = str_len(varyings[k]);
        memcpy(t + at, varyings[k], n);
        at += n;
        t[at++] = '\n';
    }
    if (t)
        t[at] = 0;
    if (p->xfb_names)
        FreeVec(p->xfb_names);
    p->xfb_names = t;
    p->nxfb = (UWORD)count;
    p->xfb_mode = bufferMode;
}

void ogles2_glGetTransformFeedbackVarying(struct OGLES2Context *ctx, GLuint program, GLuint index, GLsizei bufSize,
                                          GLsizei *length, GLsizei *size, GLenum *type, GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageXfbVar *v;
    ULONG n;
    if (!ogles2_es3_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (!p->linked || !p->image || !p->image->xfb || index >= p->image->xfb->nvaryings) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    v = &p->image->xfb->varyings[index];
    if (size)
        *size = v->size;
    if (type)
        *type = v->type;
    n = str_len(v->name);
    if (bufSize > 0 && name) {
        if (n > (ULONG)bufSize - 1)
            n = (ULONG)bufSize - 1;
        memcpy(name, v->name, n);
        name[n] = 0;
    } else {
        n = 0;
    }
    if (length)
        *length = (GLsizei)n;
}

/* ---- ES 3.0's uniform blocks (docs/GLES3_PLAN.md §5 item 6) ---- */

/* A linked program's image for an ES3 entry, or NULL (the error set:
 * GL_INVALID_VALUE or _OPERATION for the name, as lookup's). */
static const struct OGLES2Image *es3_image(struct OGLES2Context *ctx, GLuint program, struct OGLES2Program **pp)
{
    struct OGLES2Program *p;
    if (!ogles2_es3_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return 0;
    if (pp)
        *pp = p;
    return p->image;
}

/* Image uniform u's index among the active ones (glGetActiveUniform's). */
static GLint active_index(const struct OGLES2Image *im, UWORD u)
{
    UWORD k;
    GLint n = 0;
    for (k = 0; k < u; k++)
        if (visible(&im->uniforms[k]))
            n++;
    return n;
}

GLuint ogles2_glGetUniformBlockIndex(struct OGLES2Context *ctx, GLuint program, const GLchar *uniformBlockName)
{
    const struct OGLES2Image *im = es3_image(ctx, program, 0);
    UWORD k;
    for (k = 0; im && uniformBlockName && k < im->nublocks; k++)
        if (str_eq(im->ublocks[k].name, uniformBlockName))
            return k;
    return GL_INVALID_INDEX;
}

void ogles2_glGetActiveUniformBlockName(struct OGLES2Context *ctx, GLuint program, GLuint uniformBlockIndex,
                                        GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName)
{
    struct OGLES2Program *p = 0;
    const struct OGLES2Image *im = es3_image(ctx, program, &p);
    GLsizei n;
    if (!p)
        return;
    if (!im || uniformBlockIndex >= im->nublocks || bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    n = copy_out(uniformBlockName, bufSize, im->ublocks[uniformBlockIndex].name);
    if (length)
        *length = n;
}

void ogles2_glGetActiveUniformBlockiv(struct OGLES2Context *ctx, GLuint program, GLuint uniformBlockIndex,
                                      GLenum pname, GLint *params)
{
    struct OGLES2Program *p = 0;
    const struct OGLES2Image *im = es3_image(ctx, program, &p);
    const struct OGLES2ImageBlock *bl;
    UWORD k;
    if (!p)
        return;
    if (!im || uniformBlockIndex >= im->nublocks) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    bl = &im->ublocks[uniformBlockIndex];
    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING:       *params = (GLint)p->block_binding[uniformBlockIndex]; return;
    case GL_UNIFORM_BLOCK_DATA_SIZE:     *params = (GLint)bl->size; return;
    case GL_UNIFORM_BLOCK_NAME_LENGTH:   *params = (GLint)str_len(bl->name) + 1; return;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: *params = bl->nmembers; return;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES:
        for (k = 0; k < bl->nmembers; k++)
            params[k] = active_index(im, bl->members[k]);
        return;
    /* Mesa's stage bits: 0 the vertex shader, 4 the fragment shader */
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:   *params = (bl->stages & 0x01) ? GL_TRUE : GL_FALSE; return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: *params = (bl->stages & 0x10) ? GL_TRUE : GL_FALSE; return;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
    }
}

void ogles2_glUniformBlockBinding(struct OGLES2Context *ctx, GLuint program, GLuint uniformBlockIndex,
                                  GLuint uniformBlockBinding)
{
    struct OGLES2Program *p = 0;
    const struct OGLES2Image *im = es3_image(ctx, program, &p);
    if (!p)
        return;
    /* 108: GL_MAX_UNIFORM_BUFFER_BINDINGS (state.c) */
    if (!im || uniformBlockIndex >= im->nublocks || uniformBlockBinding >= 108) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    p->block_binding[uniformBlockIndex] = uniformBlockBinding;
}

/* An active uniform's name as glGetUniformIndices takes it: as
 * glGetActiveUniform reports it, or an array's without its "[0]". */
static BOOL uniform_named(const struct OGLES2ImageUniform *u, const char *name)
{
    ULONG n = str_len(u->name);
    if (strncmp(u->name, name, n))
        return FALSE;
    return !name[n] || (u->array && str_eq(name + n, "[0]"));
}

void ogles2_glGetUniformIndices(struct OGLES2Context *ctx, GLuint program, GLsizei uniformCount,
                                const GLchar *const *uniformNames, GLuint *uniformIndices)
{
    struct OGLES2Program *p = 0;
    const struct OGLES2Image *im = es3_image(ctx, program, &p);
    GLsizei k;
    UWORD u;
    if (!p)
        return;
    if (uniformCount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; uniformIndices && uniformNames && k < uniformCount; k++) {
        uniformIndices[k] = GL_INVALID_INDEX;
        for (u = 0; im && uniformNames[k] && u < im->nuniforms; u++)
            if (visible(&im->uniforms[u]) && uniform_named(&im->uniforms[u], uniformNames[k])) {
                uniformIndices[k] = (GLuint)active_index(im, u);
                break;
            }
    }
}

void ogles2_glGetActiveUniformsiv(struct OGLES2Context *ctx, GLuint program, GLsizei uniformCount,
                                  const GLuint *uniformIndices, GLenum pname, GLint *params)
{
    struct OGLES2Program *p = 0;
    const struct OGLES2Image *im = es3_image(ctx, program, &p);
    const struct OGLES2ImageUniform *u;
    GLsizei k;
    if (!p)
        return;
    if (uniformCount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; uniformIndices && k < uniformCount; k++)
        if (!im || !active_uniform(im, uniformIndices[k])) {
            ogles2_error(ctx, GL_INVALID_VALUE);
            return;
        }
    if (pname != GL_UNIFORM_TYPE && pname != GL_UNIFORM_SIZE && pname != GL_UNIFORM_NAME_LENGTH &&
        pname != GL_UNIFORM_BLOCK_INDEX && pname != GL_UNIFORM_OFFSET && pname != GL_UNIFORM_ARRAY_STRIDE &&
        pname != GL_UNIFORM_MATRIX_STRIDE && pname != GL_UNIFORM_IS_ROW_MAJOR) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    for (k = 0; uniformIndices && params && k < uniformCount; k++) {
        BOOL in_block;
        u = active_uniform(im, uniformIndices[k]);
        in_block = u->block >= 0; /* an atomic counter's offset and stride too (ES 3.1) */
        /* the default block's: -1 offsets and strides, as Mesa's */
        params[k] = pname == GL_UNIFORM_TYPE          ? (GLint)u->type
                  : pname == GL_UNIFORM_SIZE          ? (GLint)(u->array ? u->array : 1)
                  : pname == GL_UNIFORM_NAME_LENGTH   ? (GLint)active_name_len(u) + 1
                  : pname == GL_UNIFORM_BLOCK_INDEX   ? (u->flags & OGLES2_UNIFORM_ATOMIC ? -1 : u->block)
                  : pname == GL_UNIFORM_OFFSET        ? (in_block ? u->offset : -1)
                  : pname == GL_UNIFORM_ARRAY_STRIDE  ? (in_block ? u->array_stride : -1)
                  : pname == GL_UNIFORM_MATRIX_STRIDE ? (in_block ? u->matrix_stride : -1)
                                                      : (in_block && u->row_major ? GL_TRUE : GL_FALSE);
    }
}

/* ---- OES_get_program_binary: the binary is the program's .v3dp file
 * (GL_PROGRAM_BINARY_V3DP_PISTORM), as the disk cache holds it. ---- */

void ogles2_glGetProgramBinaryOES(struct OGLES2Context *ctx, GLuint program, GLsizei bufSize,
                                  GLsizei *length, GLenum *binaryFormat, void *binary)
{
    struct OGLES2Program *p;
    const struct OGLES2Image *im;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    im = p->image;
    /* Not linked, or a buffer too small for it (the extension's errors). */
    if (!p->linked || !im || !im->binary || bufSize < (GLsizei)im->binary_len) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        if (length)
            *length = 0;
        return;
    }
    if (binary)
        memcpy(binary, im->binary, im->binary_len);
    if (length)
        *length = (GLsizei)im->binary_len;
    if (binaryFormat)
        *binaryFormat = GL_PROGRAM_BINARY_V3DP_PISTORM;
}

/* The program runs the binary's image: as a link from shaders does, with
 * the binary in place of the disk cache; a binary that is not usable (its
 * reason in the info log) leaves the program not linked, which the
 * extension asks of a binary an implementation refuses. */
void ogles2_glProgramBinaryOES(struct OGLES2Context *ctx, GLuint program, GLenum binaryFormat,
                               const void *binary, GLint length)
{
    struct OGLES2Program *p;
    struct OGLES2Image *im, *c;
    char why[120];
    UBYTE *copy;
    if (!ogles2_context_valid(ctx))
        return;
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (binaryFormat != GL_PROGRAM_BINARY_V3DP_PISTORM) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ogles2_xfb_program_busy(ctx, p)) { /* as glLinkProgram (ES 3.0 2.15.2) */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    unlink_program(ctx, p);
    if (!binary || length <= 0) {
        set_log(p, "the program binary was refused: ", "empty");
        return;
    }
    if (!(im = ogles2_v3dp_load((const UBYTE *)binary, (ULONG)length, why, sizeof(why)))) {
        ogles2_log("glProgramBinaryOES: refused: %s", (ULONG)why);
        set_log(p, "the program binary was refused: ", why);
        return;
    }
    /* An OpenGL ES 3 program's file (format 2) needs an ES3 context. */
    if (im->format2 && !ogles2_es3(ctx)) {
        ogles2_image_free(im, 0);
        ogles2_log("glProgramBinaryOES: refused: an OpenGL ES 3 program in an OpenGL ES 2.0 context");
        set_log(p, "the program binary was refused: ", "an OpenGL ES 3 program needs an OpenGL ES 3 context");
        return;
    }
    /* The program cache: an image of the same program is already loaded. */
    for (c = ctx->share->images; c && memcmp(c->hash, im->hash, 32); c = c->next)
        ;
    if (c) {
        ogles2_image_free(im, 0);
        im = c;
    } else if ((copy = AllocVec((ULONG)length, MEMF_ANY))) {
        memcpy(copy, binary, (ULONG)length);
        im->binary = copy;
        im->binary_len = (ULONG)length;
        im->next = ctx->share->images;
        ctx->share->images = im;
    } else {
        ogles2_image_free(im, 0);
        set_log(p, "out of memory", 0);
        return;
    }
    if (!bind_image(p, im)) {
        unlink_program(ctx, p);
        return;
    }
    p->linked = GL_TRUE;
    p->binary_hint = p->binary_hint_pending;
    set_log(p, "", 0);
}

/* ---- ES 3.1's program pipelines (docs/GLES3_PLAN.md stage D item 5),
 * Mesa's pipelineobj.c: the context's own objects, by name; a name from
 * glGenProgramPipelines is an object (glIsProgramPipeline) once bound or
 * used. They hold their programs as glUseProgram does (in_use). ---- */

static void pair_free(struct OGLES2Context *ctx, struct OGLES2Pipeline *pp);
static void image_hex(const struct OGLES2Image *im, char *hex);
static void file_path(char *path, const char *dir, const char *hex);

static struct OGLES2Pipeline *pipe_by_name(struct OGLES2Context *ctx, GLuint name)
{
    return name && name < ctx->npipes ? ctx->pipes[name] : 0;
}

/* A program a pipeline let go of: freed if deleted and nothing else holds it. */
static void pipe_release(struct OGLES2Context *ctx, struct OGLES2Program *p)
{
    if (p && p->delete_pending && !in_use(ctx, p))
        free_program(ctx, p);
}

static void pipe_free(struct OGLES2Context *ctx, struct OGLES2Pipeline *pp)
{
    struct OGLES2Program *held[4];
    int k;
    for (k = 0; k < 3; k++)
        held[k] = pp->stage[k];
    held[3] = pp->active;
    pair_free(ctx, pp);
    ctx->pipes[pp->name] = 0;
    if (pp->info_log)
        FreeVec(pp->info_log);
    FreeVec(pp);
    /* each once: a program of two stages, or a stage's and the active one,
     * was released twice, freed by the first (2026-10-28: the A1200's
     * recoverable alerts in dEQP's separate_shader.random) */
    for (k = 0; k < 4; k++) {
        int j;
        for (j = 0; j < k && held[j] != held[k]; j++)
            ;
        if (j == k)
            pipe_release(ctx, held[k]);
    }
}

static void pipelines_free(struct OGLES2Context *ctx)
{
    GLuint i;
    ctx->pipe = 0;
    for (i = 0; i < ctx->npipes; i++)
        if (ctx->pipes[i])
            pipe_free(ctx, ctx->pipes[i]);
    if (ctx->pipes)
        FreeVec(ctx->pipes);
    ctx->pipes = 0;
    ctx->npipes = 0;
}

static void pipe_log(struct OGLES2Pipeline *pp, const char *a)
{
    ULONG n = str_len(a);
    if (pp->info_log)
        FreeVec(pp->info_log);
    if ((pp->info_log = AllocVec(n + 1, MEMF_ANY)))
        memcpy(pp->info_log, a, n + 1);
}

void ogles2_glGenProgramPipelines(struct OGLES2Context *ctx, GLsizei n, GLuint *pipelines)
{
    GLsizei k;
    GLuint name = 1;
    if (!ogles2_es31_entry(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < n && pipelines; k++) {
        struct OGLES2Pipeline *pp;
        while (name < ctx->npipes && ctx->pipes[name])
            name++;
        if (name >= ctx->npipes) {
            GLuint nn = ctx->npipes ? ctx->npipes * 2 : 16;
            struct OGLES2Pipeline **t = AllocVec(nn * sizeof(*t), MEMF_ANY | MEMF_CLEAR);
            if (!t) {
                ogles2_error(ctx, GL_OUT_OF_MEMORY);
                return;
            }
            if (ctx->pipes) {
                memcpy(t, ctx->pipes, ctx->npipes * sizeof(*t));
                FreeVec(ctx->pipes);
            }
            ctx->pipes = t;
            ctx->npipes = nn;
        }
        if (!(pp = AllocVec(sizeof(*pp), MEMF_ANY | MEMF_CLEAR))) {
            ogles2_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        pp->name = name;
        ctx->pipes[name] = pp;
        pipelines[k] = name;
    }
}

void ogles2_glDeleteProgramPipelines(struct OGLES2Context *ctx, GLsizei n, const GLuint *pipelines)
{
    GLsizei k;
    if (!ogles2_es31_entry(ctx))
        return;
    if (n < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < n && pipelines; k++) {
        struct OGLES2Pipeline *pp = pipe_by_name(ctx, pipelines[k]);
        if (!pp)
            continue;
        if (ctx->pipe == pp) /* the bound one: 0 bound */
            ctx->pipe = 0;
        pipe_free(ctx, pp);
    }
}

GLboolean ogles2_glIsProgramPipeline(struct OGLES2Context *ctx, GLuint pipeline)
{
    struct OGLES2Pipeline *pp;
    if (!ogles2_es31_entry(ctx))
        return GL_FALSE;
    pp = pipe_by_name(ctx, pipeline);
    return pp && pp->bound_once ? GL_TRUE : GL_FALSE;
}

void ogles2_glBindProgramPipeline(struct OGLES2Context *ctx, GLuint pipeline)
{
    struct OGLES2Pipeline *pp = 0;
    if (!ogles2_es31_entry(ctx))
        return;
    if (pipeline && !(pp = pipe_by_name(ctx, pipeline))) { /* a name glGenProgramPipelines did not give */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (ogles2_xfb_drawing(ctx)) { /* ES 3.1 12.1: not while transform feedback is active and not paused */
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (pp)
        pp->bound_once = TRUE;
    ctx->pipe = pp;
}

void ogles2_glUseProgramStages(struct OGLES2Context *ctx, GLuint pipeline, GLbitfield stages, GLuint program)
{
    static const GLbitfield bits[3] = { GL_VERTEX_SHADER_BIT, GL_FRAGMENT_SHADER_BIT, GL_COMPUTE_SHADER_BIT };
    struct OGLES2Pipeline *pp;
    struct OGLES2Program *p = 0, *old;
    int k;
    if (!ogles2_es31_entry(ctx))
        return;
    if (!(pp = pipe_by_name(ctx, pipeline))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    pp->bound_once = TRUE;
    if (stages != GL_ALL_SHADER_BITS &&
        (stages & ~(GLbitfield)(GL_VERTEX_SHADER_BIT | GL_FRAGMENT_SHADER_BIT | GL_COMPUTE_SHADER_BIT))) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ctx->pipe == pp && ogles2_xfb_drawing(ctx)) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (program) {
        if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
            return;
        /* linked, and separable (the parameter as it is now, Mesa's) */
        if (!p->linked || !p->separable) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    for (k = 0; k < 3; k++) {
        struct OGLES2Program *s;
        if (!(stages & bits[k]))
            continue;
        /* the stage the program has, else none */
        s = !p || !p->image ? 0
          : k == 2          ? (p->image->compute ? p : 0)
          : !p->image->compute && (p->image->stages & (k ? 0x10 : 0x01)) ? p : 0;
        old = pp->stage[k];
        pp->stage[k] = s;
        if (old != s)
            pipe_release(ctx, old);
    }
}

void ogles2_glActiveShaderProgram(struct OGLES2Context *ctx, GLuint pipeline, GLuint program)
{
    struct OGLES2Pipeline *pp;
    struct OGLES2Program *p = 0, *old;
    if (!ogles2_es31_entry(ctx))
        return;
    pp = pipe_by_name(ctx, pipeline);
    if (program && !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (!pp) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    pp->bound_once = TRUE;
    if (p && !p->linked) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    old = pp->active;
    pp->active = p;
    if (old != p)
        pipe_release(ctx, old);
}

/* ES 3.1 7.4.1: the producer's outputs and the consumer's inputs match
 * exactly (Mesa's validate_io): every input an output, by location if both
 * have one, else by name; the same type, interpolation and precision; no
 * output left over. Built-ins do not count. */
static BOOL builtin_var(const struct OGLES2ImageVar *v)
{
    return v->name[0] == 'g' && v->name[1] == 'l' && v->name[2] == '_';
}

static BOOL interfaces_match(const struct OGLES2Image *out, const struct OGLES2Image *in)
{
    UBYTE used[256];
    UWORD i, j, nout = 0;
    memset(used, 0, sizeof(used));
    for (i = 0; i < out->ninterface; i++)
        if (out->interface[i].iface == GL_PROGRAM_OUTPUT && !builtin_var(&out->interface[i]))
            nout++;
    for (i = 0; i < in->ninterface; i++) {
        const struct OGLES2ImageVar *c = &in->interface[i], *pv = 0;
        UBYTE ci, pi;
        if (c->iface != GL_PROGRAM_INPUT || builtin_var(c))
            continue;
        for (j = 0; j < out->ninterface && j < 256 && !pv; j++) {
            const struct OGLES2ImageVar *o = &out->interface[j];
            if (o->iface != GL_PROGRAM_OUTPUT || builtin_var(o) || used[j])
                continue;
            if ((c->flags & 1) ? ((o->flags & 1) && o->location == c->location)
                               : (!(o->flags & 1) && str_eq(o->name, c->name))) {
                pv = o;
                used[j] = 1;
            }
        }
        if (!pv)
            return FALSE;
        nout--;
        ci = c->interpolation ? c->interpolation : 1; /* none: smooth */
        pi = pv->interpolation ? pv->interpolation : 1;
        if (pv->type != c->type || pv->array != c->array || ci != pi || pv->precision != c->precision)
            return FALSE;
        /* the same structure (Mesa: outermost_struct_type) */
        if (out->interface_types && in->interface_types &&
            out->interface_types[pv - out->interface] != in->interface_types[c - in->interface])
            return FALSE;
    }
    return nout == 0;
}

/* Mesa's _mesa_validate_program_pipeline, for ES 3.1's stages. */
static BOOL pipe_validate(struct OGLES2Pipeline *pp)
{
    int k;
    pp->validated = GL_FALSE;
    if (pp->info_log)
        FreeVec(pp->info_log);
    pp->info_log = 0;
    /* a program active for some of the stages it was linked with, not all */
    for (k = 0; k < 2; k++) {
        struct OGLES2Program *p = pp->stage[k];
        if (p && p->image && !p->image->compute && p->image->stages == 0x11 && pp->stage[1 - k] != p) {
            pipe_log(pp, "Program is not active for all shaders that was linked");
            return FALSE;
        }
    }
    for (k = 0; k < 3; k++)
        if (pp->stage[k] && (!pp->stage[k]->image || !pp->stage[k]->image->separable)) {
            pipe_log(pp, "Program was relinked without PROGRAM_SEPARABLE state");
            return FALSE;
        }
    if (!pp->stage[0] && !pp->stage[1] && !pp->stage[2])
        return FALSE; /* empty */
    if (pp->stage[0] && pp->stage[1] && pp->stage[0] != pp->stage[1] &&
        !interfaces_match(pp->stage[0]->image, pp->stage[1]->image))
        return FALSE;
    pp->validated = GL_TRUE;
    return TRUE;
}

/* draw.c and compute.c: the bound pipeline (no program in use) is valid for
 * a draw, which then needs a vertex and a fragment program (ES 3.1
 * 11.1.3.11), or for a dispatch, its compute program. */
BOOL ogles2_pipeline_ready(struct OGLES2Context *ctx, BOOL compute)
{
    struct OGLES2Pipeline *pp = ctx->pipe;
    if (!pp || !pipe_validate(pp))
        return FALSE;
    return compute ? pp->stage[2] != 0 : pp->stage[0] && pp->stage[1];
}

/* A pipeline's pair program let go of (its stages or their images changed,
 * the pipeline deleted). */
static void pair_free(struct OGLES2Context *ctx, struct OGLES2Pipeline *pp)
{
    struct OGLES2Program *q = pp->pair;
    if (q) {
        if (ctx->program == q)
            ctx->program = 0;
        unlink_program(ctx, q);
        if (q->vs)
            FreeVec(q->vs); /* hidden shaders: their sources are the images' */
        if (q->fs)
            FreeVec(q->fs);
        if (q->info_log)
            FreeVec(q->info_log);
        if (q->xfb_names)
            FreeVec(q->xfb_names);
        FreeVec(q);
    }
    if (pp->pair_loc)
        FreeVec(pp->pair_loc);
    if (pp->pair_blk)
        FreeVec(pp->pair_blk);
    pp->pair = 0;
    pp->pair_loc = 0;
    pp->pair_blk = 0;
}

/* The image of a pipeline of vertex program image a and fragment program
 * image b: composed of theirs (ogles2_v3dp_compose), kept in the program
 * cache by its hash (BLAKE3 of "PIPV" a's hash and "PIPF" b's, as the
 * program hash's pseudo-stages; FORMAT.md), its variants its .v3dx's:
 * shipped, cached, else compiled through a Mesa pipeline of the two
 * (V3DGLSL_LINK_PIPELINE). NULL: no memory, no variants (logged). */
static struct OGLES2Image *pair_image(struct OGLES2Context *ctx, const struct OGLES2Image *a,
                                      const struct OGLES2Image *b)
{
    static const char digits[] = "0123456789abcdef";
    char line[140], hex[FILE_HASH_DIGITS + 1], path[96];
    UBYTE hash[32];
    struct blake3 h;
    struct OGLES2Image *im;
    const struct OGLES2Image *x;
    int k, s;
    /* "PIPE <a's hash> <b's hash>\n", the hex the pseudo-stages hash */
    memcpy(line, "PIPE ", 5);
    for (s = 0; s < 2; s++)
        for (k = 0; k < 32; k++) {
            const UBYTE *hh = s ? b->hash : a->hash;
            line[5 + 65 * s + 2 * k] = digits[hh[k] >> 4];
            line[5 + 65 * s + 2 * k + 1] = digits[hh[k] & 15];
        }
    line[69] = ' ';
    line[134] = '\n';
    line[135] = 0;
    blake3_init(&h);
    for (s = 0; s < 2; s++) {
        ULONG stage = s ? 0x50495046UL : 0x50495056UL; /* "PIPF", "PIPV" */
        UBYTE w[8];
        for (k = 0; k < 4; k++) {
            w[k] = (UBYTE)(stage >> (8 * k));
            w[4 + k] = (UBYTE)(64 >> (8 * k));
        }
        blake3_update(&h, w, 8);
        blake3_update(&h, line + 5 + 65 * s, 64);
    }
    blake3_final(&h, hash);
    for (im = ctx->share->images; im && memcmp(im->hash, hash, 32); im = im->next)
        ;
    if (!im) {
        if (!(im = ogles2_v3dp_compose(a, b, hash, line)))
            return 0;
        im->next = ctx->share->images;
        ctx->share->images = im;
    }
    if (im->nvariants)
        return im;
    if (!(x = ogles2_image_x(im, FALSE)) && !(x = ogles2_image_x(im, TRUE)) && ogles2_compiler_installed()) {
        char *log = AllocVec(1024, MEMF_ANY | MEMF_CLEAR);
        image_hex(im, hex);
        file_path(path, ogles2_cache_dir(), hex);
        if (log && ogles2_compile_pipeline(im->vs_src, im->fs_src, im->bind_src, im->layout_crc, path, log, 1024)) {
            im->xc_tried = FALSE;
            x = ogles2_image_x(im, TRUE);
        } else {
            ogles2_log("pipeline: the compiler made no variants for it: %s", (ULONG)(log ? log : "out of memory"));
        }
        if (log)
            FreeVec(log);
    }
    if (!x || !x->nvariants)
        return 0;
    /* its variants the .v3dx's (kept: im->xs or im->xc, and older ones chained) */
    im->variants = x->variants;
    im->nvariants = x->nvariants;
    im->code_mem = x->code_mem;
    im->code_total = x->code_total;
    return im;
}

/* A uniform of program s by name; 0xFFFF none. */
static UWORD stage_uniform(const struct OGLES2Program *s, const char *name)
{
    UWORD k;
    for (k = 0; k < s->image->nuniforms; k++)
        if (str_eq(s->image->uniforms[k].name, name))
            return k;
    return 0xFFFF;
}

/* The pair program of pipeline pp: vertex program a's vertex shader and
 * fragment program b's fragment shader linked as one separable program with
 * a's link state (its bindings and transform feedback varyings; each stage
 * then compiles as in its own program), and the maps sync_pair copies the
 * stage programs' units and bindings by. */
static BOOL pair_link(struct OGLES2Context *ctx, struct OGLES2Pipeline *pp)
{
    struct OGLES2Program *a = pp->stage[0], *b = pp->stage[1], *q;
    struct OGLES2Image *im;
    UWORD k, n;
    if (!a->image->vs_src || !b->image->fs_src)
        return FALSE;
    if (!(im = pair_image(ctx, a->image, b->image)))
        return FALSE;
    if (!(q = AllocVec(sizeof(*q), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    q->kind = OGLES2_OBJ_PROGRAM;
    q->separable = TRUE;
    memcpy(q->bind_name, a->bind_name, sizeof(q->bind_name)); /* the attributes' locations as a's */
    memcpy(q->bind_index, a->bind_index, sizeof(q->bind_index));
    q->nbindings = a->nbindings;
    pp->pair = q;
    if (!bind_image(q, im)) {
        ogles2_log("pipeline %ld: no memory for its programs %ld and %ld", (ULONG)pp->name, (ULONG)a->name,
                   (ULONG)b->name);
        return FALSE;
    }
    q->linked = GL_TRUE;
    /* each sampler's or image's unit from its stage's program (by name) */
    n = q->nlocations;
    if (!(pp->pair_loc = AllocVec(n * 4UL + 4, MEMF_ANY)) ||
        !(pp->pair_blk = AllocVec(q->image->nublocks * 2UL + 2, MEMF_ANY)))
        return FALSE;
    for (k = 0; k < n; k++) {
        const struct OGLES2ImageUniform *u;
        const struct OGLES2Program *s;
        UWORD su;
        int st;
        pp->pair_loc[k] = 0xFFFFFFFFUL;
        if (q->loc_uniform[k] >= OGLES2_LOC_INACTIVE)
            continue;
        u = &q->image->uniforms[q->loc_uniform[k]];
        if (!(u->flags & (OGLES2_UNIFORM_SAMPLER | OGLES2_UNIFORM_IMAGE)))
            continue;
        st = u->sampler[1] != ~0UL ? 1 : 0;
        s = pp->stage[st];
        if ((su = stage_uniform(s, u->name)) != 0xFFFF && s->uniform_loc[su] != 0xFFFF)
            pp->pair_loc[k] = ((ULONG)st << 16) | (ULONG)(s->uniform_loc[su] + q->loc_element[k]);
    }
    /* each uniform block's binding from its stage's program */
    for (k = 0; k < q->image->nublocks; k++) {
        const struct OGLES2ImageBlock *bl = &q->image->ublocks[k];
        const struct OGLES2Program *s = pp->stage[(bl->stages & 1) ? 0 : 1];
        UWORD j;
        pp->pair_blk[k] = 0xFFFF;
        for (j = 0; j < s->image->nublocks; j++)
            if (str_eq(s->image->ublocks[j].name, bl->name))
                pp->pair_blk[k] = (UWORD)(((bl->stages & 1) ? 0 : 0x8000) | j);
    }
    return TRUE;
}

/* The stage programs' uniform values (each stage's constant buffer as its
 * program has it: the same shader, the same layout), units and block
 * bindings into the pair program. */
static void pair_sync(struct OGLES2Pipeline *pp)
{
    struct OGLES2Program *q = pp->pair;
    UWORD k;
    int st;
    for (st = 0; st < 2; st++) {
        struct OGLES2Program *s = pp->stage[st];
        ULONG n = q->image->cbuf_words[st];
        if (pp->pair_src[st] == s && pp->pair_ver[st] == s->cbuf_ver[st])
            continue;
        if (s->image->cbuf_words[st] < n)
            n = s->image->cbuf_words[st];
        if (n && q->cbuf[st] && s->cbuf[st]) {
            memcpy(q->cbuf[st], s->cbuf[st], n * 4);
            memcpy(q->cbuf_le[st], s->cbuf_le[st], n * 4);
        }
        q->cbuf_ver[st]++; /* draw.c's kept streams made again; the state variables written again */
        pp->pair_src[st] = s;
        pp->pair_ver[st] = s->cbuf_ver[st];
    }
    for (k = 0; k < q->nlocations; k++)
        if (pp->pair_loc[k] != 0xFFFFFFFFUL) {
            const struct OGLES2Program *s = pp->stage[pp->pair_loc[k] >> 16];
            q->sampler_unit[k] = s->sampler_unit[pp->pair_loc[k] & 0xFFFF];
        }
    for (k = 0; k < q->image->nublocks; k++)
        if (pp->pair_blk[k] != 0xFFFF)
            q->block_binding[k] = pp->stage[pp->pair_blk[k] >> 15]->block_binding[pp->pair_blk[k] & 0x7FFF];
}

struct OGLES2Program *ogles2_pipeline_program(struct OGLES2Context *ctx, BOOL compute)
{
    struct OGLES2Pipeline *pp = ctx->pipe;
    if (!ogles2_pipeline_ready(ctx, compute))
        return 0;
    if (compute)
        return pp->stage[2];
    if (pp->stage[0] == pp->stage[1]) /* one program of both stages */
        return pp->stage[0];
    if (!pp->pair || pp->pair_img[0] != pp->stage[0]->image || pp->pair_img[1] != pp->stage[1]->image) {
        pair_free(ctx, pp);
        pp->pair_img[0] = pp->stage[0]->image;
        pp->pair_img[1] = pp->stage[1]->image;
        pp->pair_src[0] = pp->pair_src[1] = 0;
        if (!pair_link(ctx, pp)) {
            pair_free(ctx, pp);
            pp->pair_img[0] = pp->pair_img[1] = 0;
            return 0;
        }
    }
    pair_sync(pp);
    return pp->pair;
}

void ogles2_glValidateProgramPipeline(struct OGLES2Context *ctx, GLuint pipeline)
{
    struct OGLES2Pipeline *pp;
    if (!ogles2_es31_entry(ctx))
        return;
    if (!(pp = pipe_by_name(ctx, pipeline))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    pp->bound_once = TRUE;
    pipe_validate(pp);
}

void ogles2_glGetProgramPipelineiv(struct OGLES2Context *ctx, GLuint pipeline, GLenum pname, GLint *params)
{
    struct OGLES2Pipeline *pp;
    GLint v;
    if (!ogles2_es31_entry(ctx))
        return;
    if (!(pp = pipe_by_name(ctx, pipeline))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    pp->bound_once = TRUE;
    switch (pname) {
    case GL_ACTIVE_PROGRAM:     v = pp->active ? (GLint)pp->active->name : 0; break;
    case GL_VERTEX_SHADER:      v = pp->stage[0] ? (GLint)pp->stage[0]->name : 0; break;
    case GL_FRAGMENT_SHADER:    v = pp->stage[1] ? (GLint)pp->stage[1]->name : 0; break;
    case GL_COMPUTE_SHADER:     v = pp->stage[2] ? (GLint)pp->stage[2]->name : 0; break;
    case GL_VALIDATE_STATUS:    v = pp->validated; break;
    case GL_INFO_LOG_LENGTH:    v = pp->info_log && pp->info_log[0] ? (GLint)str_len(pp->info_log) + 1 : 0; break;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (params)
        *params = v;
}

void ogles2_glGetProgramPipelineInfoLog(struct OGLES2Context *ctx, GLuint pipeline, GLsizei bufSize,
                                        GLsizei *length, GLchar *infoLog)
{
    struct OGLES2Pipeline *pp;
    GLsizei n;
    if (!ogles2_es31_entry(ctx))
        return;
    if (!(pp = pipe_by_name(ctx, pipeline))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    n = pp->info_log ? (GLsizei)str_len(pp->info_log) : 0;
    if (n > bufSize - 1)
        n = bufSize > 0 ? bufSize - 1 : 0;
    if (infoLog && bufSize > 0) {
        if (n)
            memcpy(infoLog, pp->info_log, (ULONG)n);
        infoLog[n] = 0;
    }
    if (length)
        *length = n;
}

/* ES 3.1 7.3: a separable program of one shader, made, compiled and linked
 * at once (Mesa's _mesa_CreateShaderProgramv): returned even when the
 * shader did not compile, its log then the program's. */
GLuint ogles2_glCreateShaderProgramv(struct OGLES2Context *ctx, GLenum type, GLsizei count,
                                     const GLchar *const *strings)
{
    GLuint s, prog = 0;
    struct OGLES2Program *p;
    struct OGLES2Shader *sh;
    if (!ogles2_es31_entry(ctx))
        return 0;
    if (!(s = ogles2_glCreateShader(ctx, type))) /* GL_INVALID_ENUM for another type */
        return 0;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        ogles2_glDeleteShader(ctx, s);
        return 0;
    }
    ogles2_glShaderSource(ctx, s, count, strings, 0);
    ogles2_glCompileShader(ctx, s);
    if ((prog = ogles2_glCreateProgram(ctx)) && (p = ogles2_object(ctx, prog, OGLES2_OBJ_PROGRAM)) &&
        (sh = ogles2_object(ctx, s, OGLES2_OBJ_SHADER))) {
        p->separable = TRUE;
        if (sh->compiled) {
            ogles2_glAttachShader(ctx, prog, s);
            ogles2_glLinkProgram(ctx, prog);
            ogles2_glDetachShader(ctx, prog, s);
        } else {
            set_log(p, sh->info_log ? sh->info_log : "", 0);
        }
    }
    ogles2_glDeleteShader(ctx, s);
    return prog;
}

/* ES 3.0's names (stage C item 4): the extension's, in ES3 contexts. */
void ogles2_glGetProgramBinary(struct OGLES2Context *ctx, GLuint program, GLsizei bufSize,
                               GLsizei *length, GLenum *binaryFormat, void *binary)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glGetProgramBinaryOES(ctx, program, bufSize, length, binaryFormat, binary);
}

void ogles2_glProgramBinary(struct OGLES2Context *ctx, GLuint program, GLenum binaryFormat,
                            const void *binary, GLsizei length)
{
    if (ogles2_es3_entry(ctx))
        ogles2_glProgramBinaryOES(ctx, program, binaryFormat, binary, length);
}

void ogles2_glProgramParameteri(struct OGLES2Context *ctx, GLuint program, GLenum pname, GLint value)
{
    struct OGLES2Program *p;
    if (!ogles2_es3_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (pname != GL_PROGRAM_BINARY_RETRIEVABLE_HINT && !(ogles2_es31(ctx) && pname == GL_PROGRAM_SEPARABLE)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (value != GL_FALSE && value != GL_TRUE) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (pname == GL_PROGRAM_SEPARABLE) /* ES 3.1: for the next link; queried as set (Mesa's) */
        p->separable = (UBYTE)value;
    else
        p->binary_hint_pending = (UBYTE)value;
}

void ogles2_glGetProgramInfoLog(struct OGLES2Context *ctx, GLuint program, GLsizei bufSize,
                                GLsizei *length, GLchar *infoLog)
{
    struct OGLES2Program *p;
    GLsizei n;
    if (!ogles2_context_valid(ctx))
        return;
    if (bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    n = copy_out(infoLog, bufSize, p->info_log ? p->info_log : "");
    if (length)
        *length = n;
}

/* Linked program for a location query: GL_INVALID_OPERATION if not linked. */
static struct OGLES2Program *linked(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p = lookup(ctx, program, OGLES2_OBJ_PROGRAM);
    if (p && !p->linked) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    return p;
}

GLint ogles2_glGetAttribLocation(struct OGLES2Context *ctx, GLuint program, const GLchar *name)
{
    struct OGLES2Program *p;
    int a;
    if (!ogles2_context_valid(ctx) || !name || !(p = linked(ctx, program)))
        return -1;
    for (a = 0; a < p->image->nattribs; a++)
        if (p->image->attribs[a].slot != 0xff && str_eq(p->image->attribs[a].name, name))
            return p->attrib_index[a];
    return -1;
}

/* ES 3.0 3.9.2: a fragment output's colour number, from the program's
 * interface (format 2): "name", or "name[i]" of an array; -1 for any other
 * name, a built-in, and a GLSL ES 1.00 program (gl_FragColor and
 * gl_FragData are no outputs of its own). */
GLint ogles2_glGetFragDataLocation(struct OGLES2Context *ctx, GLuint program, const GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageVar *v;
    UWORD k;
    if (!ogles2_es3_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return -1;
    if (!p->linked) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return -1;
    }
    if (!name)
        return -1;
    for (k = 0; k < p->image->ninterface; k++) {
        ULONG n, e = 0;
        const char *q;
        v = &p->image->interface[k];
        if (v->iface != GL_PROGRAM_OUTPUT || v->location < 0)
            continue;
        n = str_len(v->name);
        if (v->array && n > 3 && !strncmp(v->name + n - 3, "[0]", 3))
            n -= 3; /* an array may be named for its first element */
        if (strncmp(v->name, name, n))
            continue;
        q = name + n;
        if (*q == '[' && v->array) { /* an element: "name[i]" */
            for (q++; *q >= '0' && *q <= '9' && e < 0x10000; q++)
                e = e * 10 + (ULONG)(*q - '0');
            if (q == name + n + 1 || *q != ']' || q[1] || e >= v->array)
                continue;
        } else if (*q) {
            continue;
        }
        return v->location + (GLint)e;
    }
    return -1;
}

void ogles2_glGetActiveAttrib(struct OGLES2Context *ctx, GLuint program, GLuint index,
                              GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type,
                              GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageAttrib *a;
    GLsizei n;
    if (!ogles2_context_valid(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (!p->image || !(a = active_attrib(p->image, index)) || bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    n = copy_out(name, bufSize, a->name);
    if (length)
        *length = n;
    if (size)
        *size = 1;
    if (type)
        *type = a->type;
}

/* ---- uniforms ----
 * Locations number every array element of every visible uniform, in the
 * image's order (the .v3dp leaves the numbering to the runtime). */

GLint ogles2_glGetUniformLocation(struct OGLES2Context *ctx, GLuint program, const GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2Image *im;
    ULONG len, base;
    LONG index = -1;
    UWORD loc;
    if (!ogles2_context_valid(ctx) || !name || !(p = linked(ctx, program)))
        return -1;
    im = p->image;
    len = str_len(name);
    base = len;
    /* "a[3]": an array element; "a" and "a[0]" are its first. */
    if (len > 3 && name[len - 1] == ']') {
        ULONG i = len - 2, v = 0, mul = 1;
        while (i > 0 && name[i] >= '0' && name[i] <= '9') {
            v += (ULONG)(name[i] - '0') * mul;
            mul *= 10;
            i--;
        }
        if (name[i] == '[' && i < len - 2) {
            base = i;
            index = (LONG)v;
        }
    }
    for (loc = 0; loc < p->nlocations; loc++) {
        const struct OGLES2ImageUniform *u;
        if (p->loc_element[loc] || p->loc_uniform[loc] >= OGLES2_LOC_INACTIVE) /* an element past the first, a gap */
            continue;
        u = &im->uniforms[p->loc_uniform[loc]];
        if (u->flags & OGLES2_UNIFORM_ATOMIC) /* a location of its own, but -1 by name (Mesa's) */
            continue;
        if (str_eq(u->name, name))
            return loc; /* the whole name, struct members included */
        if (index >= 0 && u->array && (LONG)u->array > index && str_len(u->name) == base &&
            !strncmp(u->name, name, base))
            return loc + (GLint)index;
    }
    return -1;
}

void ogles2_glGetActiveUniform(struct OGLES2Context *ctx, GLuint program, GLuint index,
                               GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type,
                               GLchar *name)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageUniform *u;
    GLsizei n;
    if (!ogles2_context_valid(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return;
    if (!p->image || !(u = active_uniform(p->image, index)) || bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    n = copy_out(name, bufSize, u->name);
    if (u->array && name && n + 3 < bufSize) {
        name[n++] = '[';
        name[n++] = '0';
        name[n++] = ']';
        name[n] = 0;
    }
    if (length)
        *length = n;
    if (size)
        *size = u->array ? u->array : 1;
    if (type)
        *type = u->type;
}

/* Kinds of uniform data. */
#define K_FLOAT 0
#define K_INT   1
#define K_BOOL  2
#define K_SAMPLER 3
#define K_UINT  4 /* GLSL ES 3.00 */
#define K_IMAGE 5 /* GLSL ES 3.10: its unit is layout(binding)'s, glUniform* cannot change it */

/* Kind, components per column and columns of a GL uniform type; 0 if unknown. */
static int type_shape(GLenum t, int *kind, int *comps, int *cols)
{
    *cols = 1;
    switch (t) {
    case GL_FLOAT:        *kind = K_FLOAT; *comps = 1; return 1;
    case GL_FLOAT_VEC2:   *kind = K_FLOAT; *comps = 2; return 1;
    case GL_FLOAT_VEC3:   *kind = K_FLOAT; *comps = 3; return 1;
    case GL_FLOAT_VEC4:   *kind = K_FLOAT; *comps = 4; return 1;
    case GL_INT:          *kind = K_INT;   *comps = 1; return 1;
    case GL_INT_VEC2:     *kind = K_INT;   *comps = 2; return 1;
    case GL_INT_VEC3:     *kind = K_INT;   *comps = 3; return 1;
    case GL_INT_VEC4:     *kind = K_INT;   *comps = 4; return 1;
    case GL_BOOL:         *kind = K_BOOL;  *comps = 1; return 1;
    case GL_BOOL_VEC2:    *kind = K_BOOL;  *comps = 2; return 1;
    case GL_BOOL_VEC3:    *kind = K_BOOL;  *comps = 3; return 1;
    case GL_BOOL_VEC4:    *kind = K_BOOL;  *comps = 4; return 1;
    case GL_FLOAT_MAT2:   *kind = K_FLOAT; *comps = 2; *cols = 2; return 1;
    case GL_FLOAT_MAT3:   *kind = K_FLOAT; *comps = 3; *cols = 3; return 1;
    case GL_FLOAT_MAT4:   *kind = K_FLOAT; *comps = 4; *cols = 4; return 1;
    /* GLSL ES 3.00: unsigned, and non-square matrices (matCxR: C columns of R) */
    case GL_UNSIGNED_INT:      *kind = K_UINT; *comps = 1; return 1;
    case GL_UNSIGNED_INT_VEC2: *kind = K_UINT; *comps = 2; return 1;
    case GL_UNSIGNED_INT_VEC3: *kind = K_UINT; *comps = 3; return 1;
    case GL_UNSIGNED_INT_VEC4: *kind = K_UINT; *comps = 4; return 1;
    case GL_FLOAT_MAT2x3: *kind = K_FLOAT; *comps = 3; *cols = 2; return 1;
    case GL_FLOAT_MAT2x4: *kind = K_FLOAT; *comps = 4; *cols = 2; return 1;
    case GL_FLOAT_MAT3x2: *kind = K_FLOAT; *comps = 2; *cols = 3; return 1;
    case GL_FLOAT_MAT3x4: *kind = K_FLOAT; *comps = 4; *cols = 3; return 1;
    case GL_FLOAT_MAT4x2: *kind = K_FLOAT; *comps = 2; *cols = 4; return 1;
    case GL_FLOAT_MAT4x3: *kind = K_FLOAT; *comps = 3; *cols = 4; return 1;
    case GL_SAMPLER_2D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_RECT: /* ARB_texture_rectangle */
    case GL_SAMPLER_3D: case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_SHADOW:
    case GL_SAMPLER_CUBE_SHADOW: case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_INT_SAMPLER_2D: case GL_INT_SAMPLER_3D: case GL_INT_SAMPLER_CUBE: case GL_INT_SAMPLER_2D_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_CUBE:
    case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
    case GL_SAMPLER_2D_MULTISAMPLE: case GL_INT_SAMPLER_2D_MULTISAMPLE: /* GLSL ES 3.10 */
    case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        *kind = K_SAMPLER; *comps = 1; return 1;
    case GL_IMAGE_2D: case GL_IMAGE_3D: case GL_IMAGE_CUBE: case GL_IMAGE_2D_ARRAY:
    case GL_INT_IMAGE_2D: case GL_INT_IMAGE_3D: case GL_INT_IMAGE_CUBE: case GL_INT_IMAGE_2D_ARRAY:
    case GL_UNSIGNED_INT_IMAGE_2D: case GL_UNSIGNED_INT_IMAGE_3D: case GL_UNSIGNED_INT_IMAGE_CUBE:
    case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        *kind = K_IMAGE; *comps = 1; return 1;
    default:              return 0;
    }
}

static ULONG fbits(GLfloat f)
{
    union { GLfloat f; ULONG u; } c;
    c.f = f;
    return c.u;
}

/* glUniform* and glUniformMatrix*: `count` values of `comps` x `cols`
 * components, floats (fv) or ints (iv; unsigned ones too), into the current
 * program's uniform at `location`. `kind` is the call's (K_FLOAT for f,
 * K_INT for i, K_UINT for ui). */
static void set_uniform(struct OGLES2Context *ctx, GLint location, GLsizei count, int kind,
                        int comps, int cols, const GLfloat *fv, const GLint *iv)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageUniform *u;
    int ukind, ucomps, ucols, c, k, pl;
    UWORD e, first, ne;
    GLsizei i;
    if (!ogles2_context_valid(ctx))
        return;
    /* ES 3.1: without one in use, the bound pipeline's active program */
    if (!(p = ctx->program) && !(ctx->pipe && (p = ctx->pipe->active))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (location == -1) /* silently ignored (GLES 2.0 section 2.10.4) */
        return;
    if (count < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (location < 0 || location >= p->nlocations || p->loc_uniform[location] == OGLES2_LOC_NONE) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (p->loc_uniform[location] == OGLES2_LOC_INACTIVE) /* ES 3.1 7.6.1: ignored */
        return;
    u = &p->image->uniforms[p->loc_uniform[location]];
    first = p->loc_element[location];
    if (!type_shape(u->type, &ukind, &ucomps, &ucols) || ucomps != comps || ucols != cols ||
        (count > 1 && !u->array) ||
        !(ukind == kind || ukind == K_BOOL || (ukind == K_SAMPLER && kind == K_INT && comps == 1))) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (!count || (!fv && !iv))
        return;
    ne = u->array ? u->array : 1;
    if (count > ne - first)
        count = ne - first; /* past the array's end: ignored (GLES 2.0 2.10.4) */
    if (ukind == K_SAMPLER) {
        for (i = 0; i < count; i++)
            if (iv[i] < 0 || iv[i] >= OGLES2_MAX_TEXTURE_UNITS) { /* a unit, 0-47 (combined) */
                ogles2_error(ctx, GL_INVALID_VALUE);
                return;
            }
        for (i = 0; i < count; i++)
            p->sampler_unit[location + i] = (UBYTE)iv[i];
        return;
    }
    for (pl = 0; pl < u->nplaces; pl++) {
        const struct OGLES2Place *place = &u->places[pl];
        for (i = 0, e = first; i < count; i++, e++)
            for (c = 0; c < cols; c++)
                for (k = 0; k < comps; k++) {
                    int src = (i * cols + c) * comps + k;
                    ULONG w;
                    ULONG at = place->offset + (ULONG)e * place->elem_stride + (ULONG)c * place->vec_stride + k;
                    if (ukind == K_BOOL) {
                        BOOL b = kind == K_FLOAT ? fv[src] != 0.0f : iv[src] != 0;
                        w = place->int_as_float ? (b ? fbits(1.0f) : 0) : (b ? ~0UL : 0);
                    } else if (ukind == K_INT) {
                        w = place->int_as_float ? fbits((GLfloat)iv[src]) : (ULONG)iv[src];
                    } else if (ukind == K_UINT) {
                        w = place->int_as_float ? fbits((GLfloat)(GLuint)iv[src]) : (ULONG)iv[src];
                    } else {
                        w = fbits(fv[src]);
                    }
                    if (at < p->image->cbuf_words[place->stage])
                        ogles2_cbuf_set(p, place->stage, at, w);
                }
    }
}

void ogles2_glUniform1fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 1, 1, value, 0);
}

void ogles2_glUniform1iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 1, 1, 0, value);
}

void ogles2_glUniform2fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 2, 1, value, 0);
}

void ogles2_glUniform2iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 2, 1, 0, value);
}

void ogles2_glUniform3fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 3, 1, value, 0);
}

void ogles2_glUniform3iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 3, 1, 0, value);
}

void ogles2_glUniform4fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLfloat *value)
{
    set_uniform(ctx, location, count, K_FLOAT, 4, 1, value, 0);
}

void ogles2_glUniform4iv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                         const GLint *value)
{
    set_uniform(ctx, location, count, K_INT, 4, 1, 0, value);
}

void ogles2_glUniform1f(struct OGLES2Context *ctx, GLint location, GLfloat v0)
{
    set_uniform(ctx, location, 1, K_FLOAT, 1, 1, &v0, 0);
}

void ogles2_glUniform2f(struct OGLES2Context *ctx, GLint location, GLfloat v0, GLfloat v1)
{
    GLfloat v[2];
    v[0] = v0; v[1] = v1;
    set_uniform(ctx, location, 1, K_FLOAT, 2, 1, v, 0);
}

void ogles2_glUniform3f(struct OGLES2Context *ctx, GLint location, GLfloat v0, GLfloat v1,
                        GLfloat v2)
{
    GLfloat v[3];
    v[0] = v0; v[1] = v1; v[2] = v2;
    set_uniform(ctx, location, 1, K_FLOAT, 3, 1, v, 0);
}

void ogles2_glUniform4f(struct OGLES2Context *ctx, GLint location, GLfloat v0, GLfloat v1,
                        GLfloat v2, GLfloat v3)
{
    GLfloat v[4];
    v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3;
    set_uniform(ctx, location, 1, K_FLOAT, 4, 1, v, 0);
}

void ogles2_glUniform1i(struct OGLES2Context *ctx, GLint location, GLint v0)
{
    set_uniform(ctx, location, 1, K_INT, 1, 1, 0, &v0);
}

void ogles2_glUniform2i(struct OGLES2Context *ctx, GLint location, GLint v0, GLint v1)
{
    GLint v[2];
    v[0] = v0; v[1] = v1;
    set_uniform(ctx, location, 1, K_INT, 2, 1, 0, v);
}

void ogles2_glUniform3i(struct OGLES2Context *ctx, GLint location, GLint v0, GLint v1, GLint v2)
{
    GLint v[3];
    v[0] = v0; v[1] = v1; v[2] = v2;
    set_uniform(ctx, location, 1, K_INT, 3, 1, 0, v);
}

void ogles2_glUniform4i(struct OGLES2Context *ctx, GLint location, GLint v0, GLint v1, GLint v2,
                        GLint v3)
{
    GLint v[4];
    v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3;
    set_uniform(ctx, location, 1, K_INT, 4, 1, 0, v);
}

/* glUniformMatrix*: GLES 2.0 refuses `transpose` (GL_INVALID_VALUE); ES 3.0
 * takes it, the matrices then given row by row. `cols` x `rows` values each. */
static void set_matrix(struct OGLES2Context *ctx, GLint location, GLsizei count, GLboolean transpose,
                       const GLfloat *value, int cols, int rows)
{
    GLfloat *t;
    GLsizei i;
    int c, r;
    if (!ogles2_context_valid(ctx))
        return;
    if (transpose != GL_FALSE && !ogles2_es3(ctx)) { /* GLES 2.0: must be FALSE */
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (transpose == GL_FALSE || count <= 0 || !value) {
        set_uniform(ctx, location, count, K_FLOAT, rows, cols, value, 0);
        return;
    }
    if (!(t = AllocVec((ULONG)count * cols * rows * sizeof(GLfloat), MEMF_ANY))) {
        ogles2_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    for (i = 0; i < count; i++)
        for (c = 0; c < cols; c++)
            for (r = 0; r < rows; r++)
                t[(i * cols + c) * rows + r] = value[(i * rows + r) * cols + c];
    set_uniform(ctx, location, count, K_FLOAT, rows, cols, t, 0);
    FreeVec(t);
}

void ogles2_glUniformMatrix2fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{
    set_matrix(ctx, location, count, transpose, value, 2, 2);
}

void ogles2_glUniformMatrix3fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{
    set_matrix(ctx, location, count, transpose, value, 3, 3);
}

void ogles2_glUniformMatrix4fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{
    set_matrix(ctx, location, count, transpose, value, 4, 4);
}

/* ES 3.0's non-square matrices: matCxR, C columns of R. (Written out: the
 * generator finds implemented entries by their definitions.) */
void ogles2_glUniformMatrix2x3fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value)
{
    if (ogles2_es3_entry(ctx))
        set_matrix(ctx, location, count, transpose, value, 2, 3);
}

void ogles2_glUniformMatrix3x2fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value)
{
    if (ogles2_es3_entry(ctx))
        set_matrix(ctx, location, count, transpose, value, 3, 2);
}

void ogles2_glUniformMatrix2x4fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value)
{
    if (ogles2_es3_entry(ctx))
        set_matrix(ctx, location, count, transpose, value, 2, 4);
}

void ogles2_glUniformMatrix4x2fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value)
{
    if (ogles2_es3_entry(ctx))
        set_matrix(ctx, location, count, transpose, value, 4, 2);
}

void ogles2_glUniformMatrix3x4fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value)
{
    if (ogles2_es3_entry(ctx))
        set_matrix(ctx, location, count, transpose, value, 3, 4);
}

void ogles2_glUniformMatrix4x3fv(struct OGLES2Context *ctx, GLint location, GLsizei count,
                                 GLboolean transpose, const GLfloat *value)
{
    if (ogles2_es3_entry(ctx))
        set_matrix(ctx, location, count, transpose, value, 4, 3);
}

/* ES 3.0's unsigned uniforms. */
void ogles2_glUniform1uiv(struct OGLES2Context *ctx, GLint location, GLsizei count, const GLuint *value)
{
    if (ogles2_es3_entry(ctx))
        set_uniform(ctx, location, count, K_UINT, 1, 1, 0, (const GLint *)value);
}

void ogles2_glUniform2uiv(struct OGLES2Context *ctx, GLint location, GLsizei count, const GLuint *value)
{
    if (ogles2_es3_entry(ctx))
        set_uniform(ctx, location, count, K_UINT, 2, 1, 0, (const GLint *)value);
}

void ogles2_glUniform3uiv(struct OGLES2Context *ctx, GLint location, GLsizei count, const GLuint *value)
{
    if (ogles2_es3_entry(ctx))
        set_uniform(ctx, location, count, K_UINT, 3, 1, 0, (const GLint *)value);
}

void ogles2_glUniform4uiv(struct OGLES2Context *ctx, GLint location, GLsizei count, const GLuint *value)
{
    if (ogles2_es3_entry(ctx))
        set_uniform(ctx, location, count, K_UINT, 4, 1, 0, (const GLint *)value);
}

void ogles2_glUniform1ui(struct OGLES2Context *ctx, GLint location, GLuint v0)
{
    ogles2_glUniform1uiv(ctx, location, 1, &v0);
}

void ogles2_glUniform2ui(struct OGLES2Context *ctx, GLint location, GLuint v0, GLuint v1)
{
    GLuint v[2];
    v[0] = v0; v[1] = v1;
    ogles2_glUniform2uiv(ctx, location, 1, v);
}

void ogles2_glUniform3ui(struct OGLES2Context *ctx, GLint location, GLuint v0, GLuint v1, GLuint v2)
{
    GLuint v[3];
    v[0] = v0; v[1] = v1; v[2] = v2;
    ogles2_glUniform3uiv(ctx, location, 1, v);
}

void ogles2_glUniform4ui(struct OGLES2Context *ctx, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3)
{
    GLuint v[4];
    v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3;
    ogles2_glUniform4uiv(ctx, location, 1, v);
}

/* glGetUniformfv / iv / uiv: the value as the first storage place holds it
 * (uns: iv receives unsigned values). */
static void get_uniform(struct OGLES2Context *ctx, GLuint program, GLint location, GLfloat *fv,
                        GLint *iv, BOOL uns)
{
    struct OGLES2Program *p;
    const struct OGLES2ImageUniform *u;
    const struct OGLES2Place *place;
    int kind, comps, cols, c, k;
    if (!ogles2_context_valid(ctx) || !(p = linked(ctx, program)))
        return;
    if (location < 0 || location >= p->nlocations || p->loc_uniform[location] >= OGLES2_LOC_INACTIVE) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    u = &p->image->uniforms[p->loc_uniform[location]];
    if (!type_shape(u->type, &kind, &comps, &cols))
        return;
    if (kind == K_SAMPLER || kind == K_IMAGE) {
        if (fv) *fv = (GLfloat)p->sampler_unit[location];
        if (iv) *iv = p->sampler_unit[location];
        return;
    }
    if (!u->nplaces)
        return;
    place = &u->places[0];
    for (c = 0; c < cols; c++)
        for (k = 0; k < comps; k++) {
            ULONG at = place->offset + (ULONG)p->loc_element[location] * place->elem_stride +
                       (ULONG)c * place->vec_stride + k;
            ULONG w = at < p->image->cbuf_words[place->stage] ? p->cbuf[place->stage][at] : 0;
            union { ULONG u; GLfloat f; } cv;
            GLfloat f;
            GLint n;
            cv.u = w;
            if (kind == K_FLOAT || place->int_as_float) {
                f = cv.f;
                n = uns ? (GLint)(GLuint)f : (GLint)f;
            } else {
                n = (GLint)w;
                f = kind == K_UINT ? (GLfloat)w : (GLfloat)n;
            }
            if (kind == K_BOOL) {
                n = w != 0;
                f = (GLfloat)n;
            }
            if (fv) fv[c * comps + k] = f;
            if (iv) iv[c * comps + k] = n;
        }
}

void ogles2_glGetUniformfv(struct OGLES2Context *ctx, GLuint program, GLint location,
                           GLfloat *params)
{
    if (params)
        get_uniform(ctx, program, location, params, 0, FALSE);
}

void ogles2_glGetUniformiv(struct OGLES2Context *ctx, GLuint program, GLint location,
                           GLint *params)
{
    if (params)
        get_uniform(ctx, program, location, 0, params, FALSE);
}

void ogles2_glGetUniformuiv(struct OGLES2Context *ctx, GLuint program, GLint location, GLuint *params)
{
    if (ogles2_es3_entry(ctx) && params)
        get_uniform(ctx, program, location, 0, (GLint *)params, TRUE);
}

/* All precisions are fp32 and 32-bit ints on V3D (plan 7.8). */
void ogles2_glGetShaderPrecisionFormat(struct OGLES2Context *ctx, GLenum shadertype,
                                       GLenum precisiontype, GLint *range, GLint *precision)
{
    BOOL is_float;
    if (!ogles2_context_valid(ctx))
        return;
    if ((shadertype != GL_VERTEX_SHADER && shadertype != GL_FRAGMENT_SHADER) ||
        precisiontype < GL_LOW_FLOAT || precisiontype > GL_HIGH_INT) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    is_float = precisiontype <= GL_HIGH_FLOAT;
    if (range) {
        range[0] = is_float ? 127 : 31;
        range[1] = is_float ? 127 : 30;
    }
    if (precision)
        *precision = is_float ? 23 : 0;
}

/* No shader binary formats (GL_NUM_SHADER_BINARY_FORMATS is 0): programs
 * come from .v3dp files found by their sources' hash. */
void ogles2_glShaderBinary(struct OGLES2Context *ctx, GLsizei count, const GLuint *shaders,
                           GLenum binaryFormat, const void *binary, GLsizei length)
{
    (void)count;
    (void)shaders;
    (void)binaryFormat;
    (void)binary;
    (void)length;
    if (ogles2_context_valid(ctx))
        ogles2_error(ctx, GL_INVALID_ENUM);
}

/* ---- ES 3.1's program interface queries (ES 3.1 7.3.1; docs/GLES3_PLAN.md
 * stage D item 6), as Mesa's shader_query.cpp answers them. The resources
 * of each interface are the image's: GL_UNIFORM the active uniforms (the
 * same indices as glGetActiveUniform), GL_BUFFER_VARIABLE the storage
 * blocks' members in the image's order, the blocks in theirs, the inputs
 * and outputs the INTERFACE section's, transform feedback's varyings as
 * linked; no atomic counter buffers yet (the compiler refuses them). ---- */

/* A buffer variable GL_BUFFER_VARIABLE lists (ES 3.1 7.3.1.1, Mesa's
 * should_add_buffer_variable): a storage block member that is an array is
 * listed by its first element only (its block's name taken off first), an
 * array of structures' members by those of element 0. */
static BOOL listed_var(const struct OGLES2Image *im, const struct OGLES2ImageUniform *u)
{
    const char *n = u->name, *dot, *sq;
    if (!(u->flags & OGLES2_UNIFORM_BUFFER))
        return FALSE;
    if (u->block >= 0 && u->block < im->nsblocks) {
        const char *b = im->sblocks[u->block].name, *bs = strchr(b, '[');
        ULONG bl = bs ? (ULONG)(bs - b) : str_len(b);
        dot = strchr(n, '.');
        if ((!dot || (ULONG)(dot - n) == bl) && !strncmp(b, n, bl))
            n += bl + 1;
    }
    dot = strchr(n, '.');
    sq = strchr(n, '[');
    return !sq || (dot && dot < sq) || !strncmp(sq, "[0]", 3);
}

/* The interface's resources: their count; -1 for an interface that is none. */
static LONG resources(const struct OGLES2Image *im, GLenum iface)
{
    LONG n = 0;
    UWORD k;
    switch (iface) {
    case GL_UNIFORM:
        for (k = 0; im && k < im->nuniforms; k++)
            n += visible(&im->uniforms[k]);
        return n;
    case GL_BUFFER_VARIABLE:
        for (k = 0; im && k < im->nuniforms; k++)
            n += listed_var(im, &im->uniforms[k]);
        return n;
    case GL_UNIFORM_BLOCK:        return im ? im->nublocks : 0;
    case GL_SHADER_STORAGE_BLOCK: return im ? im->nsblocks : 0;
    case GL_PROGRAM_INPUT:
    case GL_PROGRAM_OUTPUT:
        for (k = 0; im && k < im->ninterface; k++)
            n += im->interface[k].iface == iface;
        return n;
    case GL_TRANSFORM_FEEDBACK_VARYING: return im && im->xfb ? im->xfb->nvaryings : 0;
    case GL_ATOMIC_COUNTER_BUFFER:      return im ? im->nabuffers : 0;
    default:                            return -1;
    }
}

/* Resource i of an interface (below its count): its name, whether with
 * "[0]" (an array's, Mesa's add_index_to_name), and what it is: the
 * image's uniform u (its index ui), block, or variable. */
struct resource
{
    const char *name;
    BOOL add0;
    UWORD ui;
    const struct OGLES2ImageUniform *u;
    const struct OGLES2ImageBlock *b;
    const struct OGLES2ImageVar *v;
    const struct OGLES2ImageXfbVar *x;
    const struct OGLES2ImageAtomic *a;
};

static BOOL ends_bracket(const char *s)
{
    ULONG n = str_len(s);
    return n && s[n - 1] == ']';
}

static void resource(const struct OGLES2Image *im, GLenum iface, ULONG i, struct resource *r)
{
    UWORD k;
    memset(r, 0, sizeof(*r));
    switch (iface) {
    case GL_UNIFORM:
    case GL_BUFFER_VARIABLE:
        for (k = 0; k < im->nuniforms; k++) {
            const struct OGLES2ImageUniform *u = &im->uniforms[k];
            if (!(iface == GL_UNIFORM ? visible(u) : listed_var(im, u)) || i-- != 0)
                continue;
            r->u = u;
            r->ui = k;
            r->name = u->name;
            /* an unsized array (an array stride, no elements) too: Mesa's
             * _mesa_program_resource_array_size */
            r->add0 = u->array || (iface == GL_BUFFER_VARIABLE && u->array_stride > 0);
            return;
        }
        return;
    case GL_UNIFORM_BLOCK:
        r->b = &im->ublocks[i];
        r->name = r->b->name;
        return;
    case GL_SHADER_STORAGE_BLOCK:
        r->b = &im->sblocks[i];
        r->name = r->b->name;
        return;
    case GL_PROGRAM_INPUT:
    case GL_PROGRAM_OUTPUT:
        for (k = 0; k < im->ninterface; k++)
            if (im->interface[k].iface == iface && i-- == 0) {
                r->v = &im->interface[k];
                r->name = r->v->name;
                r->add0 = r->v->array && !ends_bracket(r->name);
                return;
            }
        return;
    case GL_ATOMIC_COUNTER_BUFFER: /* no name */
        r->a = &im->abuffers[i];
        r->name = "";
        return;
    case GL_TRANSFORM_FEEDBACK_VARYING:
        r->x = &im->xfb->varyings[i];
        r->name = r->x->name; /* as glTransformFeedbackVaryings named it (Mesa's) */
        return;
    }
}

/* The program of an ES 3.1 interface query and its interface's resources
 * counted into *n; NULL after an error (GL_INVALID_ENUM: an interface that
 * is none). A program not linked has none. */
static struct OGLES2Program *iface_program(struct OGLES2Context *ctx, GLuint program, GLenum iface, LONG *n)
{
    struct OGLES2Program *p;
    if (!ogles2_es31_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return 0;
    if ((*n = resources(p->linked ? p->image : 0, iface)) < 0) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    return p;
}

/* The stages that reference a uniform, Mesa's stage bits (0 vertex, 4
 * fragment, 5 compute): its block's, else its constant buffers' and its
 * samplers' (slot 0 a compute program's compute stage). */
static ULONG uniform_stages(const struct OGLES2Image *im, const struct OGLES2ImageUniform *u)
{
    ULONG m = 0, first = im->compute ? 0x20 : 0x01;
    UBYTE k;
    if (u->block >= 0 && (u->flags & OGLES2_UNIFORM_ATOMIC))
        return im->abuffers[u->block].stages;
    if (u->block >= 0)
        return (u->flags & OGLES2_UNIFORM_BUFFER ? im->sblocks : im->ublocks)[u->block].stages;
    for (k = 0; k < u->nplaces; k++)
        m |= u->places[k].stage ? 0x10 : first;
    if (u->sampler[0] != ~0UL)
        m |= first;
    if (u->sampler[1] != ~0UL)
        m |= 0x10;
    return m;
}

/* Image uniform u's index among the buffer variables (GL_BUFFER_VARIABLE's). */
static GLint buffer_var_index(const struct OGLES2Image *im, UWORD u)
{
    UWORD k;
    GLint n = 0;
    for (k = 0; k < u; k++)
        if (listed_var(im, &im->uniforms[k]))
            n++;
    return n;
}

/* A storage block's members GL_BUFFER_VARIABLE lists: their count, their
 * indices into v (room of them). */
static GLint block_vars(const struct OGLES2Image *im, const struct OGLES2ImageBlock *b, GLint *v, GLint room)
{
    UWORD k;
    GLint n = 0;
    for (k = 0; k < b->nmembers; k++)
        if (b->members[k] < im->nuniforms && listed_var(im, &im->uniforms[b->members[k]])) {
            if (v && n < room)
                v[n] = buffer_var_index(im, b->members[k]);
            n++;
        }
    return n;
}

void ogles2_glGetProgramInterfaceiv(struct OGLES2Context *ctx, GLuint program, GLenum programInterface,
                                    GLenum pname, GLint *params)
{
    struct OGLES2Program *p;
    struct resource r;
    LONG n, i, m = 0;
    BOOL blocks = programInterface == GL_UNIFORM_BLOCK || programInterface == GL_SHADER_STORAGE_BLOCK ||
                  programInterface == GL_ATOMIC_COUNTER_BUFFER;
    if (!(p = iface_program(ctx, program, programInterface, &n)) || !params)
        return;
    switch (pname) {
    case GL_ACTIVE_RESOURCES:
        *params = n;
        return;
    case GL_MAX_NAME_LENGTH: /* with the terminating null; 0 without resources */
        if (programInterface == GL_ATOMIC_COUNTER_BUFFER) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        for (i = 0; i < n; i++) {
            resource(p->image, programInterface, (ULONG)i, &r);
            if ((LONG)str_len(r.name) + (r.add0 ? 4 : 1) > m)
                m = (LONG)str_len(r.name) + (r.add0 ? 4 : 1);
        }
        *params = m;
        return;
    case GL_MAX_NUM_ACTIVE_VARIABLES:
        if (!blocks) {
            ogles2_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        for (i = 0; i < n; i++) {
            resource(p->image, programInterface, (ULONG)i, &r);
            if (r.b) {
                GLint nv = programInterface == GL_UNIFORM_BLOCK ? r.b->nmembers : block_vars(p->image, r.b, 0, 0);
                if (nv > m)
                    m = nv;
            }
            if (r.a && r.a->ncounters > m)
                m = r.a->ncounters;
        }
        *params = m;
        return;
    default:
        ogles2_error(ctx, GL_INVALID_ENUM);
    }
}

/* "name", or an array's "name[0]": the resource's index; an element past
 * the first is none (Mesa's _mesa_program_resource_find_name). */
GLuint ogles2_glGetProgramResourceIndex(struct OGLES2Context *ctx, GLuint program, GLenum programInterface,
                                        const GLchar *name)
{
    struct OGLES2Program *p;
    struct resource r;
    LONG n, i;
    if (!(p = iface_program(ctx, program, programInterface, &n)))
        return GL_INVALID_INDEX;
    if (programInterface == GL_ATOMIC_COUNTER_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return GL_INVALID_INDEX;
    }
    for (i = 0; name && i < n; i++) {
        ULONG len;
        resource(p->image, programInterface, (ULONG)i, &r);
        len = str_len(r.name);
        if (str_eq(r.name, name) || (r.add0 && !strncmp(r.name, name, len) && str_eq(name + len, "[0]")))
            return (GLuint)i;
        /* an array of blocks: its first by the array's name (Mesa's
         * _mesa_program_resource_find_name) */
        if ((programInterface == GL_UNIFORM_BLOCK || programInterface == GL_SHADER_STORAGE_BLOCK) &&
            len == str_len(name) + 3 && !strncmp(r.name, name, len - 3) && str_eq(r.name + len - 3, "[0]"))
            return (GLuint)i;
    }
    return GL_INVALID_INDEX;
}

void ogles2_glGetProgramResourceName(struct OGLES2Context *ctx, GLuint program, GLenum programInterface,
                                     GLuint index, GLsizei bufSize, GLsizei *length, GLchar *name)
{
    struct OGLES2Program *p;
    struct resource r;
    LONG n;
    GLsizei k;
    if (!(p = iface_program(ctx, program, programInterface, &n)))
        return;
    if (programInterface == GL_ATOMIC_COUNTER_BUFFER) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (index > 0x7fffffffUL || (LONG)index >= n || bufSize < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    resource(p->image, programInterface, index, &r);
    k = copy_out(name, bufSize, r.name);
    if (r.add0 && name)
        k += copy_out(name + k, bufSize - k, "[0]");
    if (length)
        *length = k;
}

/* A property of a resource: its values into v (room for `room`), how
 * many; -1 for a property its interface has not (GL_INVALID_OPERATION),
 * -2 for no property at all (GL_INVALID_ENUM). */
static LONG property(struct OGLES2Program *p, GLenum iface, const struct resource *r, GLenum prop, GLint *v,
                     LONG room)
{
    const struct OGLES2Image *im = p->image;
    const struct OGLES2ImageUniform *u = r->u;
    BOOL uni = iface == GL_UNIFORM || iface == GL_BUFFER_VARIABLE;
    BOOL var = uni || iface == GL_PROGRAM_INPUT || iface == GL_PROGRAM_OUTPUT;
    BOOL blk = iface == GL_UNIFORM_BLOCK || iface == GL_SHADER_STORAGE_BLOCK;
    ULONG stages;
    LONG k;
    switch (prop) {
    case GL_NAME_LENGTH:
        if (iface == GL_ATOMIC_COUNTER_BUFFER)
            return -1;
        v[0] = (GLint)str_len(r->name) + (r->add0 ? 4 : 1);
        return 1;
    case GL_TYPE:
    case GL_ARRAY_SIZE:
        if (!var && iface != GL_TRANSFORM_FEEDBACK_VARYING)
            return -1;
        if (prop == GL_TYPE)
            v[0] = (GLint)(u ? u->type : r->v ? r->v->type : r->x->type);
        else if (u && iface == GL_BUFFER_VARIABLE && u->array_stride > 0)
            v[0] = u->array; /* an unsized array's: 0 */
        else
            v[0] = u ? (u->array ? u->array : 1) : r->v ? (r->v->array ? r->v->array : 1) : r->x->size;
        return 1;
    case GL_OFFSET:
    case GL_ARRAY_STRIDE:
    case GL_MATRIX_STRIDE:
    case GL_IS_ROW_MAJOR:
    case GL_BLOCK_INDEX:
        if (!uni)
            return -1;
        /* the default block's: -1 offsets and strides, as Mesa's */
        /* an atomic counter: in no block, its offset and stride in its buffer */
        v[0] = prop == GL_BLOCK_INDEX   ? (u->flags & OGLES2_UNIFORM_ATOMIC ? -1 : u->block)
             : u->block < 0             ? (prop == GL_IS_ROW_MAJOR ? 0 : -1)
             : prop == GL_OFFSET        ? u->offset
             : prop == GL_ARRAY_STRIDE  ? u->array_stride
             : prop == GL_MATRIX_STRIDE ? u->matrix_stride
                                        : (u->row_major ? 1 : 0);
        return 1;
    case GL_ATOMIC_COUNTER_BUFFER_INDEX:
        if (iface != GL_UNIFORM)
            return -1;
        v[0] = u->flags & OGLES2_UNIFORM_ATOMIC ? u->block : -1;
        return 1;
    case GL_TOP_LEVEL_ARRAY_SIZE:
    case GL_TOP_LEVEL_ARRAY_STRIDE:
        if (iface != GL_BUFFER_VARIABLE)
            return -1;
        {
            const struct OGLES2ImageBlock *b = &im->sblocks[u->block];
            v[0] = 0;
            for (k = 0; k < b->nmembers; k++)
                if (b->members[k] == r->ui)
                    v[0] = b->top_level[2 * k + (prop == GL_TOP_LEVEL_ARRAY_STRIDE)];
        }
        return 1;
    case GL_BUFFER_BINDING:
    case GL_BUFFER_DATA_SIZE:
    case GL_NUM_ACTIVE_VARIABLES:
        if (!blk && iface != GL_ATOMIC_COUNTER_BUFFER)
            return -1;
        if (r->a) { /* an atomic counter buffer */
            v[0] = (GLint)(prop == GL_BUFFER_DATA_SIZE      ? r->a->size
                         : prop == GL_NUM_ACTIVE_VARIABLES ? r->a->ncounters
                                                           : r->a->binding);
            return 1;
        }
        v[0] = prop == GL_BUFFER_DATA_SIZE      ? (GLint)r->b->size
             : prop == GL_NUM_ACTIVE_VARIABLES ? (iface == GL_UNIFORM_BLOCK ? r->b->nmembers : block_vars(im, r->b, 0, 0))
             : iface == GL_UNIFORM_BLOCK       ? (GLint)p->block_binding[r->b - im->ublocks]
                                               : (GLint)r->b->binding;
        return 1;
    case GL_ACTIVE_VARIABLES:
        if (!blk && iface != GL_ATOMIC_COUNTER_BUFFER)
            return -1;
        if (r->a) { /* its counters, as active uniforms */
            for (k = 0; k < r->a->ncounters && k < room; k++)
                v[k] = active_index(im, r->a->counters[k]);
            return k;
        }
        if (iface != GL_UNIFORM_BLOCK) {
            GLint n = block_vars(im, r->b, v, room);
            return n < room ? n : room;
        }
        for (k = 0; k < r->b->nmembers && k < room; k++)
            v[k] = active_index(im, r->b->members[k]);
        return k;
    case GL_REFERENCED_BY_VERTEX_SHADER:
    case GL_REFERENCED_BY_FRAGMENT_SHADER:
    case GL_REFERENCED_BY_COMPUTE_SHADER:
        if (iface == GL_TRANSFORM_FEEDBACK_VARYING)
            return -1;
        stages = u ? uniform_stages(im, u) : r->b ? r->b->stages : r->a ? r->a->stages : r->v ? r->v->stages : 0;
        v[0] = (stages & (prop == GL_REFERENCED_BY_VERTEX_SHADER     ? 0x01
                          : prop == GL_REFERENCED_BY_FRAGMENT_SHADER ? 0x10
                                                                     : 0x20)) != 0;
        return 1;
    case GL_LOCATION:
        if (iface != GL_UNIFORM && iface != GL_PROGRAM_INPUT && iface != GL_PROGRAM_OUTPUT)
            return -1;
        if (u) {
            v[0] = u->block >= 0 || p->uniform_loc[r->ui] == 0xFFFF ? -1 : (GLint)p->uniform_loc[r->ui];
        } else {
            v[0] = r->v->location;
            /* a vertex input the runtime numbered (no layout(location)) */
            if (iface == GL_PROGRAM_INPUT && v[0] >= 0)
                for (k = 0; k < im->nattribs; k++)
                    if (str_eq(im->attribs[k].name, r->v->name))
                        v[0] = p->attrib_index[k] == 0xff ? -1 : p->attrib_index[k];
        }
        return 1;
    default:
        return -2;
    }
}

void ogles2_glGetProgramResourceiv(struct OGLES2Context *ctx, GLuint program, GLenum programInterface,
                                   GLuint index, GLsizei propCount, const GLenum *props, GLsizei bufSize,
                                   GLsizei *length, GLint *params)
{
    struct OGLES2Program *p;
    struct resource r;
    GLint v[1];
    LONG n, got;
    GLsizei k, out = 0;
    if (!(p = iface_program(ctx, program, programInterface, &n)))
        return;
    if (index > 0x7fffffffUL || (LONG)index >= n || propCount <= 0 || bufSize < 0 || !props) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    resource(p->image, programInterface, index, &r);
    /* every property checked first: a bad one is an error even past bufSize */
    for (k = 0; k < propCount; k++)
        if ((got = property(p, programInterface, &r, props[k], v, 1)) < 0) {
            ogles2_error(ctx, got == -2 ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
            return;
        }
    for (k = 0; k < propCount && out < bufSize && params; k++)
        out += (GLsizei)property(p, programInterface, &r, props[k], params + out, bufSize - out);
    if (length)
        *length = out;
}

GLint ogles2_glGetProgramResourceLocation(struct OGLES2Context *ctx, GLuint program, GLenum programInterface,
                                          const GLchar *name)
{
    struct OGLES2Program *p;
    LONG n;
    GLint loc;
    if (!(p = iface_program(ctx, program, programInterface, &n)))
        return -1;
    if (programInterface != GL_UNIFORM && programInterface != GL_PROGRAM_INPUT &&
        programInterface != GL_PROGRAM_OUTPUT) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return -1;
    }
    if (!p->linked) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return -1;
    }
    if (!name || !strncmp(name, "gl_", 3))
        return -1;
    /* a separable program's inputs or outputs between stages (ES 3.1: a
     * fragment program's inputs, a vertex program's outputs): the
     * interface's, -1 without a location of their own */
    if ((programInterface == GL_PROGRAM_INPUT && !(p->image->stages & 0x01)) ||
        (programInterface == GL_PROGRAM_OUTPUT && !(p->image->stages & 0x10))) {
        struct resource r;
        LONG i;
        for (i = 0; i < n; i++) {
            ULONG len;
            resource(p->image, programInterface, (ULONG)i, &r);
            len = str_len(r.name);
            if (str_eq(r.name, name) || (r.add0 && !strncmp(r.name, name, len) && str_eq(name + len, "[0]")))
                return (r.v->flags & 1) ? (GLint)r.v->location : -1;
        }
        return -1;
    }
    if (programInterface == GL_PROGRAM_OUTPUT)
        return ogles2_glGetFragDataLocation(ctx, program, name);
    if (programInterface == GL_PROGRAM_INPUT)
        return ogles2_glGetAttribLocation(ctx, program, name);
    loc = ogles2_glGetUniformLocation(ctx, program, name);
    /* a uniform block's member has none */
    return loc >= 0 && p->image->uniforms[p->loc_uniform[loc]].block >= 0 ? -1 : loc;
}

/* ---- ES 3.1's glProgramUniform* (docs/GLES3_PLAN.md stage D item 5): a
 * program's uniform set whether it is in use or not: glUniform*'s, the
 * program made the target for the one call. Its errors first: no such
 * object GL_INVALID_VALUE, a shader or a program not linked
 * GL_INVALID_OPERATION. (Written out: the generator finds implemented
 * entries by their definitions.) ---- */

static struct OGLES2Program *uniform_target(struct OGLES2Context *ctx, GLuint program)
{
    struct OGLES2Program *p, *saved;
    if (!ogles2_es31_entry(ctx) || !(p = lookup(ctx, program, OGLES2_OBJ_PROGRAM)))
        return 0;
    if (!p->linked) {
        ogles2_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    saved = ctx->program;
    ctx->program = p;
    return saved ? saved : (struct OGLES2Program *)ctx; /* ctx: none was in use */
}

static void uniform_target_end(struct OGLES2Context *ctx, struct OGLES2Program *saved)
{
    ctx->program = saved == (struct OGLES2Program *)ctx ? 0 : saved;
}


void ogles2_glProgramUniform1f(struct OGLES2Context *ctx, GLuint program, GLint location, GLfloat v0)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform1f(ctx, location, v0);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform2f(struct OGLES2Context *ctx, GLuint program, GLint location, GLfloat v0, GLfloat v1)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform2f(ctx, location, v0, v1);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform3f(struct OGLES2Context *ctx, GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform3f(ctx, location, v0, v1, v2);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform4f(struct OGLES2Context *ctx, GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform4f(ctx, location, v0, v1, v2, v3);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform1i(struct OGLES2Context *ctx, GLuint program, GLint location, GLint v0)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform1i(ctx, location, v0);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform2i(struct OGLES2Context *ctx, GLuint program, GLint location, GLint v0, GLint v1)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform2i(ctx, location, v0, v1);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform3i(struct OGLES2Context *ctx, GLuint program, GLint location, GLint v0, GLint v1, GLint v2)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform3i(ctx, location, v0, v1, v2);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform4i(struct OGLES2Context *ctx, GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform4i(ctx, location, v0, v1, v2, v3);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform1ui(struct OGLES2Context *ctx, GLuint program, GLint location, GLuint v0)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform1ui(ctx, location, v0);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform2ui(struct OGLES2Context *ctx, GLuint program, GLint location, GLuint v0, GLuint v1)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform2ui(ctx, location, v0, v1);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform3ui(struct OGLES2Context *ctx, GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform3ui(ctx, location, v0, v1, v2);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform4ui(struct OGLES2Context *ctx, GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform4ui(ctx, location, v0, v1, v2, v3);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform1fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform1fv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform2fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform2fv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform3fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform3fv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform4fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform4fv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform1iv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform1iv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform2iv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform2iv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform3iv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform3iv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform4iv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform4iv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform1uiv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLuint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform1uiv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform2uiv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLuint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform2uiv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform3uiv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLuint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform3uiv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniform4uiv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, const GLuint *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniform4uiv(ctx, location, count, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix2fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix2fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix3fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix3fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix4fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix4fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix2x3fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix2x3fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix3x2fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix3x2fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix2x4fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix2x4fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix4x2fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix4x2fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix3x4fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix3x4fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}

void ogles2_glProgramUniformMatrix4x3fv(struct OGLES2Context *ctx, GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    struct OGLES2Program *saved = uniform_target(ctx, program);
    if (!saved)
        return;
    ogles2_glUniformMatrix4x3fv(ctx, location, count, transpose, value);
    uniform_target_end(ctx, saved);
}
