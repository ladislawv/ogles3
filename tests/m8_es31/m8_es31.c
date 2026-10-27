/*
 * GLES3 stage D test (docs/GLES3_PLAN.md §7): OpenGL ES 3.1 contexts.
 *   usage: m8_es31 [NOGPU] [ES31]   (PASS/FAIL lines, returns 0 or 10)
 *          m8_es31 LIMITS [NOGPU]   (an ES 3.1 context's limits, `v3dglslc -L`'s format)
 * State-only contexts (no surface), so it runs under vamos too (NOGPU).
 * ES31: the library is a test build (make ES31=1) and must give ES 3.1
 * contexts; without it the tag's 31 must be refused.
 *
 * Covered so far: the version strings and numbers, an ES 3.0 request given
 * 3.1; the state dEQP's glu::resetState sets in an ES 3.1 context: the
 * indirect, storage and atomic counter buffer targets and indexed bindings
 * (their alignments), GL_SAMPLE_MASK and glSampleMaski,
 * GL_DEPTH_STENCIL_TEXTURE_MODE, the GL_TEXTURE_2D_MULTISAMPLE target (no
 * sampler state, base level 0), glBindProgramPipeline(0), the image units
 * (glBindImageTexture's errors, the GL_IMAGE_BINDING_* queries); the same
 * refused in an ES 3.0 context.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/ogles2.h>
#include <GLES3/gl31.h>

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static int prefix(const GLubyte *s, const char *p)
{
    return s && !strncmp((const char *)s, p, strlen(p));
}

static void *context(ULONG version, ULONG *err)
{
    *err = 99;
    return aglCreateContextTags2(err, OGLES2_CCT_PISTORM_STATE_ONLY, TRUE, OGLES2_CCT_PISTORM_ES_VERSION, version,
                                 TAG_DONE);
}

static GLint get_int(GLenum pname)
{
    GLint v = -12345;
    glGetIntegerv(pname, &v);
    return v;
}

static GLint get_inti(GLenum pname, GLuint index)
{
    GLint v = -12345;
    glGetIntegeri_v(pname, index, &v);
    return v;
}

/* The limits an ES 3.1 context answers, as `v3dglslc -L` prints them (ES
 * 3.0's are m8_es3's): tests/m8_es31/check_limits.sh compares the two. */
static void print_limits(void)
{
#define L(e) { e, #e }
    static const struct { GLenum e; const char *name; } limits[] = {
        L(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS), L(GL_MAX_COMPUTE_UNIFORM_BLOCKS),
        L(GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS), L(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE),
        L(GL_MAX_COMPUTE_UNIFORM_COMPONENTS), L(GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS),
        L(GL_MAX_COMPUTE_ATOMIC_COUNTERS), L(GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS),
        L(GL_MAX_COMPUTE_IMAGE_UNIFORMS), L(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS), L(GL_MAX_UNIFORM_LOCATIONS),
        L(GL_MAX_FRAMEBUFFER_WIDTH), L(GL_MAX_FRAMEBUFFER_HEIGHT), L(GL_MAX_FRAMEBUFFER_SAMPLES),
        L(GL_MAX_VERTEX_ATOMIC_COUNTER_BUFFERS), L(GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS),
        L(GL_MAX_COMBINED_ATOMIC_COUNTER_BUFFERS), L(GL_MAX_VERTEX_ATOMIC_COUNTERS),
        L(GL_MAX_FRAGMENT_ATOMIC_COUNTERS), L(GL_MAX_COMBINED_ATOMIC_COUNTERS),
        L(GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE), L(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS),
        L(GL_MAX_VERTEX_IMAGE_UNIFORMS), L(GL_MAX_FRAGMENT_IMAGE_UNIFORMS), L(GL_MAX_COMBINED_IMAGE_UNIFORMS),
        L(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS), L(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS),
        L(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS), L(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS),
        L(GL_MAX_SHADER_STORAGE_BLOCK_SIZE), L(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT), L(GL_MAX_IMAGE_UNITS),
        L(GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES), L(GL_MIN_PROGRAM_TEXTURE_GATHER_OFFSET),
        L(GL_MAX_PROGRAM_TEXTURE_GATHER_OFFSET), L(GL_MAX_SAMPLE_MASK_WORDS), L(GL_MAX_COLOR_TEXTURE_SAMPLES),
        L(GL_MAX_DEPTH_TEXTURE_SAMPLES), L(GL_MAX_INTEGER_SAMPLES), L(GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET),
        L(GL_MAX_VERTEX_ATTRIB_BINDINGS), L(GL_MAX_VERTEX_ATTRIB_STRIDE),
    };
    static const struct { GLenum e; const char *name; } indexed[] = {
        L(GL_MAX_COMPUTE_WORK_GROUP_COUNT), L(GL_MAX_COMPUTE_WORK_GROUP_SIZE),
    };
#undef L
    unsigned i, k;
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        GLint64 v = 0;
        glGetInteger64v(limits[i].e, &v);
        printf("%s %ld\n", limits[i].name, (long)v);
    }
    for (i = 0; i < sizeof(indexed) / sizeof(indexed[0]); i++) {
        printf("%s", indexed[i].name);
        for (k = 0; k < 3; k++)
            printf(" %ld", (long)get_inti(indexed[i].e, k));
        printf("\n");
    }
    if (glGetError() != GL_NO_ERROR)
        printf("ERROR: a limit query failed\n");
}

