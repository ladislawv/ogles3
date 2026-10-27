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
