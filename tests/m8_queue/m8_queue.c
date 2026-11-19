/*
 * The render chain (job.c, from ogles2pi3's m8_queue): a framebuffer
 * object's job queued behind the running render. On a screen of its own
 * (where the screen's render is left running), every frame first clears a
 * 64x64 texture to a colour of that frame's own, then draws it over the
 * screen; every second frame reads the screen's middle back: the frame
 * before left its render running, so this frame's object job was queued;
 * the pixel must be this frame's colour. Two cases a queued job's binning
 * must not run beside (job.c): H1, the screen's vertex shader sizes its
 * quad by the texture the object's job rendered (a binning-stage texture
 * read); H2, the screen's job feeds back into the buffer the object's job
 * drew from (ES 3.0). Ends with an object job queued and the context
 * destroyed at once (no swap), and checks that a second context's life
 * returns all memory. ENV:OGLES3/NoQueue's path is the package's second
 * step, the control.
 *   usage: m8_queue [FRAMES=60]
 * To see: a 640x480 screen changing colour every frame for about a second,
 * three times. tex1's shaders from their .v3dp (make test-v3dp); the
 * others compiled here (v3dglslc3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/ogles3.h>
#include <GLES3/gl3.h>
#pragma GCC diagnostic ignored "-Wunused-function" /* perf_target.h's gears helpers */
#include "../perf/perf_target.h"

#include "../common/tex_shaders.h"

static int fails, passes;

static void check(int ok, const char *msg)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok)
        passes++;
    else
        fails++;
}