/* The buffer targets and indexed bindings ES 3.1 adds. */
static void buffers(void)
{
    static const GLenum targets[4] = { GL_DRAW_INDIRECT_BUFFER, GL_DISPATCH_INDIRECT_BUFFER,
                                       GL_SHADER_STORAGE_BUFFER, GL_ATOMIC_COUNTER_BUFFER };
    static const GLenum bindings[4] = { GL_DRAW_INDIRECT_BUFFER_BINDING, GL_DISPATCH_INDIRECT_BUFFER_BINDING,
                                        GL_SHADER_STORAGE_BUFFER_BINDING, GL_ATOMIC_COUNTER_BUFFER_BINDING };
    GLuint b[2];
    GLint64 v64 = -1;
    int k, ok = 1;
    glGenBuffers(2, b);
    for (k = 0; k < 4; k++) {
        glBindBuffer(targets[k], b[0]);
        ok = ok && get_int(bindings[k]) == (GLint)b[0];
        glBindBuffer(targets[k], 0);
    }
    check(ok && glGetError() == GL_NO_ERROR, "buffers: the four targets bound and queried");
    /* no glBufferData: it reads the Pi's timer, which vamos lacks (a
     * range is checked against the buffer when it is used, not here) */
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 31, b[0], 4, 8);
    glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 31, &v64);
    check(glGetError() == GL_NO_ERROR && get_inti(GL_SHADER_STORAGE_BUFFER_BINDING, 31) == (GLint)b[0] &&
              get_inti(GL_SHADER_STORAGE_BUFFER_START, 31) == 4 && v64 == 8 &&
              get_int(GL_SHADER_STORAGE_BUFFER_BINDING) == (GLint)b[0],
          "buffers: a storage buffer range at binding 31, the general binding too");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 32, b[0]);
    check(glGetError() == GL_INVALID_VALUE, "buffers: storage binding 32 is GL_INVALID_VALUE");
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, b[0], 2, 8);
    check(glGetError() == GL_INVALID_VALUE, "buffers: a storage offset of 2 is GL_INVALID_VALUE");
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 15, b[1]);
    check(glGetError() == GL_NO_ERROR && get_inti(GL_ATOMIC_COUNTER_BUFFER_BINDING, 15) == (GLint)b[1] &&
              get_inti(GL_ATOMIC_COUNTER_BUFFER_SIZE, 15) == 0,
          "buffers: an atomic counter buffer at binding 15");
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 16, b[1]);
    check(glGetError() == GL_INVALID_VALUE, "buffers: atomic counter binding 16 is GL_INVALID_VALUE");
    glDeleteBuffers(2, b);
    check(get_inti(GL_SHADER_STORAGE_BUFFER_BINDING, 31) == 0 && get_inti(GL_ATOMIC_COUNTER_BUFFER_BINDING, 15) == 0 &&
              get_int(GL_SHADER_STORAGE_BUFFER_BINDING) == 0 && glGetError() == GL_NO_ERROR,
          "buffers: deleted, unbound from every binding");
}

