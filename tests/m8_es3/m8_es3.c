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
 * share groups' one version, 48 combined texture units in both versions;
 * (items 2 and 3) the #version check, GLSL ES 3.00 programs linked from
 * their format 2 files (shaders/ in the current directory, the files in
 * PROGDIR:, as `make test-v3dp` names them), explicit locations,
 * glGetFragDataLocation, the unsigned and non-square uniforms, transpose.
 */
#include <stdio.h>
#include <stdlib.h>
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

/* A shader from shaders/ in the current directory (tools/v3dglslc/test/shaders,
 * whose programs `make test-v3dp` compiles into files named by their hash). */
static char *read_text(const char *name)
{
    char path[64];
    FILE *f;
    long n;
    char *s;
    snprintf(path, sizeof(path), "shaders/%s", name);
    if (!(f = fopen(path, "rb")))
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = malloc(n + 1);
    if (s && fread(s, 1, n, f) != (size_t)n) {
        free(s);
        s = 0;
    }
    if (s)
        s[n] = 0;
    fclose(f);
    return s;
}

/* Links shaders/<name>.vert and .frag; the program, its status in *ok. */
static GLuint link_files(const char *name, GLint *ok)
{
    char vn[32], fn[32];
    char *vs, *fs;
    GLuint p = glCreateProgram(), s;
    *ok = 0;
    snprintf(vn, sizeof(vn), "%s.vert", name);
    snprintf(fn, sizeof(fn), "%s.frag", name);
    vs = read_text(vn);
    fs = read_text(fn);
    if (!vs || !fs) {
        printf("     (no shaders/%s or %s)\n", vn, fn);
        free(vs);
        free(fs);
        return p;
    }
    s = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(s, 1, (const GLchar *const *)&vs, 0);
    glCompileShader(s);
    glAttachShader(p, s);
    glDeleteShader(s);
    s = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(s, 1, (const GLchar *const *)&fs, 0);
    glCompileShader(s);
    glAttachShader(p, s);
    glDeleteShader(s);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, ok);
    if (!*ok) {
        char log[256];
        log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), 0, log);
        printf("     link %s: %s\n", name, log);
    }
    free(vs);
    free(fs);
    return p;
}

/* GLSL ES 3.00 programs in an ES3 context (stage A item 3): their files
 * (format 2) load, explicit locations hold, the ES3 uniform entries work. */
static void es3_programs(void)
{
    GLint ok, v[8];
    GLuint p = link_files("es3", &ok), q;
    check(ok, "ES 3.0: the GLSL ES 3.00 program es3 links from its format 2 file");
    if (ok) {
        check(glGetAttribLocation(p, "position") == 3, "ES 3.0: layout(location = 3) holds");
        check(glGetFragDataLocation(p, "colour") == 0 && glGetFragDataLocation(p, "id") == 1 &&
                  glGetFragDataLocation(p, "nothing") == -1 && glGetFragDataLocation(p, "gl_FragDepth") == -1 &&
                  glGetError() == GL_NO_ERROR,
              "ES 3.0: glGetFragDataLocation from the program's outputs");
        glGetProgramiv(p, GL_ACTIVE_UNIFORMS, v);
        check(v[0] == 5, "ES 3.0: the uniform blocks' 5 members are active uniforms");
    }
    q = link_files("es3tex", &ok);
    check(ok, "ES 3.0: es3tex, every sampler type, links");
    glDeleteProgram(q);
    glDeleteProgram(p);

    p = link_files("es3uni", &ok);
    check(ok, "ES 3.0: es3uni links");
    if (ok) {
        static const GLfloat rows[6] = { 1, 2, 3, 4, 5, 6 }; /* mat2x3 row by row: 3 rows of 2 */
        GLfloat f[6];
        GLuint u[3];
        GLint m23 = glGetUniformLocation(p, "m23"), u3 = glGetUniformLocation(p, "u3");
        GLint u1 = glGetUniformLocation(p, "u1"), flag = glGetUniformLocation(p, "flag");
        glUseProgram(p);
        glUniformMatrix2x3fv(m23, 1, GL_FALSE, rows);
        glGetUniformfv(p, m23, f);
        check(glGetError() == GL_NO_ERROR && f[0] == 1 && f[5] == 6, "ES 3.0: glUniformMatrix2x3fv");
        glUniformMatrix2x3fv(m23, 1, GL_TRUE, rows);
        glGetUniformfv(p, m23, f);
        /* column-major back: column 0 is rows' 1, 3, 5, column 1 2, 4, 6 */
        check(glGetError() == GL_NO_ERROR && f[0] == 1 && f[1] == 3 && f[2] == 5 && f[3] == 2 && f[5] == 6,
              "ES 3.0: transpose");
        glUniform3ui(u3, 7, 0x80000000u, 9);
        glUniform1ui(u1, 4000000000u);
        glGetUniformuiv(p, u3, u);
        check(glGetError() == GL_NO_ERROR && u[0] == 7 && u[1] == 0x80000000u && u[2] == 9,
              "ES 3.0: glUniform3ui and glGetUniformuiv");
        glGetUniformuiv(p, u1, u);
        check(u[0] == 4000000000u, "ES 3.0: glUniform1ui keeps all 32 bits");
        glUniform1i(u1, 3);
        check(glGetError() == GL_INVALID_OPERATION, "ES 3.0: glUniform1i on a uint is GL_INVALID_OPERATION");
        glUniform1ui(flag, 1);
        glGetUniformiv(p, flag, v);
        check(glGetError() == GL_NO_ERROR && v[0] == 1, "ES 3.0: glUniform1ui sets a bool");
        glUseProgram(0);
    }
    glDeleteProgram(p);
}

