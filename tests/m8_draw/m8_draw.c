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
#include <GLES3/gl31.h> /* ES 3.0 and, in ES 3.1 contexts, 3.1 */

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64 /* OES_compressed_ETC1_RGB8_texture */
#endif

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

/* Transform feedback varyings program() gives the next program (0: none). */
static const char *const *xfb_names;
static GLsizei xfb_count;

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
    if (xfb_count)
        glTransformFeedbackVaryings(p, xfb_count, xfb_names, GL_INTERLEAVED_ATTRIBS);
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
    /* GL_RASTERIZER_DISCARD: neither a clear nor a draw changes a pixel. */
    glEnable(GL_RASTERIZER_DISCARD);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableVertexAttribArray((GLuint)c);
    draw_cover(p);
    glDisableVertexAttribArray((GLuint)c);
    glDisable(GL_RASTERIZER_DISCARD);
    glClearColor(0, 0, 0, 0);
    check(pixel_is(32, 32, 0, 0, 255, 255, 0) && glGetError() == GL_NO_ERROR,
          "rasterizer discard: a clear and a draw leave the picture as it was");
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
          "block: a uniform block member without a buffer reads zero (0.25, 0.5, 0.75 + 0)");
    {
        static const GLchar *names[1] = { "Extra.add" };
        static GLfloat data[64 + 4]; /* the range at 256 bytes */
        GLuint idx = glGetUniformBlockIndex(p, "Extra"), ui = 0, buf;
        GLint size = 0, nu = 0, member = -1, offset = -1, bidx = -2, fs = 0;
        glGetActiveUniformBlockiv(p, idx, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        glGetActiveUniformBlockiv(p, idx, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &nu);
        glGetActiveUniformBlockiv(p, idx, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, &member);
        glGetActiveUniformBlockiv(p, idx, GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER, &fs);
        glGetUniformIndices(p, 1, names, &ui);
        glGetActiveUniformsiv(p, 1, &ui, GL_UNIFORM_OFFSET, &offset);
        glGetActiveUniformsiv(p, 1, &ui, GL_UNIFORM_BLOCK_INDEX, &bidx);
        check(idx == 0 && size == 16 && nu == 1 && member == (GLint)ui && fs && offset == 0 && bidx == 0 &&
                  glGetError() == GL_NO_ERROR,
              "block: Extra's index, size, member, offset (glGetActiveUniformBlockiv, glGetActiveUniformsiv)");
        data[64] = 0.25f;
        data[65] = 0.25f;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_UNIFORM_BUFFER, buf);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(data), data, GL_DYNAMIC_DRAW);
        glUniformBlockBinding(p, idx, 3);
        glBindBufferRange(GL_UNIFORM_BUFFER, 3, buf, 256, 16);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_cover(p);
        check(pixel_is(32, 32, 128, 191, 191, 255, 1), "block: Extra at binding 3 from a buffer range (+ 0.25, 0.25, 0, 0)");
        /* Two draws of one frame, the data changed between them: each its own. */
        data[64] = 0.0f;
        data[65] = 0.0f;
        data[66] = 0.25f;
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 32, 64);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_cover(p);
        glBufferSubData(GL_UNIFORM_BUFFER, 256, 16, data + 64);
        glScissor(32, 0, 32, 64);
        draw_cover(p);
        glDisable(GL_SCISSOR_TEST);
        check(pixel_is(16, 32, 128, 191, 191, 255, 1) && pixel_is(48, 32, 64, 128, 255, 255, 1) &&
                  glGetError() == GL_NO_ERROR,
              "block: a draw before and one after glBufferSubData in one frame");
        {
            GLfloat *m = glMapBufferRange(GL_UNIFORM_BUFFER, 256, 16, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
            if (m) {
                m[0] = 0.25f;
                m[1] = m[2] = m[3] = 0.0f;
            }
            glFlushMappedBufferRange(GL_UNIFORM_BUFFER, 0, 16);
            glUnmapBuffer(GL_UNIFORM_BUFFER);
            glClear(GL_COLOR_BUFFER_BIT);
            draw_cover(p);
            check(m && pixel_is(32, 32, 128, 128, 191, 255, 1) && glGetError() == GL_NO_ERROR,
                  "block: Extra written through glMapBufferRange, flushed (+ 0.25, 0, 0, 0)");
        }
        glDeleteBuffers(1, &buf);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_cover(p);
        check(pixel_is(32, 32, 64, 128, 191, 255, 1), "block: its buffer deleted, zero again");
    }
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

/* ES 3.0's sampler objects: a sampler bound to unit 0 is sampled with
 * instead of the texture's parameters, completeness too, and unbound the
 * texture's own count again (dEQP-GLES3's samplers do the same). es3wide
 * outputs one texel. */
static void test_sampler(void)
{
    static const GLubyte by[16] = { 64, 128, 192, 255, 64, 128, 192, 255, 64, 128, 192, 255, 64, 128, 192, 255 };
    GLuint p = program("es3wide"), t, s;
    if (!p) {
        check(0, "sampler objects: es3wide links");
        return;
    }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, by);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glGenSamplers(1, &s);
    /* The texture's default minification filter mipmaps: one level is incomplete. */
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 0, 0, 0, 255, 0), "sampler objects: the texture alone is incomplete (black)");
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindSampler(0, s);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 64, 128, 192, 255, 1), "sampler objects: a sampler without mipmaps completes it");
    glBindSampler(0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 64, 128, 192, 255, 1), "sampler objects: unbound, the texture's own filter");
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindSampler(0, s);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 0, 0, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "sampler objects: a mipmapping sampler makes it incomplete again");
    glBindSampler(0, 0);
    glDeleteSamplers(1, &s);
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
}

/* A w x h x d image of RGBA8 texels, slice (layer) z in the colour col(z). */
static GLubyte *slices(int w, int h, int d, void (*col)(int z, GLubyte *c))
{
    GLubyte *img = malloc((size_t)w * h * d * 4), c[4];
    int z, i;
    for (z = 0; img && z < d; z++) {
        col(z, c);
        for (i = 0; i < w * h; i++)
            memcpy(img + ((size_t)z * w * h + i) * 4, c, 4);
    }
    return img;
}

static void rgbw(int z, GLubyte *c) /* red, green, blue, white */
{
    c[0] = z == 0 || z == 3 ? 255 : 0;
    c[1] = z == 1 || z == 3 ? 255 : 0;
    c[2] = z >= 2 ? 255 : 0;
    c[3] = 255;
}

static void ramp(int z, GLubyte *c) /* red up, green down */
{
    c[0] = (GLubyte)(z * 32);
    c[1] = (GLubyte)(255 - z * 32);
    c[2] = 0;
    c[3] = 255;
}

/* Draws program p with its float uniform `u` at v; the middle pixel is (r, g, b). */
static int layer_is(GLuint p, const char *u, GLfloat v, int r, int g, int b, int tol)
{
    glUseProgram(p);
    glUniform1f(glGetUniformLocation(p, u), v);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    return pixel_is(32, 32, r, g, b, 255, tol);
}

/* ES 3.0's 2D array and 3D textures (stage B item 8): es3layer reads a
 * layer of an array, es3vol a 3D texture at a depth, at the middle of their
 * texels. A small 3D texture's level 1 (1x1x2) and a 64x64x8 one's (UIF
 * levels) made by glGenerateMipmap, which filters the slices in depth. */
static void test_layers(void)
{
    static const GLubyte yellow[2 * 2 * 4] = { 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255 };
    GLuint pa = program("es3layer"), pv = program("es3vol"), t[3];
    GLubyte *img;
    GLint b = 0;
    if (!pa || !pv) {
        check(0, "2D array and 3D textures: es3layer and es3vol link");
        return;
    }
    glGenTextures(3, t);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t[0]);
    img = slices(2, 2, 4, rgbw);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 2, 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    free(img);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &b);
    check(b == (GLint)t[0] && glGetError() == GL_NO_ERROR, "2D array: GL_TEXTURE_BINDING_2D_ARRAY");
    glUseProgram(pa);
    glUniform1i(glGetUniformLocation(pa, "tex"), 0);
    check(layer_is(pa, "layer", 1.0f, 0, 255, 0, 1) && layer_is(pa, "layer", 2.0f, 0, 0, 255, 1),
          "2D array: layers 1 and 2 (green, blue)");
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
    check(layer_is(pa, "layer", 1.0f, 255, 255, 0, 1) && layer_is(pa, "layer", 3.0f, 255, 255, 255, 1),
          "2D array: layer 1 replaced (yellow), layer 3 as it was");

    glBindTexture(GL_TEXTURE_3D, t[1]);
    img = slices(2, 2, 4, rgbw);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    free(img);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(pv);
    glUniform1i(glGetUniformLocation(pv, "tex"), 0);
    check(layer_is(pv, "z", 0.375f, 0, 255, 0, 1) && layer_is(pv, "z", 0.875f, 255, 255, 255, 1),
          "3D: slices 1 and 3 (green, white)");
    glGenerateMipmap(GL_TEXTURE_3D);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 1);
    check(layer_is(pv, "z", 0.25f, 128, 128, 0, 1) && layer_is(pv, "z", 0.75f, 128, 128, 255, 1) &&
              glGetError() == GL_NO_ERROR,
          "3D: glGenerateMipmap's level 1 (1x1x2), each slice two of level 0");

    glBindTexture(GL_TEXTURE_3D, t[2]);
    img = slices(64, 64, 8, ramp);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 64, 64, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    free(img);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    check(layer_is(pv, "z", 5.5f / 8.0f, 160, 95, 0, 1), "3D: a 64x64x8 texture's slice 5");
    glGenerateMipmap(GL_TEXTURE_3D);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 1);
    check(layer_is(pv, "z", 2.5f / 4.0f, 144, 111, 0, 1) && glGetError() == GL_NO_ERROR,
          "3D: its level 1's slice 2 (32x32x4), slices 4 and 5 filtered");
    glDeleteTextures(3, t);
    glDeleteProgram(pa);
    glDeleteProgram(pv);
}

/* ES 3.0's pixel store: a texel from inside a larger image (row length,
 * skipped rows and pixels), a 2D array's layers from images with a gap
 * (image height, skipped images), and glReadPixels into the middle of a
 * larger buffer. */
static void test_pixel_store(void)
{
    static GLubyte img[3 * 4 * 4], imgs[3 * 2 * 4];
    GLubyte out[3 * 4 * 4];
    GLuint p = program("es3wide"), pa = program("es3layer"), t[2];
    if (!p || !pa) {
        check(0, "pixel store: es3wide and es3layer link");
        return;
    }
    memset(img, 0, sizeof(img));
    img[(1 * 4 + 2) * 4 + 0] = 10; /* (2, 1) of a 4x3 image */
    img[(1 * 4 + 2) * 4 + 1] = 200;
    img[(1 * 4 + 2) * 4 + 2] = 30;
    img[(1 * 4 + 2) * 4 + 3] = 255;
    glGenTextures(2, t);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 4);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 10, 200, 30, 255, 1), "pixel store: a texel at (2, 1) of a 4-texel row length");
    memset(out, 7, sizeof(out));
    glPixelStorei(GL_PACK_ROW_LENGTH, 4);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    glPixelStorei(GL_PACK_SKIP_ROWS, 2);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    check(out[(2 * 4 + 1) * 4] >= 9 && out[(2 * 4 + 1) * 4] <= 11 && out[(2 * 4 + 1) * 4 + 1] >= 199 &&
              out[(2 * 4 + 1) * 4 + 1] <= 201 && out[0] == 7 && out[(2 * 4 + 2) * 4] == 7,
          "pixel store: glReadPixels at (1, 2) of a 4-pixel row length, the rest untouched");
    /* Three 1x2 images, the first row of each in a colour: layers from images 1 and 2. */
    memset(imgs, 0, sizeof(imgs));
    imgs[(1 * 2) * 4 + 0] = 255; /* image 1: red */
    imgs[(1 * 2) * 4 + 3] = 255;
    imgs[(2 * 2) * 4 + 2] = 255; /* image 2: blue */
    imgs[(2 * 2) * 4 + 3] = 255;
    glBindTexture(GL_TEXTURE_2D_ARRAY, t[1]);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 2);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 1, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, imgs);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(pa);
    glUniform1i(glGetUniformLocation(pa, "tex"), 0);
    check(layer_is(pa, "layer", 0.0f, 255, 0, 0, 1) && layer_is(pa, "layer", 1.0f, 0, 0, 255, 1) &&
              glGetError() == GL_NO_ERROR,
          "pixel store: layers from images 1 and 2 of 2-row images");
    glDeleteTextures(2, t);
    glDeleteProgram(p);
    glDeleteProgram(pa);
}

/* Draws es3wide (bound, its texture unit 0) over a 1x1 texture made from
 * the unpack buffer's bytes at `offset`; the middle pixel is (r, g, b). */
static int unpacked_is(GLuint p, GLuint t, GLuint buf, ULONG offset, int r, int g, int b)
{
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)offset);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    return pixel_is(32, 32, r, g, b, 255, 1);
}

/* ES 3.0's buffer targets: a texel from a pixel unpack buffer, the drawn
 * pixel read into a pack buffer and back as a texel, and a texel copied
 * from one buffer into another (glCopyBufferSubData). */