static GLint attrib_int(GLuint index, GLenum pname)
{
    GLint v = -12345;
    glGetVertexAttribiv(index, pname, &v);
    return v;
}

/* Vertex attribute binding (stage D item 5): formats apart from bindings,
 * glVertexAttribPointer and glVertexAttribDivisor through them. */
static void attrib_binding(void)
{
    static const GLfloat data[4] = { 0, 0, 0, 0 };
    GLuint b, vao;
    GLint64 off = -1;
    glBindVertexBuffer(0, 0, 0, 16);
    glVertexAttribBinding(0, 0);
    check(glGetError() == GL_INVALID_OPERATION, "binding: refused with vertex array object 0 (GL_INVALID_OPERATION)");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindVertexBuffer(0, 1234, 0, 16);
    check(glGetError() == GL_INVALID_OPERATION, "binding: a buffer name glGenBuffers did not give");
    check(attrib_int(7, GL_VERTEX_ATTRIB_BINDING) == 7 && attrib_int(7, GL_VERTEX_ATTRIB_RELATIVE_OFFSET) == 0 &&
              get_inti(GL_VERTEX_BINDING_STRIDE, 7) == 16 && get_inti(GL_VERTEX_BINDING_DIVISOR, 7) == 0 &&
              get_inti(GL_VERTEX_BINDING_BUFFER, 7) == 0 && glGetError() == GL_NO_ERROR,
          "binding: attribute 7 on binding 7, stride 16 (the initial state)");
    glGenBuffers(1, &b);
    glBindVertexBuffer(3, b, 8, 12);
    glGetInteger64i_v(GL_VERTEX_BINDING_OFFSET, 3, &off);
    check(glGetError() == GL_NO_ERROR && get_inti(GL_VERTEX_BINDING_BUFFER, 3) == (GLint)b && off == 8 &&
              get_inti(GL_VERTEX_BINDING_STRIDE, 3) == 12,
          "binding: a buffer at offset 8, stride 12, on binding 3");
    glVertexAttribBinding(0, 3);
    glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, 4);
    check(glGetError() == GL_NO_ERROR && attrib_int(0, GL_VERTEX_ATTRIB_BINDING) == 3 &&
              attrib_int(0, GL_VERTEX_ATTRIB_RELATIVE_OFFSET) == 4 && attrib_int(0, GL_VERTEX_ATTRIB_ARRAY_SIZE) == 3 &&
              attrib_int(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) == (GLint)b,
          "binding: attribute 0 reads binding 3 as 3 floats at relative offset 4");
    glVertexAttribIFormat(1, 2, GL_UNSIGNED_SHORT, 0);
    check(glGetError() == GL_NO_ERROR && attrib_int(1, GL_VERTEX_ATTRIB_ARRAY_INTEGER) == GL_TRUE,
          "binding: an integer format");
    glBindVertexBuffer(16, b, 0, 4);
    check(glGetError() == GL_INVALID_VALUE, "binding: binding 16 is GL_INVALID_VALUE");
    glBindVertexBuffer(0, b, 0, 2049);
    check(glGetError() == GL_INVALID_VALUE, "binding: a stride of 2049 is GL_INVALID_VALUE");
    glVertexAttribFormat(0, 4, GL_FLOAT, GL_FALSE, 2048);
    check(glGetError() == GL_INVALID_VALUE, "binding: a relative offset of 2048 is GL_INVALID_VALUE");
    glVertexAttribIFormat(0, 4, GL_FLOAT, 0);
    check(glGetError() == GL_INVALID_ENUM, "binding: an integer format of floats is GL_INVALID_ENUM");
    glVertexAttribBinding(0, 16);
    check(glGetError() == GL_INVALID_VALUE, "binding: attribute 0 on binding 16 is GL_INVALID_VALUE");
    glVertexAttribDivisor(2, 3);
    check(glGetError() == GL_NO_ERROR && get_inti(GL_VERTEX_BINDING_DIVISOR, 2) == 3 &&
              attrib_int(2, GL_VERTEX_ATTRIB_BINDING) == 2 && attrib_int(2, GL_VERTEX_ATTRIB_ARRAY_DIVISOR) == 3,
          "binding: glVertexAttribDivisor sets binding 2's divisor");
    glVertexAttribBinding(5, 3);
    glBindBuffer(GL_ARRAY_BUFFER, b); /* a vertex array object: no client arrays */
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    check(glGetError() == GL_NO_ERROR && attrib_int(5, GL_VERTEX_ATTRIB_BINDING) == 5 &&
              attrib_int(5, GL_VERTEX_ATTRIB_ARRAY_STRIDE) == 0 && get_inti(GL_VERTEX_BINDING_STRIDE, 5) == 16 &&
              get_inti(GL_VERTEX_BINDING_BUFFER, 3) == (GLint)b,
          "binding: glVertexAttribPointer puts attribute 5 on binding 5, stride 0 as 16 there");
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, 2049, data);
    check(glGetError() == GL_INVALID_VALUE, "binding: glVertexAttribPointer's stride of 2049 is GL_INVALID_VALUE");
    glDeleteBuffers(1, &b);
    check(get_inti(GL_VERTEX_BINDING_BUFFER, 3) == 0 && attrib_int(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) == 0 &&
              glGetError() == GL_NO_ERROR,
          "binding: the buffer deleted, binding 3 and attribute 0 without it");
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
}

