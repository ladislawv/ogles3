/*
 * GLES3 stage A test (docs/GLES3_PLAN.md §4 item 7): OpenGL ES 3.0 contexts.
 *   usage: m8_es3 [NOGPU] [ES3]     (PASS/FAIL lines, returns 0 or 10)
 *          m8_es3 LIMITS [NOGPU]    (an ES3 context's limits, `v3dglslc -L`'s format)
 * State-only contexts (no surface), so it runs under vamos too (NOGPU).
 * ES3: the library is a test build (make ES3=1) and must give ES 3.0
 * contexts; without it the tag's 30 must be refused.
 *
 * Covered so far (stage A item 1): the version tag and its errors, the
 * version strings, GL_MAJOR/MINOR_VERSION, GL_EXTENSIONS against
 * glGetStringi, ES3-only entries and queries refused in GLES2 contexts, the
 * share groups' one version, 48 combined texture units in both versions.
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/ogles2.h>
#include <GLES3/gl3.h>

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

static void *context(ULONG version, ULONG *err, void *share)
{
    *err = 99;
    return aglCreateContextTags2(err, OGLES2_CCT_PISTORM_STATE_ONLY, TRUE, OGLES2_CCT_PISTORM_ES_VERSION, version,
                                 share ? OGLES2_CCT_SHARE_WITH : TAG_IGNORE, (ULONG)share, TAG_DONE);
}

static GLint get_int(GLenum pname)
{
    GLint v = -12345;
    glGetIntegerv(pname, &v);
    return v;
}

/* The limits an ES3 context answers, as `v3dglslc -L` prints them (the ES 2.0
 * and 3.0 ones; ES 3.1's come with stage D). A host script compares the two. */
static void print_limits(void)
{
#define L(e, n) { e, #e, n }
    static const struct { GLenum e; const char *name; int n; } limits[] = {
        L(GL_MAX_VERTEX_ATTRIBS, 1), L(GL_MAX_VERTEX_UNIFORM_VECTORS, 1),
        L(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 1), L(GL_MAX_VARYING_VECTORS, 1),
        L(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 1), L(GL_MAX_TEXTURE_IMAGE_UNITS, 1),
        L(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 1), L(GL_MAX_TEXTURE_SIZE, 1),
        L(GL_MAX_CUBE_MAP_TEXTURE_SIZE, 1), L(GL_MAX_RENDERBUFFER_SIZE, 1), L(GL_MAX_VIEWPORT_DIMS, 2),
        L(GL_ALIASED_POINT_SIZE_RANGE, 2), L(GL_ALIASED_LINE_WIDTH_RANGE, 2), L(GL_SUBPIXEL_BITS, 1),
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
    };
#undef L
    unsigned i, k;
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        GLint64 v[2] = { 0, 0 };
        glGetInteger64v(limits[i].e, v);
        printf("%s", limits[i].name);
        for (k = 0; k < (unsigned)limits[i].n; k++)
            printf(" %ld", (long)v[k]); /* every value fits 32 bits */
        printf("\n");
    }
    if (glGetError() != GL_NO_ERROR)
        printf("ERROR: a limit query failed\n");
}

/* A shader's GL_COMPILE_STATUS, and whether its log has `text`. */
static int compiles(GLenum type, const char *src, const char *text, int *log_has)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[256];
    log[0] = 0;
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    glGetShaderInfoLog(s, sizeof(log), 0, log);
    if (log_has)
        *log_has = text && strstr(log, text) != 0;
    glDeleteShader(s);
    return ok;
}

static const char vs300[] = "/* a comment */\n  // and another\n#version 300 es\n"
                            "in vec4 p;\nvoid main() { gl_Position = p; }\n";
static const char vs310[] = "#version 310 es\nin vec4 p;\nvoid main() { gl_Position = p; }\n";
static const char vs100[] = "#version 100\nattribute vec4 p;\nvoid main() { gl_Position = p; }\n";