static void test_pixel_buffer(void)
{
    static const GLubyte data[16] = { 0, 0, 0, 255, 0, 0, 0, 0, 10, 200, 30, 255, 0, 0, 0, 0 };
    GLuint p = program("es3wide"), t, b[2];
    if (!p) {
        check(0, "pixel buffers: es3wide links");
        return;
    }
    glGenTextures(1, &t);
    glGenBuffers(2, b);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b[0]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
    check(unpacked_is(p, t, b[0], 8, 10, 200, 30), "pixel buffers: a texel from an unpack buffer");
    glBindBuffer(GL_PIXEL_PACK_BUFFER, b[1]);
    glBufferData(GL_PIXEL_PACK_BUFFER, 16, 0, GL_STREAM_READ);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void *)4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    check(unpacked_is(p, t, b[1], 4, 10, 200, 30), "pixel buffers: the pixel read into a pack buffer");
    glBindBuffer(GL_COPY_READ_BUFFER, b[0]);
    glBindBuffer(GL_COPY_WRITE_BUFFER, b[1]);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 4, 4);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 8, 12, 4);
    check(unpacked_is(p, t, b[1], 4, 0, 0, 0) && unpacked_is(p, t, b[1], 12, 10, 200, 30) &&
              glGetError() == GL_NO_ERROR,
          "pixel buffers: texels copied between buffers");
    glDeleteBuffers(2, b);
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
}

/* ES 3.0's vertex array objects: two, each with its position and colour
 * buffers (es3flat: the last vertex's colour), the second also with an
 * element buffer; drawing with each draws its own. */
static void test_vao(void)
{
    static const GLfloat red[12] = { 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1 };
    static const GLfloat green[12] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 };
    static const GLushort idx[3] = { 0, 1, 2 };
    GLuint p = program("es3flat"), va[2], bu[4];
    GLint pos, col, k;
    if (!p) {
        check(0, "vertex array objects: es3flat links");
        return;
    }
    pos = glGetAttribLocation(p, "pos");
    col = glGetAttribLocation(p, "col");
    glGenVertexArrays(2, va);
    glGenBuffers(4, bu);
    for (k = 0; k < 2; k++) {
        glBindVertexArray(va[k]);
        glBindBuffer(GL_ARRAY_BUFFER, bu[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(cover), cover, GL_STATIC_DRAW);
        glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray((GLuint)pos);
        glBindBuffer(GL_ARRAY_BUFFER, bu[1 + k]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(red), k ? green : red, GL_STATIC_DRAW);
        glVertexAttribPointer((GLuint)col, 4, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray((GLuint)col);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bu[3]); /* the second object's */
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(p);
    glBindVertexArray(va[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    check(pixel_is(32, 32, 255, 0, 0, 255, 0), "vertex array objects: the first's arrays (red)");
    glBindVertexArray(va[1]);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0);
    check(pixel_is(32, 32, 0, 255, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "vertex array objects: the second's arrays and element buffer (green)");
    glBindVertexArray(0);
    glDeleteVertexArrays(2, va);
    glDeleteBuffers(4, bu);
    glDeleteProgram(p);
}

/* ES 3.0's instanced draws: es3inst moves instance i right by i; a quad
 * over the left half drawn twice is the whole picture, each half in its
 * instanced colour; a divisor of 2 gives both instances the first. */
static void test_instanced(void)
{
    static const GLfloat quad[8] = { -1, -1, 0, -1, -1, 1, 0, 1 };
    static const GLfloat cols[8] = { 1, 0, 0, 1, 0, 1, 0, 1 }; /* red, green */
    static const GLfloat greens[16] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 };
    static const GLubyte idx[4] = { 0, 1, 2, 3 };
    GLuint p = program("es3inst");
    GLint pos, col, dv = -1;
    if (!p) {
        check(0, "instanced draws: es3inst links");
        return;
    }
    pos = glGetAttribLocation(p, "pos");
    col = glGetAttribLocation(p, "col");
    glUseProgram(p);
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glVertexAttribPointer((GLuint)col, 4, GL_FLOAT, GL_FALSE, 0, cols);
    glEnableVertexAttribArray((GLuint)pos);
    glEnableVertexAttribArray((GLuint)col);
    glVertexAttribDivisor((GLuint)col, 1);
    glGetVertexAttribiv((GLuint)col, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &dv);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 2);
    check(dv == 1 && pixel_is(16, 32, 255, 0, 0, 255, 0) && pixel_is(48, 32, 0, 255, 0, 255, 0),
          "instanced draws: glDrawArraysInstanced, two instances (red, green)");
    glVertexAttribDivisor((GLuint)col, 2);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsInstanced(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx, 2);
    check(pixel_is(16, 32, 255, 0, 0, 255, 0) && pixel_is(48, 32, 255, 0, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "instanced draws: glDrawElementsInstanced with a divisor of 2 (both red)");
    glVertexAttribDivisor((GLuint)col, 0);
    glVertexAttribPointer((GLuint)col, 4, GL_FLOAT, GL_FALSE, 0, greens);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    check(pixel_is(16, 32, 0, 255, 0, 255, 0) && pixel_is(48, 32, 0, 0, 0, 0, 0),
          "instanced draws: divisor 0 again, one instance (the left half, the last vertex's green)");
    glDisableVertexAttribArray((GLuint)pos);
    glDisableVertexAttribArray((GLuint)col);
    glDeleteProgram(p);
}

/* ES 3.0's vertex formats through es3int: an integer array of two
 * components (r its x / 255, alpha its w, the integer 1 the defaults give),
 * an unsigned current value (g), and the float input hf (b) from three
 * half floats, normalized unsigned ints and a packed 2_10_10_10 word. */
static void test_formats(void)
{
    static const GLint ints[6] = { 100, 7, 100, 7, 100, 7 };
    static const GLshort shorts[6] = { 50, 0, 50, 0, 50, 0 };
    static const GLushort halves[9] = { 0x3800, 0, 0, 0x3800, 0, 0, 0x3800, 0, 0 }; /* 0.5 */
    static const GLuint uints[3] = { 0x40000000UL, 0x40000000UL, 0x40000000UL };   /* 0.25 */
    static const GLuint packed[3] = { 1023, 1023, 1023 };                           /* x 1.0 */
    GLuint p = program("es3int");
    GLint ia, ub, hf, integer = -1;
    if (!p) {
        check(0, "vertex formats: es3int links");
        return;
    }
    ia = glGetAttribLocation(p, "ia");
    ub = glGetAttribLocation(p, "ub");
    hf = glGetAttribLocation(p, "hf");
    glVertexAttribIPointer((GLuint)ia, 2, GL_INT, 0, ints);
    glEnableVertexAttribArray((GLuint)ia);
    glVertexAttribI4ui((GLuint)ub, 0, 200, 0, 0);
    glVertexAttribPointer((GLuint)hf, 3, GL_HALF_FLOAT, GL_FALSE, 0, halves);
    glEnableVertexAttribArray((GLuint)hf);
    glGetVertexAttribiv((GLuint)ia, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &integer);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(integer == 1 && pixel_is(32, 32, 100, 200, 128, 255, 1),
          "vertex formats: GL_INT integers (w the integer 1), an unsigned current value, three half floats");
    glVertexAttribIPointer((GLuint)ia, 2, GL_SHORT, 0, shorts);
    glVertexAttribPointer((GLuint)hf, 1, GL_UNSIGNED_INT, GL_TRUE, 0, uints);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 50, 200, 64, 255, 1), "vertex formats: GL_SHORT integers, normalized unsigned ints");
    glVertexAttribPointer((GLuint)hf, 4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_TRUE, 0, packed);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 50, 200, 255, 255, 1) && glGetError() == GL_NO_ERROR,
          "vertex formats: a normalized GL_UNSIGNED_INT_2_10_10_10_REV word");
    glVertexAttribPointer((GLuint)hf, 3, GL_INT_2_10_10_10_REV, GL_TRUE, 0, packed);
    check(glGetError() == GL_INVALID_OPERATION, "vertex formats: a packed type of 3 components is GL_INVALID_OPERATION");
    glDisableVertexAttribArray((GLuint)ia);
    glDisableVertexAttribArray((GLuint)hf);
    glVertexAttrib4f((GLuint)ub, 0, 0, 0, 1);
    glDeleteProgram(p);
}

/* ES 3.0's fences: one after a draw is unsignalled until the job has
 * rendered (glClientWaitSync flushes it and waits); one with nothing
 * outstanding is signalled at once. */
static void test_fence(void)
{
    GLuint p = program("es3block");
    GLsync f;
    GLint st = -1, n = -1;
    GLenum r;
    if (!p) {
        check(0, "fences: es3block links");
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glGetSynciv(f, GL_SYNC_STATUS, 1, &n, &st);
    check(f && glIsSync(f) && st == GL_UNSIGNALED && n == 1 &&
              glClientWaitSync(f, 0, 0) == GL_TIMEOUT_EXPIRED,
          "fences: unsignalled after a draw, no wait without a timeout");
    r = glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
    glGetSynciv(f, GL_SYNC_STATUS, 1, &n, &st);
    check(r == GL_CONDITION_SATISFIED && st == GL_SIGNALED && glClientWaitSync(f, 0, 0) == GL_ALREADY_SIGNALED &&
              pixel_is(32, 32, 64, 128, 191, 255, 1),
          "fences: glClientWaitSync flushes and waits; signalled after");
    glDeleteSync(f);
    f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    check(!glIsSync((GLsync)&st) && glClientWaitSync(f, 0, 0) == GL_ALREADY_SIGNALED && glGetError() == GL_NO_ERROR,
          "fences: nothing outstanding, signalled at once");
    glDeleteSync(f);
    glDeleteSync((GLsync)&st);
    check(glGetError() == GL_INVALID_VALUE, "fences: deleting a sync that is none is GL_INVALID_VALUE");
    glDeleteProgram(p);
}

/* ES 3.0's occlusion queries: a draw over the picture passes samples, one
 * of a triangle off the picture none; GL_CURRENT_QUERY while active. */
static void test_query(void)
{
    static const GLfloat off[6] = { 2, 2, 3, 2, 2, 3 };
    GLuint p = program("es3block"), q[2], r0 = 7, r1 = 7, av = 0;
    GLint cur = -1, pos;
    if (!p) {
        check(0, "occlusion queries: es3block links");
        return;
    }
    glGenQueries(2, q);
    glClear(GL_COLOR_BUFFER_BIT);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, q[0]);
    glGetQueryiv(GL_ANY_SAMPLES_PASSED, GL_CURRENT_QUERY, &cur);
    draw_cover(p);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, q[1]);
    pos = glGetAttribLocation(p, "pos");
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, off);
    glEnableVertexAttribArray((GLuint)pos);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray((GLuint)pos);
    glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
    glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &r0);
    glGetQueryObjectuiv(q[1], GL_QUERY_RESULT, &r1);
    glGetQueryObjectuiv(q[1], GL_QUERY_RESULT_AVAILABLE, &av);
    check(cur == (GLint)q[0] && r0 == GL_TRUE && r1 == GL_FALSE && av == GL_TRUE && glIsQuery(q[0]) &&
              glGetError() == GL_NO_ERROR,
          "occlusion queries: a draw passes samples, a triangle off the picture none");
    glBeginQuery(GL_ANY_SAMPLES_PASSED, q[0]);
    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, q[1]);
    check(glGetError() == GL_INVALID_OPERATION, "occlusion queries: one active at a time");
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    /* An empty scissor box draws nothing (nor counts). */
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 0, 0);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, q[0]);
    draw_cover(p);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    glDisable(GL_SCISSOR_TEST);
    r1 = 7;
    glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &r1);
    check(pixel_is(32, 32, 0, 0, 0, 0, 0) && pixel_is(0, 0, 0, 0, 0, 0, 0) && r1 == GL_FALSE,
          "an empty scissor box: nothing drawn, no samples");
    glDeleteQueries(2, q);
    glDeleteProgram(p);
}

/* ES 3.0's immutable storage: two levels of a 2x2 GL_RGBA8 texture given
 * by sub-images, sampled (es3wide) at base level 0, 1 and 5, which ES 3.0
 * clamps to the last level. */
static void test_storage(void)
{
    static const GLubyte red[16] = { 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255 };
    static const GLubyte green[4] = { 0, 255, 0, 255 };
    GLuint p = program("es3wide"), t;
    if (!p) {
        check(0, "immutable storage: es3wide links");
        return;
    }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 2, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 255, 0, 0, 255, 0), "immutable storage: level 0 (red)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 0, 255, 0, 255, 0), "immutable storage: base level 1 (green)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 5);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 0, 255, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "immutable storage: base level 5 clamped to the last (green)");
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
}

/* Transform feedback (stage C item 1) with es3xfb: each vertex's v_twice,
 * v_pos and v_id, 6 words, read back by mapping the buffer. One triangle
 * drawn and fed back, the primitives-written query 1; then with
 * GL_RASTERIZER_DISCARD (nothing drawn) two triangles around a pause and a
 * draw that does not feed back, the second appended after the first. */
static int xfb_vertex_is(const GLfloat *w, GLfloat x, GLfloat y, GLfloat a, int id)
{
    GLint idw;
    memcpy(&idw, &w[5], 4);
    if (w[0] == a * 2 && w[1] == x * 2 && w[2] == y * 2 && w[3] == a * 2 && w[4] == 2 && idw == id)
        return 1;
    printf("     fed back %g, (%g %g %g %g), %ld; not %g, (%g %g %g 2), %d\n", w[0], w[1], w[2], w[3], w[4],
           (long)idw, a * 2, x * 2, y * 2, a * 2, id);
    return 0;
}