/* The sample mask, the textures' new state, pipelines, image units (those
 * of a texture with storage with the GPU only). */
static void others(int gpu)
{
    GLuint t[2];
    GLint v = -1;
    GLboolean bv = GL_TRUE;
    check(!glIsEnabled(GL_SAMPLE_MASK) && get_inti(GL_SAMPLE_MASK_VALUE, 0) == -1, "sample mask: off, all ones");
    glEnable(GL_SAMPLE_MASK);
    glSampleMaski(0, 0x5);
    check(glIsEnabled(GL_SAMPLE_MASK) && get_inti(GL_SAMPLE_MASK_VALUE, 0) == 5 && glGetError() == GL_NO_ERROR,
          "sample mask: enabled, word 0 set");
    glSampleMaski(1, 0);
    check(glGetError() == GL_INVALID_VALUE, "sample mask: word 1 is GL_INVALID_VALUE");
    glSampleMaski(0, ~0u);
    glDisable(GL_SAMPLE_MASK);

    glGenTextures(2, t);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, &v);
    check(v == GL_DEPTH_COMPONENT, "textures: GL_DEPTH_STENCIL_TEXTURE_MODE is GL_DEPTH_COMPONENT");
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, &v);
    check(v == GL_STENCIL_INDEX && glGetError() == GL_NO_ERROR, "textures: set to GL_STENCIL_INDEX");
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_RGBA);
    check(glGetError() == GL_INVALID_ENUM, "textures: GL_RGBA is GL_INVALID_ENUM");
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, t[1]);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_R, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAX_LEVEL, 0);
    check(glGetError() == GL_NO_ERROR && get_int(GL_TEXTURE_BINDING_2D_MULTISAMPLE) == (GLint)t[1],
          "textures: a 2D multisample texture bound, its swizzle and max level set");
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    check(glGetError() == GL_INVALID_ENUM, "textures: no sampler state for it (GL_INVALID_ENUM)");
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_BASE_LEVEL, 1);
    check(glGetError() == GL_INVALID_OPERATION, "textures: its base level 1 is GL_INVALID_OPERATION");
    glTexStorage2D(GL_TEXTURE_2D_MULTISAMPLE, 1, GL_RGBA8, 4, 4);
    check(glGetError() == GL_INVALID_ENUM, "textures: glTexStorage2D of it is GL_INVALID_ENUM");
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glBindTexture(GL_TEXTURE_2D, t[1]);
    check(glGetError() == GL_INVALID_OPERATION, "textures: a multisample name bound as 2D is GL_INVALID_OPERATION");

    glBindProgramPipeline(0);
    check(glGetError() == GL_NO_ERROR && get_int(GL_PROGRAM_PIPELINE_BINDING) == 0, "pipelines: 0 bound");

    check(get_inti(GL_IMAGE_BINDING_NAME, 255) == 0 && get_inti(GL_IMAGE_BINDING_ACCESS, 255) == GL_READ_ONLY &&
              get_inti(GL_IMAGE_BINDING_FORMAT, 255) == GL_R32UI && glGetError() == GL_NO_ERROR,
          "images: unit 255 unbound, GL_READ_ONLY, GL_R32UI");
    glBindImageTexture(256, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    check(glGetError() == GL_INVALID_VALUE, "images: unit 256 is GL_INVALID_VALUE");
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGB8);
    check(glGetError() == GL_INVALID_VALUE, "images: GL_RGB8 is GL_INVALID_VALUE");
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_RGBA, GL_R32UI);
    check(glGetError() == GL_INVALID_ENUM, "images: an access of GL_RGBA is GL_INVALID_ENUM");
    if (!gpu) { /* uploads read the Pi's timer, which vamos lacks */
        glDeleteTextures(2, t);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindImageTexture(0, t[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
    check(glGetError() == GL_INVALID_OPERATION, "images: a mutable texture is GL_INVALID_OPERATION");
    glDeleteTextures(1, &t[0]);
    glGenTextures(1, &t[0]);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 4, 4);
    {
        GLint w = -1, f = -1, rs = -1, rt = -1, w2 = -1, d = -1;
        GLfloat ff = 0.0f;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_INTERNAL_FORMAT, &f);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_RED_SIZE, &rs);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_RED_TYPE, &rt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_DEPTH_SIZE, &d);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &w2);
        glGetTexLevelParameterfv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ff);
        check(glGetError() == GL_NO_ERROR && w == 2 && f == GL_RGBA8 && rs == 8 && rt == GL_UNSIGNED_NORMALIZED &&
                  d == 0 && w2 == 0 && ff == 4.0f,
              "level queries: an RGBA8 texture's levels, one not defined");
        glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP, 0, GL_TEXTURE_WIDTH, &w);
        check(glGetError() == GL_INVALID_ENUM, "level queries: GL_TEXTURE_CUBE_MAP is GL_INVALID_ENUM");
        glGetTexLevelParameteriv(GL_TEXTURE_2D, -1, GL_TEXTURE_WIDTH, &w);
        check(glGetError() == GL_INVALID_VALUE, "level queries: level -1 is GL_INVALID_VALUE");
    }
    glBindImageTexture(3, t[0], 1, GL_TRUE, 2, GL_WRITE_ONLY, GL_RGBA8);
    glGetBooleani_v(GL_IMAGE_BINDING_LAYERED, 3, &bv);
    check(glGetError() == GL_NO_ERROR && get_inti(GL_IMAGE_BINDING_NAME, 3) == (GLint)t[0] &&
              get_inti(GL_IMAGE_BINDING_LEVEL, 3) == 1 && bv == GL_TRUE && get_inti(GL_IMAGE_BINDING_LAYER, 3) == 2 &&
              get_inti(GL_IMAGE_BINDING_ACCESS, 3) == GL_WRITE_ONLY && get_inti(GL_IMAGE_BINDING_FORMAT, 3) == GL_RGBA8,
          "images: an immutable texture's level 1 bound to unit 3, every query");
    glDeleteTextures(2, t);
    check(get_inti(GL_IMAGE_BINDING_NAME, 3) == 0 && glGetError() == GL_NO_ERROR,
          "images: the texture deleted, unit 3 unbound");
}

