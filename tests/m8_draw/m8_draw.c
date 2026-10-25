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