static void test_xfb(void)
{
    static const char *const names[3] = { "v_twice", "v_pos", "v_id" };
    static const GLfloat pos[12] = { -1, -1, 3, -1, -1, 3, -0.5f, -0.5f, 0.25f, -0.5f, -0.5f, 0.25f };
    static const GLfloat attr[6] = { 0.25f, 0.5f, 0.125f, 1, 2, 3 };
    static GLubyte zeros[256];
    GLuint p, b, q, prims = 0;
    GLint lp, la, k, ok = 1;
    const GLfloat *w;
    xfb_names = names;
    xfb_count = 3;
    p = program("es3xfb");
    xfb_names = 0;
    xfb_count = 0;
    if (!p) {
        check(0, "transform feedback: es3xfb links with its varyings");
        return;
    }
    lp = glGetAttribLocation(p, "pos");
    la = glGetAttribLocation(p, "a");
    glUseProgram(p);
    glVertexAttribPointer((GLuint)lp, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer((GLuint)la, 1, GL_FLOAT, GL_FALSE, 0, attr);
    glEnableVertexAttribArray((GLuint)lp);
    glEnableVertexAttribArray((GLuint)la);
    glGenBuffers(1, &b);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, b);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(zeros), zeros, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
    glGenQueries(1, &q);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
    glBeginTransformFeedback(GL_TRIANGLES);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &prims);
    check(glGetError() == GL_NO_ERROR && prims == 1, "transform feedback: a triangle drawn, 1 primitive written");
    w = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 72, GL_MAP_READ_BIT);
    for (k = 0; w && k < 3; k++)
        ok &= xfb_vertex_is(w + 6 * k, pos[2 * k], pos[2 * k + 1], attr[k], k);
    check(w && ok, "transform feedback: each vertex's v_twice, v_pos and v_id in the buffer");
    glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);

    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_TRIANGLES);
    glDrawArrays(GL_TRIANGLES, 3, 3);
    glPauseTransformFeedback();
    glDrawArrays(GL_TRIANGLES, 0, 3); /* not fed back */
    glResumeTransformFeedback();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    check(glGetError() == GL_NO_ERROR && pixel_is(32, 32, 0, 0, 255, 255, 0),
          "transform feedback: GL_RASTERIZER_DISCARD draws nothing");
    w = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 144, GL_MAP_READ_BIT);
    ok = 1;
    for (k = 0; w && k < 3; k++)
        ok &= xfb_vertex_is(w + 6 * k, pos[6 + 2 * k], pos[7 + 2 * k], attr[3 + k], 3 + k);
    for (k = 0; w && k < 3; k++)
        ok &= xfb_vertex_is(w + 18 + 6 * k, pos[2 * k], pos[2 * k + 1], attr[k], k);
    check(w && ok, "transform feedback: two triangles around a pause, the second appended");
    glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);

    glDisableVertexAttribArray((GLuint)lp);
    glDisableVertexAttribArray((GLuint)la);
    glClearColor(0, 0, 0, 0);
    glUseProgram(0);
    glDeleteQueries(1, &q);
    glDeleteBuffers(1, &b);
    glDeleteProgram(p);
}

/* The colour es3wide samples from the bound 2D texture at (32, 32). */
static int sampled_is(GLuint p, int r, int g, int b)
{
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    return pixel_is(32, 32, r, g, b, 255, 0);
}

/* ES 3.0 3.8.13: a mutable texture's chain is measured from its base
 * level, whatever level 0 is (es3wide, es3size): a texture with levels 2
 * and 3 only, then a level 0 not of their chain; glGenerateMipmap from base
 * level 1, level 0 untouched. */
static void test_base_levels(void)
{
    static GLubyte red[16 * 16 * 4], green[8 * 8 * 4];
    static const GLubyte blue[4] = { 0, 0, 255, 255 };
    GLuint p = program("es3wide"), ps = program("es3size"), t;
    int i;
    if (!p || !ps) {
        check(0, "base levels: es3wide and es3size link");
        return;
    }
    for (i = 0; i < 16 * 16; i++) {
        red[i * 4] = 255;
        red[i * 4 + 3] = 255;
    }
    for (i = 0; i < 8 * 8; i++) {
        green[i * 4 + 1] = 255;
        green[i * 4 + 3] = 255;
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glUseProgram(ps);
    glUniform1i(glGetUniformLocation(ps, "tex"), 0);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 2);
    check(sampled_is(p, 0, 255, 0), "base levels: level 2 alone, base level 2 (green)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    check(sampled_is(p, 0, 0, 0), "base levels: mipmapped without level 3: incomplete (black)");
    glTexImage2D(GL_TEXTURE_2D, 3, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
    check(sampled_is(p, 0, 255, 0), "base levels: levels 2 and 3, mipmapped (green)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 3);
    check(sampled_is(p, 0, 0, 255), "base levels: base level 3 (blue)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    check(sampled_is(p, 0, 255, 0), "base levels: a level 0 not of their chain, base level 2 (green)");
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(ps);
    check(pixel_is(20, 20, 2, 2, 0, 255, 0), "base levels: textureSize there (2x2)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    check(sampled_is(p, 255, 0, 0), "base levels: base level 0 (red)");
    glDeleteTextures(1, &t);

    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
    glGenerateMipmap(GL_TEXTURE_2D);
    check(glGetError() == GL_NO_ERROR, "base levels: glGenerateMipmap from base level 1");
    check(sampled_is(p, 0, 255, 0), "base levels: its chain from level 1 (green)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 4);
    check(sampled_is(p, 0, 255, 0), "base levels: its level 4, 1x1 (green)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    check(sampled_is(p, 255, 0, 0), "base levels: level 0 untouched (red)");
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
    glDeleteProgram(ps);
}

/* ES 3.0's R and RG formats sampled (es3wide): (r, 0, 0, 1) and (r, g, 0, 1). */
static int one_texel_is(GLuint p, GLenum ifmt, GLenum format, GLenum type, const void *texel, int r, int g)
{
    GLuint t;
    int ok;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, 1, 1, 0, format, type, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    ok = pixel_is(32, 32, r, g, 0, 255, 1);
    glDeleteTextures(1, &t);
    return ok;
}

static void test_rg(void)
{
    static const GLubyte r8[1] = { 200 }, rg8[2] = { 10, 20 };
    static const GLushort r16f[1] = { 0x3800 }; /* 0.5 */
    static const GLfloat rg32f[2] = { 0.25f, 0.75f };
    GLuint p = program("es3wide");
    if (!p) {
        check(0, "R and RG formats: es3wide links");
        return;
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    check(one_texel_is(p, GL_R8, GL_RED, GL_UNSIGNED_BYTE, r8, 200, 0) &&
              one_texel_is(p, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, rg8, 10, 20),
          "R and RG formats: GL_R8 (200, 0, 0, 1), GL_RG8 (10, 20, 0, 1)");
    check(one_texel_is(p, GL_R16F, GL_RED, GL_HALF_FLOAT, r16f, 128, 0) &&
              one_texel_is(p, GL_RG32F, GL_RG, GL_FLOAT, rg32f, 64, 191) && glGetError() == GL_NO_ERROR,
          "R and RG formats: GL_R16F, GL_RG32F");
    glDeleteProgram(p);
}

/* A 1x1 texture of `ifmt` from (format, type, texel) sampled (es3wide, bound): (r, g, b, a) within tol. */
static int texel_is(GLuint p, GLenum ifmt, GLenum format, GLenum type, const void *texel, int r, int g, int b, int a,
                    int tol)
{
    GLuint t;
    int ok;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, 1, 1, 0, format, type, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    ok = pixel_is(32, 32, r, g, b, a, tol) && glGetError() == GL_NO_ERROR;
    glDeleteTextures(1, &t);
    return ok;
}

/* ES 3.0's signed normalized, sRGB, packed and 32F depth formats. */
static void test_formats3(void)
{
    static const GLbyte snorm[4] = { 127, 64, -127, 127 };
    static const GLubyte srgb[4] = { 188, 188, 188, 255 };          /* about 0.5 linear */
    static const GLuint rgb10a2[1] = { 1023UL | (512UL << 10) | (3UL << 30) };
    static const GLfloat flt3[3] = { 0.5f, 0.25f, 0.75f };
    static const GLfloat depth[1] = { 0.5f };
    GLuint p = program("es3wide");
    if (!p) {
        check(0, "more formats: es3wide links");
        return;
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    check(texel_is(p, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, snorm, 255, 129, 0, 255, 2),
          "more formats: GL_RGBA8_SNORM (1, 0.5, -1 clamped by the target, 1)");
    check(texel_is(p, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, srgb, 128, 128, 128, 255, 2),
          "more formats: GL_SRGB8_ALPHA8 decoded (188 to 0.5)");
    check(texel_is(p, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, rgb10a2, 255, 128, 0, 255, 1),
          "more formats: GL_RGB10_A2");
    check(texel_is(p, GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, flt3, 128, 64, 191, 255, 1) &&
              texel_is(p, GL_RGB9_E5, GL_RGB, GL_FLOAT, flt3, 128, 64, 191, 255, 1),
          "more formats: GL_R11F_G11F_B10F and GL_RGB9_E5 from floats");
    check(texel_is(p, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, depth, 128, 0, 0, 255, 1),
          "more formats: GL_DEPTH_COMPONENT32F as (d, 0, 0, 1)");
    glDeleteProgram(p);
}

/* A 4x4 texture of one compressed block sampled (es3wide, bound): (r, g, b, a) within tol. */
static int block_is(GLuint p, GLenum ifmt, const GLubyte *block, int bytes, int r, int g, int b, int a, int tol)
{
    GLuint t;
    int ok;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, ifmt, 4, 4, 0, bytes, block);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    ok = pixel_is(32, 32, r, g, b, a, tol) && glGetError() == GL_NO_ERROR;
    glDeleteTextures(1, &t);
    return ok;
}

/* ES 3.0's ETC2/EAC formats sampled by the TMU (stage B item 9), a solid
 * block each: an individual-mode block (4-bit colours, table 0, every index
 * +2) as ETC1, RGB8 and sRGB; punch-through opaque and transparent; R11 and
 * RG11 (base*8+4 + 2), signed R11 (base*8 + 2 of 1023); RGBA8 with an EAC
 * alpha (base 128 + 2). A 2D array of two ETC2 layers, one replaced. */
static void test_etc2(void)
{
    static const GLubyte red[8] = { 0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0 };       /* (255, 2, 2) */
    static const GLubyte grey[8] = { 0x88, 0x88, 0x88, 0x00, 0, 0, 0, 0 };      /* (138, 138, 138) */
    static const GLubyte opaque[8] = { 0xF8, 0x00, 0x00, 0x02, 0, 0, 0, 0 };    /* differential, opaque bit */
    static const GLubyte clear[8] = { 0xF8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0, 0 }; /* index 2: transparent */
    static const GLubyte r11[8] = { 0x80, 0x00, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24 };   /* 1030 of 2047 */
    static const GLubyte rg11[16] = { 0x80, 0x00, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24,
                                      0x40, 0x00, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24 }; /* 1030, 518 */
    static const GLubyte sr11[8] = { 0x40, 0x00, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24 };  /* 514 of 1023 */
    static const GLubyte rgba[16] = { 0x80, 0x10, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24,
                                      0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0 };             /* alpha 130 */
    GLubyte layers[16];
    GLuint p = program("es3wide"), pa = program("es3layer"), t;
    GLint n = 0;
    if (!p || !pa) {
        check(0, "ETC2/EAC: es3wide and es3layer link");
        return;
    }
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    check(n == 15, "ETC2/EAC: 15 compressed formats listed");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    check(block_is(p, GL_ETC1_RGB8_OES, red, 8, 255, 2, 2, 255, 1) &&
              block_is(p, GL_COMPRESSED_RGB8_ETC2, red, 8, 255, 2, 2, 255, 1),
          "ETC2/EAC: ETC1 and GL_COMPRESSED_RGB8_ETC2 (255, 2, 2)");
    check(block_is(p, GL_COMPRESSED_SRGB8_ETC2, grey, 8, 65, 65, 65, 255, 2),
          "ETC2/EAC: GL_COMPRESSED_SRGB8_ETC2 decoded (138 to 65)");
    check(block_is(p, GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, opaque, 8, 255, 2, 2, 255, 1) &&
              block_is(p, GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, clear, 8, 0, 0, 0, 0, 0),
          "ETC2/EAC: punch-through opaque and transparent");
    check(block_is(p, GL_COMPRESSED_R11_EAC, r11, 8, 128, 0, 0, 255, 1) &&
              block_is(p, GL_COMPRESSED_RG11_EAC, rg11, 16, 128, 64, 0, 255, 1) &&
              block_is(p, GL_COMPRESSED_SIGNED_R11_EAC, sr11, 8, 128, 0, 0, 255, 1),
          "ETC2/EAC: R11, RG11 and signed R11");
    check(block_is(p, GL_COMPRESSED_RGBA8_ETC2_EAC, rgba, 16, 255, 2, 2, 130, 1),
          "ETC2/EAC: GL_COMPRESSED_RGBA8_ETC2_EAC (alpha 130)");

    memcpy(layers, red, 8);
    memcpy(layers + 8, grey, 8);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RGB8_ETC2, 4, 4, 2, 0, 16, layers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(pa);
    glUniform1i(glGetUniformLocation(pa, "tex"), 0);
    check(layer_is(pa, "layer", 0.0f, 255, 2, 2, 1) && layer_is(pa, "layer", 1.0f, 138, 138, 138, 1),
          "ETC2/EAC: a compressed 2D array's layers 0 and 1");
    glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 4, 4, 1, GL_COMPRESSED_RGB8_ETC2, 8, red);
    check(layer_is(pa, "layer", 1.0f, 255, 2, 2, 1) && glGetError() == GL_NO_ERROR,
          "ETC2/EAC: layer 1 replaced by glCompressedTexSubImage3D");
    glCompressedTexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RGB8_ETC2, 4, 4, 1, 0, 8, red);
    check(glGetError() == GL_INVALID_OPERATION, "ETC2/EAC: a compressed 3D texture is GL_INVALID_OPERATION");
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
    glDeleteProgram(pa);
}

/* ---- render targets beyond 8 bits (GLES3 plan stage B item 10) ---- */

static GLuint storage_texture(GLenum ifmt)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, 16, 16);
    return t;
}

/* The read buffer's pixel (x, y) as RGBA floats is want (within tol). */
static int floats_are(int x, int y, const GLfloat *want, GLfloat tol)
{
    GLfloat px[4];
    int k;
    px[0] = px[1] = px[2] = px[3] = -99.0f;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, px);
    for (k = 0; k < 4; k++)
        if (px[k] - want[k] > tol || want[k] - px[k] > tol) { /* in thousandths: libnix prints no floats */
            printf("     pixel (%d, %d) is %ld %ld %ld %ld, not %ld %ld %ld %ld (x 1/1000)\n", x, y,
                   (long)(px[0] * 1000), (long)(px[1] * 1000), (long)(px[2] * 1000), (long)(px[3] * 1000),
                   (long)(want[0] * 1000), (long)(want[1] * 1000), (long)(want[2] * 1000), (long)(want[3] * 1000));
            return 0;
        }
    return 1;
}

/* The read buffer's pixel (x, y) as RGBA_INTEGER of `type` is want. */
static int ints_are(int x, int y, GLenum type, const GLint *want)
{
    GLint px[4];
    int k;
    px[0] = px[1] = px[2] = px[3] = 99;
    glReadPixels(x, y, 1, 1, GL_RGBA_INTEGER, type, px);
    for (k = 0; k < 4; k++)
        if (px[k] != want[k]) {
            printf("     pixel (%d, %d) is %ld %ld %ld %ld, not %ld %ld %ld %ld\n", x, y, (long)px[0], (long)px[1],
                   (long)px[2], (long)px[3], (long)want[0], (long)want[1], (long)want[2], (long)want[3]);
            return 0;
        }
    return 1;
}

static void rt_uniforms(GLuint p, const GLfloat *f, const GLint *i, const GLuint *u, GLfloat z)
{
    glUseProgram(p);
    glUniform4fv(glGetUniformLocation(p, "f"), 1, f);
    glUniform4iv(glGetUniformLocation(p, "i"), 1, i);
    glUniform4uiv(glGetUniformLocation(p, "u"), 1, u);
    glUniform1f(glGetUniformLocation(p, "z"), z);
}

/* An RGBA32F, an RGBA8I and an R32UI target at once (es3rt's three
 * outputs: 32-bit writes, software blending's key, 128-bit 16x16 tiles),
 * read as ES 3.0's RGBA/FLOAT and RGBA_INTEGER; each cleared by
 * glClearBuffer* in the tile buffer, one confined by the scissor box (the
 * CPU's fill); RGBA32F blended in the shader, the integer target not. */
static void test_rt_wide(void)
{
    static const GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    static const GLfloat f[4] = { 0.25f, -2.0f, 100.5f, 0.5f }, cf[4] = { 1.5f, 2.5f, -3.0f, 4.0f };
    static const GLfloat sf[4] = { 7.0f, 7.0f, 7.0f, 7.0f }, bf[4] = { 1.75f, 0.5f, 97.5f, 4.5f };
    static const GLint iv[4] = { -5, 7, -128, 127 }, ci[4] = { -1, -2, -3, -4 };
    static const GLuint uv[4] = { 4000000000u, 1, 2, 3 }, cu[4] = { 9, 8, 7, 6 };
    static const GLint ru[4] = { (GLint)4000000000u, 0, 0, 1 }, rcu[4] = { 9, 0, 0, 1 };
    GLuint p = program("es3rt"), fb, t[2], rb;
    GLint fmt = 0, type = 0;
    if (!p) {
        check(0, "render targets: es3rt links");
        return;
    }
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    t[0] = storage_texture(GL_RGBA32F);
    t[1] = storage_texture(GL_RGBA8I);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_R32UI, 16, 16);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t[1], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_RENDERBUFFER, rb);
    glDrawBuffers(3, bufs);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR,
          "render targets: RGBA32F, RGBA8I and R32UI complete");
    glViewport(0, 0, 16, 16);
    rt_uniforms(p, f, iv, uv, 0.0f);
    draw_cover(p);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    check(floats_are(8, 8, f, 0.0f) && type == GL_FLOAT, "render targets: RGBA32F drawn, read as RGBA/FLOAT");
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
    check(ints_are(8, 8, GL_INT, iv) && fmt == GL_RGBA_INTEGER, "render targets: RGBA8I drawn, read as RGBA_INTEGER");
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    check(ints_are(8, 8, GL_UNSIGNED_INT, ru), "render targets: R32UI drawn (4000000000, 0, 0, 1)");

    glClearBufferfv(GL_COLOR, 0, cf);
    glClearBufferiv(GL_COLOR, 1, ci);
    glClearBufferuiv(GL_COLOR, 2, cu);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    check(floats_are(8, 8, cf, 0.0f) && glGetError() == GL_NO_ERROR, "glClearBufferfv: RGBA32F");
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    check(ints_are(8, 8, GL_INT, ci), "glClearBufferiv: RGBA8I");
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    check(ints_are(8, 8, GL_UNSIGNED_INT, rcu), "glClearBufferuiv: R32UI");
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 4, 4);
    glClearBufferfv(GL_COLOR, 0, sf);
    glDisable(GL_SCISSOR_TEST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    check(floats_are(2, 2, sf, 0.0f) && floats_are(8, 8, cf, 0.0f), "glClearBufferfv in the scissor box: RGBA32F");

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    draw_cover(p);
    glDisable(GL_BLEND);
    check(floats_are(8, 8, bf, 0.0f), "software blending: RGBA32F, ONE + ONE");
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    check(ints_are(8, 8, GL_INT, iv) && glGetError() == GL_NO_ERROR, "software blending: RGBA8I not blended");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(2, t);
    glDeleteRenderbuffers(1, &rb);
    glDeleteProgram(p);
}

