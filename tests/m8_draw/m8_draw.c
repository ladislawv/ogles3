/*
 * GLES3 stage A drawing test (docs/GLES3_PLAN.md §4 items 3, 4 and 7): GLSL
 * ES 3.00 programs drawn in an OpenGL ES 3.0 context (a library built with
 * ES3=1) into a 64x64 bitmap, their pixels read back. Nothing to watch.
 * Needs the GPU (not vamos). The programs' sources are in shaders/ in the
 * current directory and their files (format 2) in PROGDIR:, as `make
 * test-v3dp` writes them, so no compiler is needed.
 *   usage: m8_draw
 *   flat   a flat varying: the provoking (last) vertex's colour over the
 *          whole triangle, not a blend of the three
 *   block  a uniform block member reads zeros (no buffers before stage B)
 *   size   textureSize of a 16x8 texture, and of its base level 1 (8x4)
 *   vertex id  gl_VertexID's real numbers: glDrawArrays from vertex 3,
 *          indices as given, with a base vertex, and past 8-bit indices
 *   variant, 32-bit samplers, alpha to coverage  draw-time variants by key
 *          (item 5): compiled at the first draw with the compiler, from the
 *          cache's .v3dx without it, or from a shipped .v3dw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/ogles2.h>
#include <GLES3/gl3.h>

struct GfxBase *GfxBase;
static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

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

/* shaders/<name>.vert and .frag, linked; 0 if not (the log printed). */
static GLuint program(const char *name)
{
    char vn[32], fn[32], log[256];
    char *src[2];
    GLuint p = glCreateProgram(), s;
    GLint ok = 0, k;
    snprintf(vn, sizeof(vn), "%s.vert", name);
    snprintf(fn, sizeof(fn), "%s.frag", name);
    src[0] = read_text(vn);
    src[1] = read_text(fn);
    for (k = 0; k < 2; k++) {
        if (!src[k]) {
            printf("     no shaders/%s\n", k ? fn : vn);
            continue;
        }
        s = glCreateShader(k ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource(s, 1, (const GLchar *const *)&src[k], 0);
        glCompileShader(s);
        glAttachShader(p, s);
        glDeleteShader(s);
        free(src[k]);
    }
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), 0, log);
        printf("     %s: link failed: %s\n", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* A triangle over the whole 64x64 framebuffer (its third corner far off). */
static const GLfloat cover[6] = { -1, -1, 3, -1, -1, 3 };

static void draw_cover(GLuint p)
{
    GLint pos = glGetAttribLocation(p, "pos");
    glUseProgram(p);
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, cover);
    glEnableVertexAttribArray((GLuint)pos);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray((GLuint)pos);
}

/* The pixel at (x, y) is (r, g, b, a) within `tol`. */
static int pixel_is(int x, int y, int r, int g, int b, int a, int tol)
{
    GLubyte px[4];
    int want[4], k;
    want[0] = r; want[1] = g; want[2] = b; want[3] = a;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (k = 0; k < 4; k++)
        if (px[k] - want[k] > tol || want[k] - px[k] > tol) {
            printf("     pixel (%d, %d) is %d %d %d %d, not %d %d %d %d\n", x, y, px[0], px[1], px[2], px[3],
                   r, g, b, a);
            return 0;
        }
    return 1;
}

static void test_flat(void)
{
    static const GLfloat col[12] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1 }; /* red, green, blue */
    GLuint p = program("es3flat");
    GLint c;
    if (!p) {
        check(0, "flat: es3flat links");
        return;
    }
    c = glGetAttribLocation(p, "col");
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer((GLuint)c, 4, GL_FLOAT, GL_FALSE, 0, col);
    glEnableVertexAttribArray((GLuint)c);
    draw_cover(p);
    glDisableVertexAttribArray((GLuint)c);
    check(pixel_is(2, 2, 0, 0, 255, 255, 0) && pixel_is(32, 32, 0, 0, 255, 255, 0) &&
              pixel_is(61, 3, 0, 0, 255, 255, 0) && pixel_is(3, 61, 0, 0, 255, 255, 0),
          "flat: the last vertex's blue everywhere (not a blend of red, green and blue)");
    glDeleteProgram(p);
}