/* ES 3.1's state refused in an ES 3.0 context. */
static void in_es30(void)
{
    GLint v = -1;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    check(glGetError() == GL_INVALID_ENUM, "ES 3.0: GL_SHADER_STORAGE_BUFFER is GL_INVALID_ENUM");
    glEnable(GL_SAMPLE_MASK);
    check(glGetError() == GL_INVALID_ENUM, "ES 3.0: GL_SAMPLE_MASK is GL_INVALID_ENUM");
    glSampleMaski(0, 1);
    check(glGetError() == GL_INVALID_OPERATION, "ES 3.0: glSampleMaski is GL_INVALID_OPERATION");
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &v);
    check(glGetError() == GL_INVALID_ENUM, "ES 3.0: GL_MAX_IMAGE_UNITS is GL_INVALID_ENUM");
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &v);
    check(glGetError() == GL_INVALID_ENUM, "ES 3.0: GL_SHADER_STORAGE_BUFFER_BINDING is GL_INVALID_ENUM");
}

int main(int argc, char **argv)
{
    void *c31, *c30;
    ULONG err;
    int a, es31 = 0, limits = 0, gpu = 1;
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "ES31"))
            es31 = 1;
        else if (!strcmp(argv[a], "NOGPU"))
            gpu = 0;
        else if (!strcmp(argv[a], "LIMITS"))
            limits = 1;
    }
    if (!OGLES2Open()) {
        printf("FAIL: no %s\n", OGLES2NAME);
        return 20;
    }
    if (limits) {
        if (!(c31 = context(31, &err))) {
            printf("ERROR: no ES 3.1 context (error %lu): a library built with ES31=1 is needed\n", (unsigned long)err);
            OGLES2Close();
            return 20;
        }
        aglMakeCurrent(c31);
        print_limits();
        aglDestroyContext(c31);
        OGLES2Close();
        return 0;
    }
    printf("m8_es31: OpenGL ES 3.1 contexts\n");
    c31 = context(31, &err);
    if (!es31) {
        check(!c31 && err == OGLES2_CCE_UNSUPPORTED, "ES 3.1 refused (a build without ES31=1)");
        if (c31)
            aglDestroyContext(c31);
    } else if (!c31) {
        check(0, "ES 3.1: a context");
    } else {
        aglMakeCurrent(c31);
        check(prefix(glGetString(GL_VERSION), "OpenGL ES 3.1 ") &&
                  prefix(glGetString(GL_SHADING_LANGUAGE_VERSION), "OpenGL ES GLSL ES 3.10") &&
                  get_int(GL_MAJOR_VERSION) == 3 && get_int(GL_MINOR_VERSION) == 1,
              "ES 3.1: the version strings and numbers");
        buffers();
        attrib_binding();
        others(gpu);
        check(glGetError() == GL_NO_ERROR, "ES 3.1: no GL error left over");
        aglDestroyContext(c31);
        c30 = context(30, &err);
        if (c30) {
            aglMakeCurrent(c30);
            check(prefix(glGetString(GL_VERSION), "OpenGL ES 3.1 "), "ES 3.0 asked: 3.1 given (the highest)");
            aglDestroyContext(c30);
        }
    }
    /* an ES 3.0 context, from an ES3=1 build only: ES 3.1's state refused */
    if (!es31 && (c30 = context(30, &err)) != 0) {
        aglMakeCurrent(c30);
        in_es30();
        aglDestroyContext(c30);
    }
    OGLES2Close();
    printf("%s: %d of %d\n", failures ? "FAILED" : "PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