/* One target of each 8-bit and 16F kind: (0.5, 0.25, 0.75, 1) drawn and
 * read back; sRGB stored encoded; R11F_G11F_B10F and R16F read as floats.
 * Then RG8 blended with GL_DST_ALPHA: a target without alpha reads 1. */
static int one_rt_is(GLuint p, GLenum ifmt, const GLubyte *want, const GLfloat *wantf)
{
    static const GLfloat f[4] = { 0.5f, 0.25f, 0.75f, 1.0f };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint fb, t = storage_texture(ifmt);
    int ok;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glViewport(0, 0, 16, 16);
    rt_uniforms(p, f, iv, uv, 0.0f);
    draw_cover(p);
    ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
         (want ? pixel_is(8, 8, want[0], want[1], want[2], want[3], 1) : floats_are(8, 8, wantf, 0.0f));
    if (!ok)
        printf("     (format 0x%04lx)\n", (unsigned long)ifmt);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &t);
    return ok;
}

static void test_rt_formats(void)
{
    static const GLubyte r8[4] = { 128, 0, 0, 255 }, rg8[4] = { 128, 64, 0, 255 };
    static const GLubyte srgb[4] = { 188, 137, 225, 255 }, rgb10a2[4] = { 128, 64, 191, 255 };
    static const GLfloat r11[4] = { 0.5f, 0.25f, 0.75f, 1.0f }, r16[4] = { 0.5f, 0.0f, 0.0f, 1.0f };
    static const GLfloat f[4] = { 0.5f, 0.25f, 0.75f, 1.0f }, clear0[4] = { 0, 0, 0, 0 };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint p = program("es3rt"), fb, t;
    if (!p) {
        check(0, "render target formats: es3rt links");
        return;
    }
    check(one_rt_is(p, GL_R8, r8, 0) && one_rt_is(p, GL_RG8, rg8, 0), "render targets: R8 and RG8");
    check(one_rt_is(p, GL_SRGB8_ALPHA8, srgb, 0), "render targets: SRGB8_ALPHA8 stored encoded");
    check(one_rt_is(p, GL_RGB10_A2, rgb10a2, 0), "render targets: RGB10_A2");
    check(one_rt_is(p, GL_R11F_G11F_B10F, 0, r11) && one_rt_is(p, GL_R16F, 0, r16) && glGetError() == GL_NO_ERROR,
          "render targets: R11F_G11F_B10F and R16F read as floats");

    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    t = storage_texture(GL_RG8);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glViewport(0, 0, 16, 16);
    glClearBufferfv(GL_COLOR, 0, clear0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_ALPHA, GL_ZERO);
    rt_uniforms(p, f, iv, uv, 0.0f);
    draw_cover(p);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    check(pixel_is(8, 8, 128, 64, 0, 255, 1) && glGetError() == GL_NO_ERROR,
          "blending: RG8 reads a destination alpha of 1");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
}

/* DEPTH32F_STENCIL8 as depth and stencil (32F depth in the tile buffer, its
 * stencil an S8 image of its own): cleared by glClearBufferfi to 0.5 and 3;
 * green nearer passes both tests, red farther fails the depth test, blue
 * with another reference fails the stencil test. */
static void test_rt_depth32f(void)
{
    static const GLfloat black[4] = { 0, 0, 0, 1 };
    static const GLfloat green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint p = program("es3rt"), fb, rb[2];
    GLint ct = 0;
    if (!p) {
        check(0, "32F depth: es3rt links");
        return;
    }
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &ct);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && ct == GL_FLOAT &&
              glGetError() == GL_NO_ERROR,
          "32F depth: DEPTH32F_STENCIL8 complete, its depth GL_FLOAT");
    glViewport(0, 0, 16, 16);
    glClearBufferfv(GL_COLOR, 0, black);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.5f, 3);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 3, 0xff);
    rt_uniforms(p, green, iv, uv, -0.5f);
    draw_cover(p);
    rt_uniforms(p, red, iv, uv, 0.5f);
    draw_cover(p);
    glStencilFunc(GL_EQUAL, 4, 0xff);
    rt_uniforms(p, blue, iv, uv, -0.9f);
    draw_cover(p);
    glDisable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    check(pixel_is(8, 8, 0, 255, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "32F depth: depth and stencil tests against glClearBufferfi's 0.5 and 3");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(2, rb);
    glDeleteProgram(p);
}

/* A DEPTH32F_STENCIL8 texture as depth and stencil: its stencil (an image
 * of its own) is kept across a read-back, and as the stencil only. */
static void test_depth32f_texture(void)
{
    static const GLfloat black[4] = { 0, 0, 0, 1 };
    static const GLfloat green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint p = program("es3rt"), fb, t[2];
    int ok;
    if (!p) {
        check(0, "32F depth texture: es3rt links");
        return;
    }
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(2, t);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, 16, 16);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t[1], 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR,
          "32F depth texture: DEPTH32F_STENCIL8 as depth and stencil is complete");
    glViewport(0, 0, 16, 16);
    glClearBufferfv(GL_COLOR, 0, black);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.5f, 3);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 3, 0xff);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 8, 16); /* left: stencil 3 becomes 4 */
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    rt_uniforms(p, red, iv, uv, 0.0f);
    draw_cover(p);
    glDisable(GL_SCISSOR_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    ok = pixel_is(4, 8, 255, 0, 0, 255, 0) && pixel_is(12, 8, 0, 0, 0, 255, 0);
    glStencilFunc(GL_EQUAL, 4, 0xff); /* after the read-back: only the left passes */
    rt_uniforms(p, green, iv, uv, 0.0f);
    draw_cover(p);
    ok = ok && pixel_is(4, 8, 0, 255, 0, 255, 0) && pixel_is(12, 8, 0, 0, 0, 255, 0);
    check(ok && glGetError() == GL_NO_ERROR, "32F depth texture: its stencil kept across a read-back");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t[1], 0);
    glStencilFunc(GL_EQUAL, 3, 0xff); /* the stencil only: the right side's 3 */
    rt_uniforms(p, red, iv, uv, 0.0f);
    draw_cover(p);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && pixel_is(4, 8, 0, 255, 0, 255, 0) &&
              pixel_is(12, 8, 255, 0, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "32F depth texture: as the stencil attachment only");
    glDisable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(2, t);
    glDeleteProgram(p);
}

/* glCopyTexSubImage3D: the default framebuffer's colour into a layer of a
 * 2D array and a slice of a 3D texture, read back through
 * glFramebufferTextureLayer; the others untouched. */