/* GL_EXTENSIONS and glGetStringi list the same names in the same order. */
static int extensions_agree(void)
{
    const char *all = (const char *)glGetString(GL_EXTENSIONS), *p = all;
    GLint n = get_int(GL_NUM_EXTENSIONS), i;
    if (!all || n <= 0)
        return 0;
    for (i = 0; i < n; i++) {
        const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        size_t len = name ? strlen(name) : 0;
        if (!len || strncmp(p, name, len) || (p[len] != ' ' && p[len] != 0))
            return 0;
        p += len;
        if (*p == ' ')
            p++;
    }
    return *p == 0 && glGetError() == GL_NO_ERROR;
}

int main(int argc, char **argv)
{
    int es3 = 0, limits = 0, k;
    ULONG err;
    void *c2, *c3, *c;

    for (k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "ES3"))
            es3 = 1;
        else if (!strcmp(argv[k], "LIMITS"))
            limits = 1;
    }
    if (!OGLES2Open()) {
        printf("FAIL: OGLES2Open\n");
        return 10;
    }
    if (limits) {
        c3 = context(30, &err, 0);
        if (!c3) {
            printf("ERROR: no ES 3.0 context (error %lu): a library built with ES3=1 is needed\n",
                   (unsigned long)err);
            OGLES2Close();
            return 10;
        }
        aglMakeCurrent(c3);
        print_limits();
        aglDestroyContext(c3);
        OGLES2Close();
        return 0;
    }
    printf("m8_es3: OpenGL ES 3.0 contexts (library %ld.%ld, %s build)\n", (long)OGLES2Base->lib_Version,
           (long)OGLES2Base->lib_Revision, es3 ? "an ES3" : "a release");

    /* The tag's values. */
    c = context(25, &err, 0);
    check(!c && err == OGLES2_CCE_BAD_TAGS, "version 25: OGLES2_CCE_BAD_TAGS");
    c = context(31, &err, 0);
    check(!c && err == OGLES2_CCE_UNSUPPORTED, "version 31: OGLES2_CCE_UNSUPPORTED (no ES 3.1 yet)");
    c3 = context(30, &err, 0);
    if (es3)
        check(c3 && err == OGLES2_CCE_OK, "version 30: an ES 3.0 context");
    else
        check(!c3 && err == OGLES2_CCE_UNSUPPORTED, "version 30: OGLES2_CCE_UNSUPPORTED (not a test build)");
    c2 = context(20, &err, 0);
    check(c2 && err == OGLES2_CCE_OK, "version 20: an ES 2.0 context");
    if (!c2) {
        OGLES2Close();
        return 10;
    }

    /* The GLES2 context: as 1.x, with 48 combined units; the ES3 entries and
     * queries refused. */
    aglMakeCurrent(c2);
    check(prefix(glGetString(GL_VERSION), "OpenGL ES 2.0 ogles2.library "), "ES 2.0: GL_VERSION");
    check(prefix(glGetString(GL_SHADING_LANGUAGE_VERSION), "OpenGL ES GLSL ES 1.00"), "ES 2.0: GLSL ES 1.00");
    check(get_int(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) == 48 && glGetError() == GL_NO_ERROR,
          "ES 2.0: 48 combined texture units");
    check(get_int(GL_MAJOR_VERSION) == -12345 && glGetError() == GL_INVALID_ENUM,
          "ES 2.0: GL_MAJOR_VERSION is GL_INVALID_ENUM");
    check(glGetStringi(GL_EXTENSIONS, 0) == 0 && glGetError() == GL_INVALID_OPERATION,
          "ES 2.0: glGetStringi is GL_INVALID_OPERATION");
    {
        GLint64 v = -5;
        glGetInteger64v(GL_MAX_TEXTURE_SIZE, &v);
        check(v == -5 && glGetError() == GL_INVALID_OPERATION, "ES 2.0: glGetInteger64v is GL_INVALID_OPERATION");
    }
    glActiveTexture(GL_TEXTURE0 + 47);
    check(glGetError() == GL_NO_ERROR, "ES 2.0: glActiveTexture(GL_TEXTURE47)");
    glActiveTexture(GL_TEXTURE0 + 48);
    check(glGetError() == GL_INVALID_ENUM, "ES 2.0: glActiveTexture(GL_TEXTURE48) is GL_INVALID_ENUM");
    glActiveTexture(GL_TEXTURE0);
    {
        int has = 0;
        check(!compiles(GL_VERTEX_SHADER, vs300, "needs an OpenGL ES 3 context", &has) && has,
              "ES 2.0: #version 300 es (after comments) refused, with the reason in the log");
        check(compiles(GL_VERTEX_SHADER, vs100, 0, 0), "ES 2.0: #version 100 compiles");
    }

    if (c3) {
        /* Contexts share objects only with their own version. */
        c = context(30, &err, c2);
        check(!c && err == OGLES2_CCE_BAD_TAGS, "an ES 3.0 context cannot share with an ES 2.0 one");
        c = context(20, &err, c3);
        check(!c && err == OGLES2_CCE_BAD_TAGS, "an ES 2.0 context cannot share with an ES 3.0 one");
        c = context(30, &err, c3);
        check(c && err == OGLES2_CCE_OK, "two ES 3.0 contexts share objects");
        if (c)
            aglDestroyContext(c);

        aglMakeCurrent(c3);
        check(prefix(glGetString(GL_VERSION), "OpenGL ES 3.0 ogles2.library "), "ES 3.0: GL_VERSION");
        check(prefix(glGetString(GL_SHADING_LANGUAGE_VERSION), "OpenGL ES GLSL ES 3.00"), "ES 3.0: GLSL ES 3.00");
        check(get_int(GL_MAJOR_VERSION) == 3 && get_int(GL_MINOR_VERSION) == 0 && glGetError() == GL_NO_ERROR,
              "ES 3.0: GL_MAJOR_VERSION 3, GL_MINOR_VERSION 0");
        check(extensions_agree(), "ES 3.0: glGetStringi lists GL_EXTENSIONS");
        check(glGetStringi(GL_EXTENSIONS, (GLuint)get_int(GL_NUM_EXTENSIONS)) == 0 &&
                  glGetError() == GL_INVALID_VALUE,
              "ES 3.0: glGetStringi past the last is GL_INVALID_VALUE");
        check(glGetStringi(GL_VENDOR, 0) == 0 && glGetError() == GL_INVALID_ENUM,
              "ES 3.0: glGetStringi(GL_VENDOR) is GL_INVALID_ENUM");
        {
            GLint64 v = 0;
            glGetInteger64v(GL_MAX_ELEMENT_INDEX, &v);
            check(v == 0xffffff && glGetError() == GL_NO_ERROR, "ES 3.0: GL_MAX_ELEMENT_INDEX 2^24 - 1");
        }
        check(get_int(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) == 48 && get_int(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT) == 256 &&
                  glGetError() == GL_NO_ERROR,
              "ES 3.0: 48 combined texture units, uniform buffer offsets aligned to 256");
        {
            int has = 0;
            check(compiles(GL_VERTEX_SHADER, vs300, 0, 0), "ES 3.0: #version 300 es compiles");
            check(compiles(GL_VERTEX_SHADER, vs100, 0, 0), "ES 3.0: #version 100 compiles");
            check(!compiles(GL_VERTEX_SHADER, vs310, "needs an OpenGL ES 3.1 context", &has) && has,
                  "ES 3.0: #version 310 es refused, with the reason in the log");
        }
        aglDestroyContext(c3);
    }
    aglDestroyContext(c2);
    OGLES2Close();
    printf("%s: %d of %d\n", failures ? "FAILED" : "PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