static GLuint link_src(const char *vs, const char *fs, const char *const *varyings, int nvaryings)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    glShaderSource(v, 1, &vs, 0);
    glCompileShader(v);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_position");
    glBindAttribLocation(p, 1, "a_texcoord");
    if (nvaryings)
        glTransformFeedbackVaryings(p, nvaryings, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

/* The frame's colour: each channel 0, 0x80 or 0xFF, never the same twice
 * running. */
static void colour(int i, GLfloat *c)
{
    static const GLfloat lv[3] = {0.0f, 128.0f / 255.0f, 1.0f};
    i = i % 26 + 1; /* 1..26: not black */
    c[0] = lv[i % 3];
    c[1] = lv[i / 3 % 3];
    c[2] = lv[i / 9 % 3];
}

static ULONG rgb(const GLfloat *c)
{
    return ((ULONG)(c[0] * 255.0f + 0.5f) << 16) | ((ULONG)(c[1] * 255.0f + 0.5f) << 8) |
           (ULONG)(c[2] * 255.0f + 0.5f);
}

static ULONG read_middle(int w, int h)
{
    UBYTE px[4];
    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return ((ULONG)px[0] << 16) | ((ULONG)px[1] << 8) | px[2];
}

static const GLfloat quad[] = { /* x y z u v */
    -1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1,
};

/* H1's screen program: the quad's size the texture's green at its middle,
 * read in the vertex shader (the binning stage reads it too) */
static const char h1_vs[] =
    "attribute vec4 a_position;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    float s = texture2DLod(u_tex, vec2(0.5, 0.5), 0.0).g;\n"
    "    gl_Position = vec4(a_position.xy * s, 0.0, 1.0);\n"
    "}\n";
static const char h1_fs[] =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";

/* H2's programs (ES 3.0): the object's draw takes its colour from a
 * uniform block in the buffer (the GPU reads the buffer's words where they
 * lie), and the screen's feedback writes zeros into the same words */
static const char h2_draw_vs[] =
    "#version 300 es\n"
    "in vec4 a_position;\n"
    "void main() { gl_Position = a_position; }\n";
static const char h2_draw_fs[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "layout(std140) uniform U { vec4 c; };\n"
    "out vec4 o;\n"
    "void main() { o = c; }\n";
static const char h2_xfb_vs[] =
    "#version 300 es\n"
    "in vec4 a_position;\n"
    "out vec4 v_out;\n"
    "void main() { v_out = vec4(0.0, 0.0, 0.0, 1.0); gl_Position = a_position; gl_PointSize = 1.0; }\n";
static const char h2_xfb_fs[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(0.0); }\n";

static void *open_ctx(ULONG mode, ULONG es)
{
    ULONG err = 0;
    void *ctx = aglCreateContextTags2(&err, OGLES3_CCT_MODEID, mode, OGLES3_CCT_VSYNC, FALSE,
                                      es ? OGLES3_CCT_PISTORM_ES_VERSION : TAG_IGNORE, es, TAG_DONE);
    char msg[120];
    if (!ctx) {
        sprintf(msg, "a screen of its own (aglCreateContext2 error %lu)", (unsigned long)err);
        check(0, msg);
        return 0;
    }
    aglMakeCurrent(ctx);
    return ctx;
}

/* A size x size texture and its framebuffer object */
static int make_target(GLuint *tex, GLuint *fbo, int size)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        check(0, "the texture's framebuffer is complete");
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

/* One context's life: frames as described above; last_queued: it ends with
 * an object's job queued, destroyed without a swap. */
static void run(ULONG mode, int frames, int last_queued)
{
    void *ctx;
    GLuint prog, tex, fbo;
    GLfloat c[3];
    int i, bad = 0, reads = 0;
    char msg[160];
    if (!(ctx = open_ctx(mode, 0)))
        return;
    if (!(prog = link_src(tex1_vs, tex1_fs, 0, 0))) {
        check(0, "the program links (its .v3dp beside the program)");
        aglDestroyContext(ctx);
        return;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, quad + 3);
    if (!make_target(&tex, &fbo, 64)) {
        aglDestroyContext(ctx);
        return;
    }
    for (i = 0; i < frames; i++) {
        colour(i, c);
        /* the object's pass first: queued behind the frame before's render */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 64, 64);
        glClearColor(c[0], c[1], c[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, PG_W, PG_H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (i & 1) {
            ULONG got = read_middle(PG_W, PG_H);
            reads++;
            if (got != rgb(c) && bad++ < 4) {
                sprintf(msg, "frame %d: screen %06lx, its colour %06lx", i, (unsigned long)got,
                        (unsigned long)rgb(c));
                check(0, msg);
            }
        }
        aglSwapBuffers();
    }
    sprintf(msg, "%d frames, %d read back: each the frame's own colour (%d wrong)", frames, reads, bad);
    check(!bad, msg);
    if (last_queued) {
        /* An object's job queued, then the context destroyed with it. */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 64, 64);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT); /* starts the screen's job: the object's flushed (queued) */
    } else {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteProgram(prog);
    }
    aglDestroyContext(ctx);
}

/* H1: every frame the object cleared to green on even frames, black on odd
 * ones, then the screen's quad sized by it in the vertex shader: magenta at
 * the middle on even frames, black on odd ones. A screen job binned beside
 * the object's render would read last frame's green. */
static void run_h1(ULONG mode, int frames)
{
    void *ctx;
    GLuint prog, tex, fbo;
    int i, bad = 0, reads = 0;
    char msg[160];
    if (!(ctx = open_ctx(mode, 0)))
        return;
    if (!(prog = link_src(h1_vs, h1_fs, 0, 0))) {
        check(0, "H1: the vertex-texture program links");
        aglDestroyContext(ctx);
        return;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, quad);
    if (!make_target(&tex, &fbo, 64)) {
        aglDestroyContext(ctx);
        return;
    }
    for (i = 0; i < frames; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 64, 64);
        glClearColor(0, (i & 1) ? 0.0f : 1.0f, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, PG_W, PG_H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (i >= 2) {
            ULONG want = (i & 1) ? 0x000000UL : 0xff00ffUL, got = read_middle(PG_W, PG_H);
            reads++;
            if (got != want && bad++ < 4) {
                sprintf(msg, "H1: frame %d: screen %06lx, want %06lx", i, (unsigned long)got, (unsigned long)want);
                check(0, msg);
            }
        }
        aglSwapBuffers();
    }
    sprintf(msg, "H1: %d frames whose vertex shader reads the texture just rendered: %d of %d right", frames,
            reads - bad, reads);
    check(!bad, msg);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    aglDestroyContext(ctx);
}

#define H2_SIZE 512 /* the object's: its render long (with H2_QUADS quads over it) */
#define H2_QUADS 64

/* H2 (ES 3.0): every frame the object draws its quad in the colour of a
 * uniform block in buffer `ub` (red), then the screen's job feeds zeros
 * back into `ub` (rasterizer discard), then `ub` is given its red again.
 * The object's texture must be red at its middle: a screen job binned
 * beside the object's render would write the zeros while that render
 * reads them. */
static void run_h2(ULONG mode, int frames)
{
    static const GLfloat red[16] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    static const char *const varyings[1] = {"v_out"};
    void *ctx;
    GLuint pdraw, pxfb, tex, fbo, ub;
    int i, k, bad = 0;
    char msg[160];
    if (!(ctx = open_ctx(mode, 30)))
        return;
    pdraw = link_src(h2_draw_vs, h2_draw_fs, 0, 0);
    pxfb = link_src(h2_xfb_vs, h2_xfb_fs, varyings, 1);
    if (!pdraw || !pxfb) {
        check(0, "H2: the ES 3.0 programs link");
        aglDestroyContext(ctx);
        return;
    }
    if (!make_target(&tex, &fbo, H2_SIZE)) {
        aglDestroyContext(ctx);
        return;
    }
    glUniformBlockBinding(pdraw, glGetUniformBlockIndex(pdraw, "U"), 0);
    glGenBuffers(1, &ub);
    glBindBuffer(GL_UNIFORM_BUFFER, ub);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(red), red, GL_DYNAMIC_DRAW);
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, ub, 0, 16);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, quad);
    for (i = 0; i < frames; i++) {
        ULONG got;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, H2_SIZE, H2_SIZE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(pdraw);
        for (k = 0; k < H2_QUADS; k++) /* a render long enough to overlap the next job's binning */
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, PG_W, PG_H);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(pxfb);
        glEnable(GL_RASTERIZER_DISCARD);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, ub);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 4);
        glEndTransformFeedback();
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glDisable(GL_RASTERIZER_DISCARD);
        aglSwapBuffers();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        got = read_middle(H2_SIZE, H2_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (got != 0xff0000UL && bad++ < 4) {
            sprintf(msg, "H2: frame %d: the object's middle %06lx, want ff0000", i, (unsigned long)got);
            check(0, msg);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, ub);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(red), red);
    }
    sprintf(msg, "H2: %d frames feeding back into the uniform block the object drew with: %d of %d right",
            frames, frames - bad, frames);
    check(!bad, msg);
    glDeleteBuffers(1, &ub);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(pdraw);
    glDeleteProgram(pxfb);
    aglDestroyContext(ctx);
}