static void test_copy3d(void)
{
    static const GLenum targets[2] = { GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D };
    GLuint t[2], fb;
    int k, ok = 1;
    glGenTextures(2, t);
    glGenFramebuffers(1, &fb);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    for (k = 0; k < 2; k++) {
        glBindTexture(targets[k], t[k]);
        glTexImage3D(targets[k], 0, GL_RGBA8, 8, 8, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glCopyTexSubImage3D(targets[k], 0, 2, 2, k + 1, 0, 0, 4, 4);
        ok = ok && glGetError() == GL_NO_ERROR;
    }
    glCopyTexSubImage3D(GL_TEXTURE_3D, 0, 6, 0, 0, 0, 0, 4, 4);
    ok = ok && glGetError() == GL_INVALID_VALUE;
    glCopyTexSubImage3D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 0, 4, 4);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    for (k = 0; k < 2; k++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t[k], 0, k + 1);
        ok = ok && pixel_is(3, 3, 0, 0, 255, 255, 0) && pixel_is(1, 1, 0, 0, 0, 0, 0);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t[k], 0, 0);
        ok = ok && pixel_is(3, 3, 0, 0, 0, 0, 0);
    }
    check(ok && glGetError() == GL_NO_ERROR, "glCopyTexSubImage3D: into a layer and a slice, errors");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(2, t);
}

/* OES_vertex_array_object and EXT_instanced_arrays through their own
 * entries, with a GLSL ES 1.00 program: two instances of one triangle,
 * the colour an instanced attribute's (the last instance's shows). */
static void test_instanced_ext(void)
{
    static const GLfloat col[8] = { 1, 0, 0, 1, 0, 1, 0, 1 }; /* red, green */
    static const GLushort idx[3] = { 0, 1, 2 };
    GLuint p = program("inst100"), a = 0, bu[3];
    GLint pos, c;
    GLenum e;
    int ok;
    if (!p) {
        check(0, "EXT_instanced_arrays: inst100 links");
        return;
    }
    pos = glGetAttribLocation(p, "pos");
    c = glGetAttribLocation(p, "col");
    glGenVertexArraysOES(1, &a);
    glBindVertexArrayOES(a);
    glUseProgram(p);
    /* buffers: an object but 0 takes no client arrays */
    glGenBuffers(3, bu);
    glBindBuffer(GL_ARRAY_BUFFER, bu[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cover), cover, GL_STATIC_DRAW);
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray((GLuint)pos);
    glBindBuffer(GL_ARRAY_BUFFER, bu[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(col), col, GL_STATIC_DRAW);
    glVertexAttribPointer((GLuint)c, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray((GLuint)c);
    glVertexAttribDivisorEXT((GLuint)c, 1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bu[2]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysInstancedEXT(GL_TRIANGLES, 0, 3, 2);
    ok = pixel_is(30, 30, 0, 255, 0, 255, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsInstancedEXT(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0, 1); /* one: the first colour */
    ok = ok && pixel_is(30, 30, 255, 0, 0, 255, 0);
    glBindVertexArrayOES(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteVertexArraysOES(1, &a);
    glDeleteBuffers(3, bu);
    e = glGetError(); /* read whatever ok says, so no error is left for the next test */
    check(ok && e == GL_NO_ERROR,
          "EXT_instanced_arrays: an instanced colour in an OES_vertex_array_object object, both draws");
    glDeleteProgram(p);
}

/* dEQP-GLES3 state_query.fbo: the sizes and component type of attachments
 * without storage are 0 and GL_NONE; GL_DEPTH_STENCIL_ATTACHMENT answers
 * both sizes of a depth and stencil renderbuffer. */
static void test_attachment_queries(void)
{
    GLuint fb, rb, tx;
    GLint r = -1, ct = -1, tr = -1, d = -1, s = -1;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glGenTextures(1, &tx);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &ct);
    glBindTexture(GL_TEXTURE_2D, tx);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tx, 0);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &tr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &d);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &s);
    check(r == 0 && ct == GL_NONE && tr == 0 && d == 24 && s == 8 && glGetError() == GL_NO_ERROR,
          "attachment queries: no storage 0 and GL_NONE; depth and stencil 24 and 8");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &rb);
    glDeleteTextures(1, &tx);
}

/* EXT_frag_depth: a GLSL ES 1.00 program's gl_FragDepthEXT is tested
 * against the cleared 0.5 (its triangle's own depth is 0.5 too), in an
 * object with a depth renderbuffer (the context has none). */
static void test_frag_depth(void)
{
    GLuint p = program("fragdepth"), fb, rb[2];
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    int ok;
    if (!p) {
        check(0, "EXT_frag_depth: fragdepth links");
        return;
    }
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(2, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glClearDepthf(0.5f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "c"), 0.0f, 1.0f, 0.0f, 1.0f);
    glUniform1f(glGetUniformLocation(p, "z"), 0.25f);
    draw_cover(p);
    glUniform4f(glGetUniformLocation(p, "c"), 1.0f, 0.0f, 0.0f, 1.0f);
    glUniform1f(glGetUniformLocation(p, "z"), 0.75f);
    draw_cover(p);
    ok = pixel_is(30, 30, 0, 255, 0, 255, 0);
    glDisable(GL_DEPTH_TEST);
    glClearDepthf(1.0f);
    check(ok && ext && strstr(ext, "GL_EXT_frag_depth") && glGetError() == GL_NO_ERROR,
          "EXT_frag_depth: gl_FragDepthEXT tested (0.25 passes 0.5, 0.75 does not)");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(2, rb);
    glDeleteProgram(p);
}

/* dEQP-GLES3 fbo.msaa.renderbuffer_resize: a renderbuffer cleared, given
 * new storage while attached, cleared again, blitted into another one a
 * clear made red: the latter is the second clear's green. */
static void test_rb_resize(void)
{
    static const GLfloat red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    static GLubyte px[40 * 40 * 4];
    GLuint fb[2], rb[2];
    GLenum st[3];
    int ok = 1, k;
    glGenFramebuffers(2, fb);
    glGenRenderbuffers(2, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 40, 40);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    st[0] = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glClearBufferfv(GL_COLOR, 0, red);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, 10, 10);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[1]);
    st[1] = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glClearBufferfv(GL_COLOR, 0, blue);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, 40, 40);
    st[2] = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glClearBufferfv(GL_COLOR, 0, green);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]); /* as dEQP: no read-back before the blit */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[0]);
    glBlitFramebuffer(0, 0, 40, 40, 0, 0, 40, 40, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glReadPixels(0, 0, 40, 40, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (k = 0; k < 40 * 40 && ok; k++)
        if (px[4 * k] != 0 || px[4 * k + 1] != 255 || px[4 * k + 2] != 0 || px[4 * k + 3] != 255) {
            printf("     pixel (%d, %d) is %d %d %d %d\n", k % 40, k / 40, px[4 * k], px[4 * k + 1], px[4 * k + 2],
                   px[4 * k + 3]);
            ok = 0;
        }
    check(ok && st[0] == GL_FRAMEBUFFER_COMPLETE && st[1] == GL_FRAMEBUFFER_COMPLETE &&
              st[2] == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR,
          "renderbuffer resize: cleared again and blitted");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    glDeleteRenderbuffers(2, rb);
}

/* An RGBA8 and an R32I target with depth and stencil: partial clears where
 * a 16-bit colour write must not reach the integer target (it upsets the
 * GPU until a power cycle): the stencil through the program that writes no
 * colour, each colour on the CPU; then a draw where the stencil is 1. */
static void test_rt_mixed_clears(void)
{
    static const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    static const GLfloat black[4] = { 0, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 }, green[4] = { 0, 1, 0, 1 };
    static const GLint zero[4] = { 0, 0, 0, 0 }, seven[4] = { 7, 0, 0, 0 }, iv[4] = { -5, 0, 0, 0 };
    static const GLint r_draw[4] = { -5, 0, 0, 1 }, r_seven[4] = { 7, 0, 0, 1 }, r_zero[4] = { 0, 0, 0, 1 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint p = program("es3rt"), fb, t[2], rb;
    if (!p) {
        check(0, "mixed clears: es3rt links");
        return;
    }
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(2, t);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 128, 128);
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32I, 128, 128);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 128, 128);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t[1], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    glDrawBuffers(2, bufs);
    glViewport(0, 0, 128, 128);
    glClearBufferfv(GL_COLOR, 0, black);
    glClearBufferiv(GL_COLOR, 1, zero);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
    rt_uniforms(p, green, iv, uv, 0.0f); /* a draw first: the clears below cannot be the tile buffer's */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 128, 1);
    draw_cover(p);
    glScissor(0, 0, 64, 128);
    glClearStencil(1);
    glClear(GL_STENCIL_BUFFER_BIT);
    glClearStencil(0);
    glScissor(64, 0, 64, 64);
    glClearBufferfv(GL_COLOR, 0, blue);
    glClearBufferiv(GL_COLOR, 1, seven);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 1, 0xff);
    draw_cover(p);
    glDisable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    check(pixel_is(10, 100, 0, 255, 0, 255, 0) && pixel_is(100, 100, 0, 0, 0, 255, 0) &&
              pixel_is(100, 10, 0, 0, 255, 255, 0) && glGetError() == GL_NO_ERROR,
          "mixed clears: RGBA8 beside R32I, stencil cleared in half, colour in a quarter");
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    check(ints_are(10, 100, GL_INT, r_draw) && ints_are(100, 100, GL_INT, r_zero) &&
              ints_are(100, 10, GL_INT, r_seven) && glGetError() == GL_NO_ERROR,
          "mixed clears: the R32I target the same");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(2, t);
    glDeleteRenderbuffers(1, &rb);
    glDeleteProgram(p);
}

/* ES 3.0's framebuffers (GLES3 plan stage B item 11): a separate read
 * binding (glReadPixels from an object while the default framebuffer is
 * drawn into), GL_DEPTH_STENCIL_ATTACHMENT, a 2D array's layer and a 3D
 * texture's slice drawn into and sampled, glInvalidateFramebuffer's checks. */
static void test_framebuffers(void)
{
    static const GLfloat red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    static const GLenum inv[2] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT };
    GLuint pr = program("es3rt"), pa = program("es3layer"), pv = program("es3vol"), fb, rb, t[3];
    GLint rd = -1, dr = -1, type = 0;
    if (!pr || !pa || !pv) {
        check(0, "framebuffers: es3rt, es3layer and es3vol link");
        return;
    }
    glGenFramebuffers(1, &fb);
    glGenTextures(3, t);
    glGenRenderbuffers(1, &rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 16, 16);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && type == GL_RENDERBUFFER &&
              glGetError() == GL_NO_ERROR,
          "framebuffers: GL_DEPTH_STENCIL_ATTACHMENT sets depth and stencil");
    glViewport(0, 0, 16, 16);
    rt_uniforms(pr, red, iv, uv, 0.0f);
    draw_cover(pr);
    /* the object for reading, the default framebuffer for drawing */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    rt_uniforms(pr, green, iv, uv, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(pr);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
    check(rd == (GLint)fb && dr == 0 && pixel_is(8, 8, 255, 0, 0, 255, 0),
          "framebuffers: glReadPixels from the read binding (red) while drawing into the default one");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    check(pixel_is(8, 8, 0, 255, 0, 255, 0), "framebuffers: the default framebuffer drawn green");
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, inv);
    check(glGetError() == GL_NO_ERROR, "framebuffers: glInvalidateFramebuffer of colour and depth-stencil");
    {
        static const GLenum bad[1] = { GL_COLOR_ATTACHMENT0 + 7 };
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, bad);
        check(glGetError() == GL_INVALID_OPERATION, "framebuffers: an attachment past the last: GL_INVALID_OPERATION");
    }

    /* layer 1 of a 2D array, slice 2 of a 3D texture */
    glBindTexture(GL_TEXTURE_2D_ARRAY, t[1]);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 16, 16, 3);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_3D, t[2]);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 16, 16, 4);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t[1], 0, 1);
    glViewport(0, 0, 16, 16);
    rt_uniforms(pr, blue, iv, uv, 0.0f);
    draw_cover(pr);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && pixel_is(4, 4, 0, 0, 255, 255, 0),
          "framebuffers: a 2D array's layer 1 drawn blue");
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t[2], 0, 2);
    rt_uniforms(pr, green, iv, uv, 0.0f);
    draw_cover(pr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t[1]);
    glUseProgram(pa);
    glUniform1i(glGetUniformLocation(pa, "tex"), 0);
    check(layer_is(pa, "layer", 1.0f, 0, 0, 255, 1),
          "framebuffers: the array sampled: layer 1 blue");
    glBindTexture(GL_TEXTURE_3D, t[2]);
    glUseProgram(pv);
    glUniform1i(glGetUniformLocation(pv, "tex"), 0);
    check(layer_is(pv, "z", 2.5f / 4.0f, 0, 255, 0, 1) &&
              glGetError() == GL_NO_ERROR,
          "framebuffers: the 3D texture sampled: slice 2 green");
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(3, t);
    glDeleteRenderbuffers(1, &rb);
    glDeleteProgram(pr);
    glDeleteProgram(pa);
    glDeleteProgram(pv);
}