/* The same programs in a GLES2 context: not linked, and their binary
 * (format 2) refused. */
static void es3_programs_in_es2(void *c3, void *c2)
{
    GLint ok, len = 0;
    GLenum fmt = 0;
    GLuint p, q;
    void *bin;
    aglMakeCurrent(c3);
    p = link_files("es3", &ok);
    glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len); /* = OES_get_program_binary's */
    bin = len > 0 ? malloc(len) : 0;
    if (bin)
        glGetProgramBinaryOES(p, len, 0, &fmt, bin);
    glDeleteProgram(p);
    aglMakeCurrent(c2);
    q = link_files("es3", &ok);
    check(!ok, "ES 2.0: a GLSL ES 3.00 program does not link");
    glDeleteProgram(q);
    q = glCreateProgram();
    if (bin)
        glProgramBinaryOES(q, fmt, bin, len);
    glGetProgramiv(q, GL_LINK_STATUS, &ok);
    check(bin && !ok, "ES 2.0: its format 2 binary is refused (glProgramBinaryOES)");
    glDeleteProgram(q);
    free(bin);
}

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
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    check(glGetError() == GL_INVALID_ENUM, "ES 2.0: GL_PRIMITIVE_RESTART_FIXED_INDEX is GL_INVALID_ENUM");
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
            GLboolean on = GL_FALSE;
            glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            glGetBooleanv(GL_PRIMITIVE_RESTART_FIXED_INDEX, &on);
            check(on && glIsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX) && glGetError() == GL_NO_ERROR,
                  "ES 3.0: GL_PRIMITIVE_RESTART_FIXED_INDEX enabled");
            glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            check(!glIsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX), "ES 3.0: and disabled again");
        }
        {
            int has = 0;
            check(compiles(GL_VERTEX_SHADER, vs300, 0, 0), "ES 3.0: #version 300 es compiles");
            check(compiles(GL_VERTEX_SHADER, vs100, 0, 0), "ES 3.0: #version 100 compiles");
            check(!compiles(GL_VERTEX_SHADER, vs310, "needs an OpenGL ES 3.1 context", &has) && has,
                  "ES 3.0: #version 310 es refused, with the reason in the log");
        }
        es3_programs();
        es3_programs_in_es2(c3, c2);
        aglDestroyContext(c3);
    }
    aglDestroyContext(c2);
    OGLES2Close();
    printf("%s: %d of %d\n", failures ? "FAILED" : "PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