static void test_block(void)
{
    GLuint p = program("es3block");
    if (!p) {
        check(0, "block: es3block links");
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(10, 10, 64, 128, 191, 255, 1) && pixel_is(50, 40, 64, 128, 191, 255, 1),
          "block: a uniform block member reads zero (0.25, 0.5, 0.75 + 0)");
    glDeleteProgram(p);
}

static void test_size(void)
{
    static GLubyte texels[16 * 8 * 4];
    GLuint p = program("es3size"), t;
    if (!p) {
        check(0, "size: es3size links");
        return;
    }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 8, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 16, 8, 0, 255, 0), "size: textureSize of a 16x8 texture");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 8, 4, 0, 255, 0), "size: textureSize at base level 1 (8x4)");
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
}

/* gl_VertexID's numbers, and the attributes read at them: the covering
 * triangle lies only at the vertices each draw names (the others far off
 * screen), so a wrong address draws nothing, a wrong number the wrong
 * colour. 300 vertices: the last draw's numbers outgrow 8-bit indices. */
static void test_vertex_id(void)
{
    static GLfloat v[300 * 2];
    static const GLubyte i8[3] = { 0, 1, 2 };
    static const GLushort i16[3] = { 6, 7, 8 };
    GLuint p = program("es3vid");
    GLint pos;
    int k;
    if (!p) {
        check(0, "vertex id: es3vid links");
        return;
    }
    for (k = 0; k < 300; k++) { /* far off screen... */
        v[2 * k] = 5.0f;
        v[2 * k + 1] = 5.0f;
    }
    for (k = 0; k < 3; k++) { /* ...but at 3-5, 6-8 and 297-299 */
        int at[3], j;
        at[0] = 3 + k; at[1] = 6 + k; at[2] = 297 + k;
        for (j = 0; j < 3; j++) {
            v[2 * at[j]] = cover[2 * k];
            v[2 * at[j] + 1] = cover[2 * k + 1];
        }
    }
    pos = glGetAttribLocation(p, "pos");
    glUseProgram(p);
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray((GLuint)pos);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 3, 3);
    check(pixel_is(30, 30, 5, 0, 0, 255, 0), "vertex id: glDrawArrays from vertex 3 (the last is 5)");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, i16);
    check(pixel_is(30, 30, 8, 0, 0, 255, 0), "vertex id: indices 6, 7, 8 as given");
    glClear(GL_COLOR_BUFFER_BIT);
    {
        static const GLushort z16[3] = { 0, 1, 2 };
        glDrawElementsBaseVertexOES(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, z16, 6);
    }
    check(pixel_is(30, 30, 8, 0, 0, 255, 0), "vertex id: indices 0, 1, 2 with base vertex 6");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsBaseVertexOES(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, i8, 297);
    check(pixel_is(30, 30, 299 & 255, 299 >> 8, 0, 255, 0),
          "vertex id: 8-bit indices with base vertex 297 (299: past 8 bits)");
    /* Primitive restart (ES 3.0's fixed index): the strip ends at 0xFFFF,
     * the next one (vertices 0-2) lies off screen; without the restart it
     * would go on from vertex 5 through 65535. */
    {
        static const GLushort strip[7] = { 3, 4, 5, 0xFFFF, 0, 1, 2 };
        glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLE_STRIP, 7, GL_UNSIGNED_SHORT, strip);
        glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        check(pixel_is(30, 30, 5, 0, 0, 255, 0) && glGetError() == GL_NO_ERROR,
              "primitive restart: a strip ended by index 0xFFFF");
    }
    glDisableVertexAttribArray((GLuint)pos);
    glDeleteProgram(p);
}

/* A draw-time variant (stage A item 5): es3out's precompiled fragment
 * variants are for a float and an unsigned target; drawn into the RGBA8
 * default framebuffer, the draw takes one compiled for its own key (now, or
 * earlier into the cache's .v3dx). The library's log says which. */
static void test_variant(void)
{
    GLuint p = program("es3out");
    if (!p) {
        check(0, "draw-time variant: es3out links");
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(30, 30, 0, 255, 0, 255, 0) && pixel_is(5, 58, 0, 255, 0, 255, 0),
          "draw-time variant: two outputs into one RGBA8 target draw its green");
    glDeleteProgram(p);
}