/* glBlitFramebuffer (item 11, colour on the CPU): a 16x16 RGBA8 object,
 * red left and green right, scaled twice into the default framebuffer;
 * mirrored; RGBA32F into RGBA32F with the scissor test; and the default
 * framebuffer back into an object. */
static void test_blit(void)
{
    static const GLfloat red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, f[4] = { 0.25f, -2.0f, 100.5f, 0.5f };
    static const GLfloat zero[4] = { 0, 0, 0, 0 };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint p = program("es3rt"), fb[3], t[3];
    int k;
    if (!p) {
        check(0, "blit: es3rt links");
        return;
    }
    glGenFramebuffers(3, fb);
    glGenTextures(3, t);
    for (k = 0; k < 3; k++) {
        glBindTexture(GL_TEXTURE_2D, t[k]);
        glTexStorage2D(GL_TEXTURE_2D, 1, k == 2 ? GL_RGBA8 : k ? GL_RGBA32F : GL_RGBA8, 16, 16);
        glBindFramebuffer(GL_FRAMEBUFFER, fb[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[k], 0);
    }
    /* object 0: red left, green right */
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glViewport(0, 0, 16, 16);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 8, 16);
    rt_uniforms(p, red, iv, uv, 0.0f);
    draw_cover(p);
    glScissor(8, 0, 8, 16);
    rt_uniforms(p, green, iv, uv, 0.0f);
    draw_cover(p);
    glDisable(GL_SCISSOR_TEST);
    /* scaled twice into the default framebuffer, then mirrored */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 32, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    check(pixel_is(4, 4, 255, 0, 0, 255, 0) && pixel_is(28, 28, 0, 255, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "blit: an RGBA8 object scaled twice into the default framebuffer");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glBlitFramebuffer(0, 0, 16, 16, 48, 0, 32, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    check(pixel_is(46, 4, 255, 0, 0, 255, 0) && pixel_is(34, 4, 0, 255, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "blit: mirrored (green left, red right)");

    /* RGBA32F into RGBA32F, the scissor box confining it */
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]);
    glViewport(0, 0, 16, 16);
    rt_uniforms(p, f, iv, uv, 0.0f);
    draw_cover(p);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[2]); /* an RGBA8 one, to have a second float target: t[1] again */
    glBindTexture(GL_TEXTURE_2D, t[2]);
    {
        GLuint t4;
        glGenTextures(1, &t4);
        glBindTexture(GL_TEXTURE_2D, t4);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 16, 16);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t4, 0);
        glClearBufferfv(GL_COLOR, 0, zero);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 8, 16);
        glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[2]);
        check(floats_are(4, 4, f, 0.0f) && floats_are(12, 4, zero, 0.0f) && glGetError() == GL_NO_ERROR,
              "blit: RGBA32F into RGBA32F within the scissor box");
        glBindFramebuffer(GL_FRAMEBUFFER, fb[2]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[2], 0);
        glDeleteTextures(1, &t4);
    }
    /* the default framebuffer (red at 4,4) back into object 2 */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[2]);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[2]);
    check(pixel_is(4, 4, 255, 0, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "blit: the default framebuffer into an object");
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_DEPTH_BUFFER_BIT, GL_LINEAR);
    check(glGetError() == GL_INVALID_OPERATION, "blit: depth with GL_LINEAR is GL_INVALID_OPERATION");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(3, fb);
    glDeleteTextures(3, t);
    glDeleteProgram(p);
}

/* Multisampled renderbuffers (GLES3 plan stage C item 2): an RGBA8 and a
 * D24S8 one of 4 samples, a triangle's diagonal edge drawn into them, then
 * resolved by glBlitFramebuffer into a texture: inside red, outside the
 * clear colour, the pixels the edge crosses in between. A second job loads
 * every sample back and draws over all with alpha to coverage (alpha 0.5:
 * two samples of four). And ES 3.0's errors: no samples for RGBA32F,
 * reads and copies from a multisampled framebuffer, blits into one or of
 * another rectangle. */
static void test_msaa(void)
{
    static const GLfloat red[4] = { 1, 0, 0, 1 }, half_green[4] = { 0, 1, 0, 0.5f };
    static const GLfloat tri[6] = { -1, -1, 1, -1, -1, 1 }; /* x + y < 16 of 16x16 */
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint p = program("es3rt"), fb[2], rb[2], t;
    GLint n = -1, sb = -1, pos;
    GLubyte px[4];
    if (!p) {
        check(0, "msaa: es3rt links");
        return;
    }
    glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_SAMPLES, 1, &n);
    check(n == 4 && glGetError() == GL_NO_ERROR, "msaa: RGBA8 takes 4 samples");
    glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA32F, GL_NUM_SAMPLE_COUNTS, 1, &n);
    check(n == 0 && glGetError() == GL_NO_ERROR, "msaa: RGBA32F takes none");
    glGenRenderbuffers(2, rb);
    glGenFramebuffers(2, fb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 2, GL_RGBA32F, 16, 16);
    check(glGetError() == GL_INVALID_OPERATION, "msaa: RGBA32F with samples is GL_INVALID_OPERATION");
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 2, GL_RGBA8, 16, 16);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &n);
    check(n == 4 && glGetError() == GL_NO_ERROR, "msaa: 2 samples asked, 4 given");
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, 16, 16);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sb);
    glGetIntegerv(GL_SAMPLES, &n);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && sb == 1 && n == 4,
          "msaa: complete, GL_SAMPLE_BUFFERS 1 and GL_SAMPLES 4");
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(glGetError() == GL_INVALID_OPERATION, "msaa: glReadPixels is GL_INVALID_OPERATION");
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 4, 4, 0);
    check(glGetError() == GL_INVALID_OPERATION, "msaa: glCopyTexImage2D is GL_INVALID_OPERATION");
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);

    /* the edge, depth tested against a cleared depth of 1 */
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    rt_uniforms(p, red, iv, uv, 0.0f);
    pos = glGetAttribLocation(p, "pos");
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray((GLuint)pos);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray((GLuint)pos);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    check(glGetError() == GL_NO_ERROR, "msaa: resolved by glBlitFramebuffer");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]);
    glReadPixels(7, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(pixel_is(2, 2, 255, 0, 0, 255, 0) && pixel_is(14, 14, 0, 0, 0, 0, 0), "msaa: inside red, outside clear");
    check(px[0] > 32 && px[0] < 224 && px[0] == px[3],
          "msaa: a pixel on the edge partly covered (its samples averaged)");
    if (!(px[0] > 32 && px[0] < 224))
        printf("     pixel (7, 8) is %d %d %d %d\n", px[0], px[1], px[2], px[3]);

    /* a second job: every sample loaded, half of them drawn over */
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    rt_uniforms(p, half_green, iv, uv, 0.0f);
    draw_cover(p);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]);
    check(pixel_is(2, 2, 128, 128, 0, 191, 3) && pixel_is(14, 14, 0, 128, 0, 64, 3),
          "msaa: a second job over the samples kept, alpha to coverage covering two");

    /* ES 3.0 4.3.3's errors */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    check(glGetError() == GL_INVALID_OPERATION, "msaa: a resolve into another rectangle is GL_INVALID_OPERATION");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[0]);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    check(glGetError() == GL_INVALID_OPERATION, "msaa: a blit into multisampled renderbuffers is GL_INVALID_OPERATION");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glDeleteFramebuffers(2, fb);
    glDeleteRenderbuffers(2, rb);
    glDeleteTextures(1, &t);
    glDeleteProgram(p);
}

/* ES 3.1's vertex attribute binding (GLES3 plan stage D item 5), in an ES
 * 3.1 context only (an ES31=1 library gives one for 3.0): es3inst's pos and
 * col interleaved in one buffer read through one binding at two relative
 * offsets; col from a binding of stride 0 (every vertex its first
 * element); col an element per instance by glVertexBindingDivisor. The
 * triangle covers the lower left of x + y < 0 (NDC); instance 1 moves
 * right by 1 (32 pixels). */
static void test_vab(void)
{
    static const GLfloat inter[18] = { -1, -1, 0, 1, 0, 1,   0, -1, 0, 1, 0, 1,   -1, 0, 0, 1, 0, 1 };
    static const GLfloat pos[6] = { -1, -1, 0, -1, -1, 0 };
    static const GLfloat cols[8] = { 1, 0, 0, 1,   1, 1, 0, 1 };
    static const GLfloat blue[8] = { 0, 0, 1, 1,   1, 1, 0, 1 };
    GLuint p = program("es3inst"), b[3], vao;
    GLint lp, lc;
    if (!p) {
        check(0, "vertex attribute binding: es3inst links");
        return;
    }
    lp = glGetAttribLocation(p, "pos");
    lc = glGetAttribLocation(p, "col");
    glUseProgram(p);
    glGenVertexArrays(1, &vao); /* ES 3.1: not with object 0 */
    glBindVertexArray(vao);
    glGenBuffers(3, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(inter), inter, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, b[2]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cols), cols, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray((GLuint)lp);
    glEnableVertexAttribArray((GLuint)lc);

    /* interleaved: binding 0, stride 24; pos at 0, col (green) at 8 */
    glBindVertexBuffer(0, b[0], 0, 24);
    glVertexAttribFormat((GLuint)lp, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribFormat((GLuint)lc, 4, GL_FLOAT, GL_FALSE, 8);
    glVertexAttribBinding((GLuint)lp, 0);
    glVertexAttribBinding((GLuint)lc, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    check(pixel_is(4, 4, 0, 255, 0, 255, 0) && pixel_is(40, 40, 0, 0, 0, 0, 0) && glGetError() == GL_NO_ERROR,
          "vertex attribute binding: one interleaved buffer, two relative offsets");

    /* col from binding 1 of stride 0: its first element (red) for every vertex */
    glBindVertexBuffer(1, b[2], 0, 0);
    glBindVertexBuffer(0, b[1], 0, 8);
    glVertexAttribFormat((GLuint)lc, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribBinding((GLuint)lc, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    check(pixel_is(4, 4, 255, 0, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "vertex attribute binding: a binding of stride 0 gives every vertex its first element");

    /* col an element per instance: binding 1 at stride 16, divisor 1 */
    glBindBuffer(GL_ARRAY_BUFFER, b[2]);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(blue), blue);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexBuffer(1, b[2], 0, 16);
    glVertexBindingDivisor(1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
    check(pixel_is(4, 4, 0, 0, 255, 255, 0) && pixel_is(36, 4, 255, 255, 0, 255, 0) && glGetError() == GL_NO_ERROR,
          "vertex attribute binding: glVertexBindingDivisor, an element per instance");

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(3, b);
    glDeleteProgram(p);
}

/* dst[i] is src[i] * scale + i for i below n, else `rest` (glMapBufferRange
 * of the storage buffer: the dispatch's results copied back). */
static int storage_is(GLuint buf, const GLuint *src, GLuint scale, int n, int total, GLuint rest)
{
    const GLuint *d;
    int i, ok = 1;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    d = (const GLuint *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, total * 4, GL_MAP_READ_BIT);
    if (!d)
        return 0;
    for (i = 0; i < total && ok; i++)
        if (d[i] != (i < n ? src[i] * scale + (GLuint)i : rest)) {
            printf("     dst[%d] is %lu\n", i, (unsigned long)d[i]);
            ok = 0;
        }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return ok;
}

/* shaders/<name>.comp, linked alone; 0 if not (the log printed). */
static GLuint compute_program(const char *name)
{
    char fn[32], log[256], *text;
    GLuint p, s;
    GLint ok = 0;
    snprintf(fn, sizeof(fn), "%s.comp", name);
    if (!(text = read_text(fn))) {
        printf("     no shaders/%s\n", fn);
        return 0;
    }
    p = glCreateProgram();
    s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, (const GLchar *const *)&text, 0);
    glCompileShader(s);
    glAttachShader(p, s);
    glDeleteShader(s);
    free(text);
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

/* ES 3.1's bounds checks (Mesa patch 0017; the V3D's MMU is unused, so a
 * store outside its buffer would land in the Amiga's memory): es31oob
 * writes dst[i] = i + tmp[l] (tmp[l] = 1) for 64 invocations, and also
 * dst[i + 64] and tmp[l + 16], past their ends. dst is a 256-byte range of
 * a 512-byte buffer: a storage store out of range is dropped, so the second
 * half keeps its 0x55 bytes. A shared one goes to offset 0 instead (Mesa's
 * nir_lower_robust_access: inside the array), so tmp[0] may read 7; were
 * it unchecked, it would land in the next work group's array (its other
 * elements 7) or past the block. Either way only the test's memory is
 * hit. */
static void test_compute_bounds(void)
{
    GLuint p = compute_program("es31oob"), b;
    GLubyte fill[512];
    const GLuint *d;
    int i, ok;
    if (!p) {
        check(0, "compute bounds: es31oob links");
        return;
    }
    memset(fill, 0x55, sizeof(fill));
    glGenBuffers(1, &b);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(fill), fill, GL_DYNAMIC_READ);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, b, 0, 256);
    glUseProgram(p);
    glUniform1ui(glGetUniformLocation(p, "past"), 16);
    glDispatchCompute(4, 1, 1);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
    d = (const GLuint *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(fill), GL_MAP_READ_BIT);
    ok = d != 0;
    for (i = 0; ok && i < 128; i++)
        if (d[i] != (i < 64 ? (GLuint)i + 1 : 0x55555555UL) && !(i < 64 && i % 16 == 0 && d[i] == (GLuint)i + 7)) {
            printf("     word %d is 0x%08lx\n", i, (unsigned long)d[i]);
            ok = 0;
        }
    if (d)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    check(ok && glGetError() == GL_NO_ERROR,
          "compute bounds: a storage store out of range dropped, a shared one kept in its array");
    glUseProgram(0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glDeleteBuffers(1, &b);
    glDeleteProgram(p);
}

/* ES 3.1's atomic counters (GLES3 plan stage D item 2): es31ac's 64
 * invocations each store what atomicCounterIncrement gave them (binding 2,
 * offset 4: the counter is the buffer's word 1, starting at 100). The
 * counter ends at 164, the stored values are 100 to 163 once each, the
 * words beside it keep theirs; the queries see the buffer and the counter. */
static void test_atomic_counters(void)
{
    static const GLuint init[3] = { 0x77, 100, 0x77 };
    static const GLenum props[2] = { GL_BUFFER_BINDING, GL_NUM_ACTIVE_VARIABLES };
    static const GLenum uprops[2] = { GL_ATOMIC_COUNTER_BUFFER_INDEX, GL_OFFSET };
    GLuint p = compute_program("es31ac"), b[2], seen[2] = { 0, 0 }, cu;
    GLint bv[2] = { -1, -1 }, uv[2] = { -1, -1 }, n = -1, na = -1;
    const GLuint *d;
    int i, ok;
    if (!p) {
        check(0, "atomic counters: es31ac links");
        return;
    }
    glGetProgramiv(p, GL_ACTIVE_ATOMIC_COUNTER_BUFFERS, &na);
    glGetProgramInterfaceiv(p, GL_ATOMIC_COUNTER_BUFFER, GL_ACTIVE_RESOURCES, &n);
    if (n == 1)
        glGetProgramResourceiv(p, GL_ATOMIC_COUNTER_BUFFER, 0, 2, props, 2, 0, bv);
    cu = glGetProgramResourceIndex(p, GL_UNIFORM, "count");
    if (cu != GL_INVALID_INDEX)
        glGetProgramResourceiv(p, GL_UNIFORM, cu, 2, uprops, 2, 0, uv);
    check(glGetError() == GL_NO_ERROR && na == 1 && n == 1 && bv[0] == 2 && bv[1] == 1 && uv[0] == 0 &&
              uv[1] == 4 && glGetUniformLocation(p, "count") == -1,
          "atomic counters: the queries (buffer binding 2, one counter at offset 4, no location)");
    glGenBuffers(2, b);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, b[0]);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(init), init, GL_DYNAMIC_READ);
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 2, b[0]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 64 * 4, 0, GL_DYNAMIC_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b[1]);
    glUseProgram(p);
    glDispatchCompute(4, 1, 1);
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    d = (const GLuint *)glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(init), GL_MAP_READ_BIT);
    ok = d && d[0] == 0x77 && d[1] == 164 && d[2] == 0x77;
    if (d && !ok)
        printf("     counter buffer 0x%lx %lu 0x%lx\n", (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2]);
    if (d)
        glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
    d = (const GLuint *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 64 * 4, GL_MAP_READ_BIT);
    for (i = 0; ok && d && i < 64; i++) {
        GLuint v = d[i] - 100;
        if (v >= 64 || (seen[v / 32] & (1UL << (v % 32)))) {
            printf("     dst[%d] is %lu\n", i, (unsigned long)d[i]);
            ok = 0;
        } else {
            seen[v / 32] |= 1UL << (v % 32);
        }
    }
    if (d)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    check(ok && d && glGetError() == GL_NO_ERROR,
          "atomic counters: 64 increments, each value given once, the counter at 164");
    glUseProgram(0);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glDeleteBuffers(2, b);
    glDeleteProgram(p);
}