int main(int argc, char **argv)
{
    struct pg_args a;
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    ULONG mode, before, after;
    char msg[120];

    memset(&a, 0, sizeof(a));
    a.fullscreen = 1;
    if (frames < 4)
        frames = 60;
    if (!pt_open(&a, "m8_queue") || !OGLES3Open()) {
        printf("needs %s\n", OGLES3NAME);
        pt_close();
        return 20;
    }
    if (!(mode = pt_find_mode())) {
        check(0, "a 640x480 32-bit screen mode");
    } else {
        run(mode, frames, 1); /* also the library's first-use memory */
        before = AvailMem(MEMF_ANY);
        run(mode, frames, 0);
        after = AvailMem(MEMF_ANY);
        /* within 4 KB: other tasks allocate meanwhile; a leaked slot of
         * lists or tile pools is megabytes */
        sprintf(msg, "a second context's life returns the memory (%ld bytes difference)", (long)(after - before));
        check((long)(after - before) > -4096 && (long)(after - before) < 4096, msg);
        run_h1(mode, frames);
        run_h2(mode, frames);
    }
    printf("%s: %d of %d checks passed\n", fails ? "FAILED" : "PASSED", passes, passes + fails);
    OGLES3Close();
    pt_close();
    return fails ? 10 : 0;
}