/* A format 1.0 file of an OpenGL ES 3 program, as a 1.x compiler made it,
 * in the cache (Run-M1 puts it there, item 7): the library refuses it and,
 * with the compiler, puts a format 2 file in its place; without the
 * compiler, in the next step, that file links. */
static void test_old(void)
{
    GLuint p = program("es3old");
    if (!p) {
        check(0, "a 1.x compiler's file of es3old refused and replaced: es3old links");
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(30, 30, 255, 128, 0, 255, 1), "a 1.x compiler's file of es3old refused and replaced");
    glDeleteProgram(p);
}

/* The keys' 32-bit samplers and alpha to coverage (stage A item 5), in a 4x
 * MSAA context: es3wide outputs one texel. A float texture needs the
 * sampler's 32-bit bit in the fragment key (a 16-bit return reads it
 * wrong); the shipped .v3dw has it. Alpha to coverage, its alpha 0.5,
 * covers two of the four samples, so the resolved pixel is half the texel's
 * (no file has that variant: compiled at the first draw, then from the
 * cache's .v3dx). The float texture with alpha to coverage needs both in
 * one key, which no file has either. */
static void test_wide_a2c(void)
{
    static const GLfloat fl[4] = { 0.25f, 0.5f, 0.75f, 0.5f };
    static const GLubyte by[4] = { 64, 128, 192, 128 };
    GLuint p = program("es3wide"), t[2];
    GLint samples = 0;
    int k;
    glGetIntegerv(GL_SAMPLES, &samples);
    check(samples == 4, "32-bit samplers and alpha to coverage: a 4x MSAA context");
    if (!p) {
        check(0, "32-bit samplers and alpha to coverage: es3wide links");
        return;
    }
    glGenTextures(2, t);
    for (k = 0; k < 2; k++) {
        glBindTexture(GL_TEXTURE_2D, t[k]);
        if (k)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, fl);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, by);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glClearColor(0, 0, 0, 0);

    glBindTexture(GL_TEXTURE_2D, t[1]);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 64, 128, 191, 128, 2), "32-bit samplers: a float texture read right");
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 32, 64, 96, 64, 20), "alpha to coverage: alpha 0.5 covers half the samples");
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 32, 64, 96, 64, 20), "both: a float texture with alpha to coverage");
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 64, 128, 192, 128, 2), "neither: an RGBA8 texture covers every sample");
    glDeleteTextures(2, t);
    glDeleteProgram(p);
}

int main(void)
{
    struct BitMap *bm;
    void *ctx;
    ULONG e = 0;
    printf("ogles2 M8: GLSL ES 3.00 programs drawn in an OpenGL ES 3.0 context\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (!GfxBase || !OGLES2Open()) {
        printf("needs graphics 39 and %s\n", OGLES2NAME);
        if (GfxBase)
            CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    bm = AllocBitMap(64, 64, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm || !(ctx = aglCreateContextTags2(&e, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_PISTORM_ES_VERSION, 30,
                                             TAG_DONE))) {
        printf("FAIL: no ES 3.0 context (error %lu): a library built with ES3=1 is needed\n", (unsigned long)e);
        if (bm)
            FreeBitMap(bm);
        OGLES2Close();
        CloseLibrary((struct Library *)GfxBase);
        return 20;
    }
    aglMakeCurrent(ctx);
    glViewport(0, 0, 64, 64);
    test_flat();
    test_block();
    test_size();
    test_vertex_id();
    test_variant();
    test_old();
    check(glGetError() == GL_NO_ERROR, "no GL error left over");
    aglDestroyContext(ctx);
    if ((ctx = aglCreateContextTags2(&e, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_PISTORM_ES_VERSION, 30,
                                     OGLES2_CCT_PISTORM_MSAA, 4, TAG_DONE))) {
        aglMakeCurrent(ctx);
        glViewport(0, 0, 64, 64);
        test_wide_a2c();
        check(glGetError() == GL_NO_ERROR, "no GL error left over (4x MSAA)");
        aglDestroyContext(ctx);
    } else {
        check(0, "a 4x MSAA ES 3.0 context");
    }
    FreeBitMap(bm);
    OGLES2Close();
    CloseLibrary((struct Library *)GfxBase);
    printf("%s: %d of %d\n", failures ? "FAILED" : "PASSED", checks - failures, checks);
    return failures ? 10 : 0;
}