/* ES 3.1's images (GLES3 plan stage D item 2): es31img reads src (unit 1,
 * R32UI, x + 8y + 10) and writes dst (unit 3, RGBA8 8x8: red the value,
 * green imageSize's width 8), its stores past the edge dropped; the image
 * uniforms' units are layout(binding)'s, glUniform1i cannot change them. */
static void test_images(void)
{
    GLuint p = compute_program("es31img"), t[2], fb, v[64];
    GLubyte px[8 * 8 * 4];
    GLint unit = -1, loc;
    int i, ok = 1;
    if (!p) {
        check(0, "images: es31img links");
        return;
    }
    loc = glGetUniformLocation(p, "dst");
    if (loc >= 0)
        glGetUniformiv(p, loc, &unit);
    glUseProgram(p);
    glUniform1i(loc, 0);
    check(unit == 3 && glGetError() == GL_INVALID_OPERATION, "images: dst at its layout(binding) unit 3, glUniform1i refused");
    for (i = 0; i < 64; i++)
        v[i] = (GLuint)(i + 10);
    glGenTextures(2, t);
    glBindTexture(GL_TEXTURE_2D, t[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 8, 8);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RED_INTEGER, GL_UNSIGNED_INT, v);
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
    glBindImageTexture(1, t[0], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    glBindImageTexture(3, t[1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute(2, 2, 1);
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[1], 0);
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < 64 && ok; i++) {
        const GLubyte *q = px + i * 4;
        if (q[0] != i + 10 || q[1] != 8 || q[2] != 0 || q[3] != 255) {
            printf("     dst (%d, %d) is %d %d %d %d\n", i % 8, i / 8, q[0], q[1], q[2], q[3]);
            ok = 0;
        }
    }
    check(ok && glGetError() == GL_NO_ERROR, "images: a dispatch loads src and stores dst (8x8, its sizes)");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    glBindImageTexture(3, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    glDeleteTextures(2, t);
    glUseProgram(0);
    glDeleteProgram(p);
}

/* ES 3.1's explicit uniform locations (GLES3 plan stage D item 6): es31loc
 * has tint at 7 and scale at 2 (the colour is tint * scale), an unused
 * uniform at 20 (not active: glUniform* there is ignored), nothing at 3
 * (GL_INVALID_OPERATION); tint and scale set with glProgramUniform* first. */
static void test_uniform_locations(void)
{
    GLuint p = program("es31loc");
    if (!p) {
        check(0, "uniform locations: es31loc links");
        return;
    }
    {
        GLint unit = -1, lt = glGetUniformLocation(p, "tex");
        if (lt >= 0)
            glGetUniformiv(p, lt, &unit);
        check(unit == 3 && glGetError() == GL_NO_ERROR, "uniform locations: a sampler starts at its layout(binding) unit 3");
    }
    check(glGetUniformLocation(p, "tint") == 7 && glGetUniformLocation(p, "scale") == 2 &&
              glGetUniformLocation(p, "unused") == -1 &&
              glGetProgramResourceLocation(p, GL_UNIFORM, "tint") == 7 && glGetError() == GL_NO_ERROR,
          "uniform locations: layout(location) 7 and 2 as given, an inactive one -1");
    /* ES 3.1's glProgramUniform*: set while another program (none) is in use */
    glUseProgram(0);
    glProgramUniform4f(p, 7, 0.0f, 1.0f, 0.5f, 1.0f);
    glProgramUniform1f(p, 2, 1.0f);
    check(glGetError() == GL_NO_ERROR, "uniform locations: glProgramUniform* on a program not in use");
    glUseProgram(p);
    glUniform4f(20, 1.0f, 1.0f, 1.0f, 1.0f);
    check(glGetError() == GL_NO_ERROR, "uniform locations: glUniform* at an inactive explicit location is ignored");
    glUniform1f(3, 1.0f);
    check(glGetError() == GL_INVALID_OPERATION, "uniform locations: a location of no uniform is GL_INVALID_OPERATION");
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(32, 32, 0, 255, 128, 255, 2) && glGetError() == GL_NO_ERROR,
          "uniform locations: a draw with tint and scale set at 7 and 2");
    glUseProgram(0);
    glDeleteProgram(p);
}

/* dEQP-GLES31's draw_indirect.*.multiple_attributes setup, which restarted
 * the A1200 (2026-10-28): a vertex array object with two attributes from
 * buffers of their own, es3inst's pos from 4 floats a vertex (read as its
 * vec2) and col from 2 (read as (x, y, 0, 1)), first 1; drawn directly
 * (vao2) or indirectly (vao2_indirect). Pixel (4, 4) is (255, 255, 0). */
static void test_vao2(int indirect)
{
    static const GLfloat pos[16] = { 9, 9, 9, 1,   -1, -1, 0, 1,   0, -1, 0, 1,   -1, 0, 0, 1 };
    static const GLfloat col[8] = { 0, 0,   1, 1,   1, 1,   1, 1 };
    static const GLuint cmd[4] = { 3, 1, 1, 0 };
    GLuint p = program("es3inst"), b[3], vao;
    GLint lp, lc;
    if (!p) {
        check(0, "two attribute buffers: es3inst links");
        return;
    }
    lp = glGetAttribLocation(p, "pos");
    lc = glGetAttribLocation(p, "col");
    glUseProgram(p);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(3, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
    glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray((GLuint)lp);
    glBindBuffer(GL_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(col), col, GL_STATIC_DRAW);
    glVertexAttribPointer((GLuint)lc, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray((GLuint)lc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (indirect) {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b[2]);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), cmd, GL_STATIC_DRAW);
        glDrawArraysIndirect(GL_TRIANGLES, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    } else {
        glDrawArrays(GL_TRIANGLES, 1, 3);
    }
    check(pixel_is(4, 4, 255, 255, 0, 255, 0) && pixel_is(40, 40, 0, 0, 0, 0, 0) && glGetError() == GL_NO_ERROR,
          indirect ? "two attribute buffers in a vertex array object, glDrawArraysIndirect"
                   : "two attribute buffers in a vertex array object, glDrawArrays first 1");
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(3, b);
    glUseProgram(0);
    glDeleteProgram(p);
}

/* ES 3.1's indirect draws (GLES3 plan stage D item 3) with es3inst (the
 * triangle on the lower left of x + y < 0, instance i moved right by 32
 * pixels): glDrawArraysIndirect of 2 instances (green), then
 * glDrawElementsIndirect with a base vertex of 3 (vertices 0 to 2 lie off
 * the screen) and its command at offset 16 (blue); then ES 3.1's errors:
 * an offset not a word's, a command past the buffer's end, vertex array
 * object 0, no indirect buffer. */
static void test_indirect(void)
{
    static const GLfloat pos[12] = { 5, 5, 6, 5, 5, 6,   -1, -1, 0, -1, -1, 0 };
    static const GLubyte idx[3] = { 0, 1, 2 };
    static const GLuint cmds[9] = { 3, 2, 3, 0,   3, 1, 0, 3, 0 }; /* arrays: first 3; elements at 16 */
    GLuint p = program("es3inst"), b[3], vao;
    GLint lp, lc;
    if (!p) {
        check(0, "indirect draws: es3inst links");
        return;
    }
    lp = glGetAttribLocation(p, "pos");
    lc = glGetAttribLocation(p, "col");
    glUseProgram(p);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(3, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
    glVertexAttribPointer((GLuint)lp, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray((GLuint)lp);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b[2]);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmds), cmds, GL_STATIC_DRAW);

    glVertexAttrib4f((GLuint)lc, 0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysIndirect(GL_TRIANGLES, (const void *)0);
    check(pixel_is(4, 4, 0, 255, 0, 255, 0) && pixel_is(36, 4, 0, 255, 0, 255, 0) &&
              pixel_is(40, 40, 0, 0, 0, 0, 0) && glGetError() == GL_NO_ERROR,
          "indirect draws: glDrawArraysIndirect, 2 instances from first 3");
    glVertexAttrib4f((GLuint)lc, 0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_BYTE, (const void *)16);
    check(pixel_is(4, 4, 0, 0, 255, 255, 0) && pixel_is(36, 4, 0, 0, 0, 0, 0) && glGetError() == GL_NO_ERROR,
          "indirect draws: glDrawElementsIndirect at offset 16, base vertex 3");

    glDrawArraysIndirect(GL_TRIANGLES, (const void *)2);
    check(glGetError() == GL_INVALID_VALUE, "indirect draws: an offset not a word's is GL_INVALID_VALUE");
    glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_BYTE, (const void *)20);
    check(glGetError() == GL_INVALID_OPERATION, "indirect draws: a command past the buffer's end is refused");
    glBindVertexArray(0);
    glDrawArraysIndirect(GL_TRIANGLES, (const void *)0);
    check(glGetError() == GL_INVALID_OPERATION, "indirect draws: vertex array object 0 is refused");
    glBindVertexArray(vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glDrawArraysIndirect(GL_TRIANGLES, (const void *)0);
    check(glGetError() == GL_INVALID_OPERATION, "indirect draws: no indirect buffer is refused");

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(3, b);
    glUseProgram(0);
    glDeleteProgram(p);
}

/* ES 3.1's storage buffers and atomic counters in a fragment shader (GLES3
 * plan stage D item 2): es31frag over the whole 64x64 framebuffer counts
 * its fragments (binding 1) and stores each one's count in its pixel's
 * word (binding 2). The counter ends at 4096, every word holds one of 1 to
 * 4096 and no two the same; the pixels are green. Read back through
 * mappings: the job is waited for and its writes cleaned to memory. */
static void test_fragment_storage(void)
{
    GLuint p = program("es31frag"), b[2], seen[128];
    GLuint zero = 0;
    const GLuint *d;
    int i, ok;
    if (!p) {
        check(0, "fragment storage: es31frag links");
        return;
    }
    memset(seen, 0, sizeof(seen));
    glGenBuffers(2, b);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, b[0]);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, 4, &zero, GL_DYNAMIC_READ);
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 1, b[0]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4096 * 4, 0, GL_DYNAMIC_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, b[1]);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    ok = pixel_is(4, 4, 0, 255, 0, 255, 0) && pixel_is(60, 60, 0, 255, 0, 255, 0);
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    d = (const GLuint *)glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, 4, GL_MAP_READ_BIT);
    if (!d || d[0] != 4096) {
        printf("     the counter is %lu\n", d ? (unsigned long)d[0] : 0UL);
        ok = 0;
    }
    if (d)
        glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
    d = (const GLuint *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 4096 * 4, GL_MAP_READ_BIT);
    for (i = 0; ok && d && i < 4096; i++) {
        GLuint v = d[i] - 1;
        if (v >= 4096 || (seen[v / 32] & (1UL << (v % 32)))) {
            printf("     pixel %d, %d holds %lu\n", i % 64, i / 64, (unsigned long)d[i]);
            ok = 0;
        } else {
            seen[v / 32] |= 1UL << (v % 32);
        }
    }
    if (d)
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    check(ok && d && glGetError() == GL_NO_ERROR,
          "fragment storage: 4096 fragments counted, each storing its count in its pixel's word");
    glUseProgram(0);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glDeleteBuffers(2, b);
    glDeleteProgram(p);
}

/* ES 3.1's compute shaders and storage buffers (GLES3 plan stage D items 1
 * and 2): es31cs (16 invocations a work group) writes dst[i] = src[i] *
 * scale + i, src at storage binding 0, dst at 3; read back through a
 * mapping. The errors: a draw with a compute program, a dispatch without
 * one or past the work group limit; glDispatchComputeIndirect. */
static void test_compute(void)
{
    static const GLuint ind[3] = { 2, 1, 1 };
    GLuint src[64], zero[64], b[3], p = compute_program("es31cs"), vao;
    GLint i;
    if (!p) {
        check(0, "compute: es31cs links");
        return;
    }
    {
        /* ES 3.1's program interface queries on it */
        static const GLenum bprops[3] = { GL_BUFFER_BINDING, GL_NUM_ACTIVE_VARIABLES, GL_REFERENCED_BY_COMPUTE_SHADER };
        static const GLenum vprops[4] = { GL_OFFSET, GL_ARRAY_STRIDE, GL_ARRAY_SIZE, GL_BLOCK_INDEX };
        GLint bv[3] = { -1, -1, -1 }, vv[4] = { -1, -1, -1, -1 }, n = -1, len = -1;
        GLuint out = glGetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "Out");
        GLuint dst = glGetProgramResourceIndex(p, GL_BUFFER_VARIABLE, "dst[0]");
        char name[16];
        glGetProgramInterfaceiv(p, GL_BUFFER_VARIABLE, GL_ACTIVE_RESOURCES, &n);
        if (out != GL_INVALID_INDEX)
            glGetProgramResourceiv(p, GL_SHADER_STORAGE_BLOCK, out, 3, bprops, 3, 0, bv);
        if (dst != GL_INVALID_INDEX) {
            glGetProgramResourceiv(p, GL_BUFFER_VARIABLE, dst, 4, vprops, 4, 0, vv);
            glGetProgramResourceName(p, GL_BUFFER_VARIABLE, dst, sizeof(name), &len, name);
        }
        check(glGetError() == GL_NO_ERROR && n == 2 && bv[0] == 3 && bv[1] == 1 && bv[2] == 1 &&
                  vv[0] == 0 && vv[1] == 4 && vv[2] == 0 && vv[3] == (GLint)out && len == 6 && !strcmp(name, "dst[0]") &&
                  glGetProgramResourceIndex(p, GL_BUFFER_VARIABLE, "dst") == dst &&
                  glGetProgramResourceLocation(p, GL_UNIFORM, "scale") == glGetUniformLocation(p, "scale"),
              "compute: program interface queries (storage block Out, buffer variable dst[], uniform scale)");
        if (!(n == 2 && bv[0] == 3))
            printf("     %d buffer variables; Out %lu: binding %d, %d variables, compute %d; dst %lu: offset %d, "
                   "stride %d, size %d, block %d\n", (int)n, (unsigned long)out, (int)bv[0], (int)bv[1], (int)bv[2],
                   (unsigned long)dst, (int)vv[0], (int)vv[1], (int)vv[2], (int)vv[3]);
    }
    for (i = 0; i < 64; i++) {
        src[i] = (GLuint)(i * 3 + 1);
        zero[i] = 0;
    }
    glGenBuffers(3, b);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(src), src, GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero, GL_DYNAMIC_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, b[1]);

    glDispatchCompute(1, 1, 1);
    check(glGetError() == GL_INVALID_OPERATION, "compute: a dispatch without a compute program is refused");
    glUseProgram(p);
    glUniform1ui(glGetUniformLocation(p, "scale"), 5);
    glDispatchCompute(4, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    check(glGetError() == GL_NO_ERROR && storage_is(b[1], src, 5, 64, 64, 0),
          "compute: 4 work groups of 16 write 64 words of a storage buffer");

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b[1]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero), zero);
    glGenBuffers(1, &b[2]);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, b[2]);
    glBufferData(GL_DISPATCH_INDIRECT_BUFFER, sizeof(ind), ind, GL_STATIC_DRAW);
    glUniform1ui(glGetUniformLocation(p, "scale"), 7);
    glDispatchComputeIndirect(0);
    check(glGetError() == GL_NO_ERROR && storage_is(b[1], src, 7, 32, 64, 0),
          "compute: glDispatchComputeIndirect, 2 work groups from a buffer, after glBufferSubData");
    glDispatchComputeIndirect(2);
    check(glGetError() == GL_INVALID_VALUE, "compute: an indirect offset not a word's is GL_INVALID_VALUE");
    glDispatchComputeIndirect(4);
    check(glGetError() == GL_INVALID_OPERATION, "compute: an indirect command past the buffer's end is refused");
    glDispatchCompute(65536, 1, 1);
    check(glGetError() == GL_INVALID_VALUE, "compute: 65536 work groups is GL_INVALID_VALUE");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    check(glGetError() == GL_INVALID_OPERATION, "compute: a draw with a compute program is refused");
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glUseProgram(0);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    glDeleteBuffers(3, b);
    glDeleteProgram(p);
}

/* Texel (x, y) of t sampled (es3tc, nearest) is (r, g, b). */
static int texel_at(GLuint p, GLuint t, int w, int h, int x, int y, int r, int g, int b)
{
    glBindTexture(GL_TEXTURE_2D, t);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glUniform2f(glGetUniformLocation(p, "at"), ((GLfloat)x + 0.5f) / (GLfloat)w, ((GLfloat)y + 0.5f) / (GLfloat)h);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    if (!pixel_is(32, 32, r, g, b, 255, 1)) {
        printf("     (texel %d, %d of %dx%d)\n", x, y, w, h);
        return 0;
    }
    return 1;
}

/* A texture a framebuffer object drew into, sampled: green, its left 64
 * columns red (drawn with the scissor test), at sizes whose layouts differ
 * (dEQP-GLES3's fbo.color.tex2d draws into 129x117). */
static int rendered_is_sampled(GLuint pr, GLuint pt, int w, int h)
{
    static const GLfloat green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 };
    static const GLint iv[4] = { 0, 0, 0, 0 };
    static const GLuint uv[4] = { 0, 0, 0, 0 };
    GLuint fb, t;
    int ok;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glViewport(0, 0, w, h);
    rt_uniforms(pr, green, iv, uv, 0.0f);
    draw_cover(pr);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 64, h);
    rt_uniforms(pr, red, iv, uv, 0.0f);
    draw_cover(pr);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    ok = texel_at(pt, t, w, h, 10, h / 2, 255, 0, 0) && texel_at(pt, t, w, h, (w + 64) / 2, h / 2, 0, 255, 0) &&
         texel_at(pt, t, w, h, 63, h - 1, 255, 0, 0) && texel_at(pt, t, w, h, 64, 0, 0, 255, 0) &&
         texel_at(pt, t, w, h, w - 1, h - 1, 0, 255, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &t);
    return ok;
}

static void test_rendered_textures(void)
{
    GLuint pr = program("es3rt"), pt = program("es3tc");
    if (!pr || !pt) {
        check(0, "rendered textures: es3rt and es3tc link");
        return;
    }
    check(rendered_is_sampled(pr, pt, 128, 128) && glGetError() == GL_NO_ERROR,
          "rendered textures: 128x128 drawn, then sampled");
    check(rendered_is_sampled(pr, pt, 129, 117) && glGetError() == GL_NO_ERROR,
          "rendered textures: 129x117 drawn, then sampled");
    check(rendered_is_sampled(pr, pt, 99, 128) && glGetError() == GL_NO_ERROR,
          "rendered textures: 99x128 drawn, then sampled");
    glDeleteProgram(pr);
    glDeleteProgram(pt);
}

/* ES 3.0's integer textures through es3utex (usampler2D) and es3itex
 * (isampler2D, magnitudes): 8-bit unsigned, 32-bit unsigned (32-bit
 * returns), 16-bit signed; with a linear filter incomplete, the integer
 * (0, 0, 0, 1). */
static void test_int_textures(void)
{
    static const GLubyte u8[4] = { 10, 20, 30, 255 };
    static const GLuint u32[4] = { 40, 50, 60, 255 };
    static const GLshort s16[1] = { -20 };
    GLuint pu = program("es3utex"), pi = program("es3itex");
    if (!pu || !pi) {
        check(0, "integer textures: es3utex and es3itex link");
        return;
    }
    glUseProgram(pu);
    glUniform1i(glGetUniformLocation(pu, "tex"), 0);
    check(texel_is(pu, GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, u8, 10, 20, 30, 255, 0),
          "integer textures: GL_RGBA8UI (10, 20, 30, 255)");
    check(texel_is(pu, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, u32, 40, 50, 60, 255, 0),
          "integer textures: GL_RGBA32UI (40, 50, 60, 255)");
    glUseProgram(pi);
    glUniform1i(glGetUniformLocation(pi, "tex"), 0);
    check(texel_is(pi, GL_R16I, GL_RED_INTEGER, GL_SHORT, s16, 20, 0, 0, 1, 0),
          "integer textures: GL_R16I -20 (20, 0, 0, 1)");
    {
        GLuint t;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16I, 1, 1, 0, GL_RED_INTEGER, GL_SHORT, s16);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_cover(pi);
        check(pixel_is(32, 32, 0, 0, 0, 1, 0) && glGetError() == GL_NO_ERROR,
              "integer textures: a linear filter makes one incomplete (the integer (0, 0, 0, 1))");
        glDeleteTextures(1, &t);
    }
    glDeleteProgram(pu);
    glDeleteProgram(pi);
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
    /* ES 3.0's texture swizzle: (b, 0, 1, r) of the texel. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_cover(p);
    check(pixel_is(20, 20, 192, 0, 255, 64, 2), "texture swizzle: (b, 0, 1, r)");
    glDeleteTextures(2, t);
    glDeleteProgram(p);
}

/* ENV:M8_SKIP names tests to leave out (words of their names after
 * "test_", e.g. "indirect fragment_storage"): bisecting a fault on the A1200. */
static int skipped(const char *name)
{
    const char *list = getenv("M8_SKIP");
    size_t n = strlen(name);
    const char *at;
    for (at = list; at && (at = strstr(at, name)) != 0; at += n)
        if ((at == list || at[-1] == ' ') && (at[n] == 0 || at[n] == ' ')) {
            printf("SKIP: test_%s (ENV:M8_SKIP)\n", name);
            return 1;
        }
    return 0;
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
    test_sampler();
    test_layers();
    test_pixel_store();
    test_pixel_buffer();
    test_vao();
    test_instanced();
    test_formats();
    test_fence();
    test_query();
    test_storage();
    test_base_levels();
    test_rg();
    test_formats3();
    test_int_textures();
    test_etc2();
    test_rt_wide();
    test_rt_formats();
    test_rt_depth32f();
    test_rendered_textures();
    test_rt_mixed_clears();
    test_framebuffers();
    test_blit();
    test_depth32f_texture();
    test_rb_resize();
    test_copy3d();
    test_frag_depth();
    test_instanced_ext();
    test_attachment_queries();
    test_xfb();
    test_msaa();
    {
        GLint minor = 0;
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (minor >= 1) { /* an ES 3.1 context (an ES31=1 library) */
            if (!skipped("vab"))
                test_vab();
            if (!skipped("compute"))
                test_compute();
            if (!skipped("compute_bounds"))
                test_compute_bounds();
            if (!skipped("atomic_counters"))
                test_atomic_counters();
            if (!skipped("fragment_storage"))
                test_fragment_storage();
            if (!skipped("indirect"))
                test_indirect();
            if (!skipped("vao2"))
                test_vao2(0);
            if (!skipped("vao2_indirect"))
                test_vao2(1);
            if (!skipped("uniform_locations"))
                test_uniform_locations();
            if (!skipped("images"))
                test_images();
        }
    }
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
