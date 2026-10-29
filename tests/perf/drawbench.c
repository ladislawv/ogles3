/*
 * drawbench: the CPU cost of the GL calls a game makes, call by call and as
 * one frame of Quake III's renderergl2 (its counts from the glue's profile,
 * docs/GLES3_PLAN.md §1: 71 draws a frame, 250 attribute pointers, 180
 * buffer updates, ...). The same source builds against ogles3.library's SDK
 * and ogles2.library's (tests/perf/build_drawbench.sh), so one run compares
 * the two libraries on the same calls. It draws into a small hidden bitmap:
 * the GPU's work is negligible, the times are the library's.
 *   usage: drawbench [MS=800] [ES=20|30|31] [TEST=name]
 * ES: ogles3 only (default its own); TEST: that test alone (for a profile).
 * Each test runs about MS milliseconds; a line per test:
 *   TEST <lib> <name> <ns a call> ns (<calls>)
 * and the frame mix as fps of the mix alone (integers: clib2's printf
 * prints no floating point without its maths library).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/timer.h>

#ifdef BENCH_OGLES2
#include <proto/ogles2.h>
#define LIB "ogles2"
#define BOpen OGLES2Open
#define BClose OGLES2Close
#else
#include <proto/ogles3.h>
#define LIB "ogles3"
#define BOpen OGLES3Open
#define BClose OGLES3Close
#endif

#define W 128
#define H 128
#define NV 40       /* vertices a draw (Quake's: about 930 bytes of them) */
#define NI 160      /* indices a draw */
#define VSTRIDE 24  /* position 3 floats, texture coordinates 2, colour 4 bytes */

struct GfxBase *GfxBase;
struct Device *TimerBase;
static struct timerequest tr;
static ULONG efreq;

static double now_us(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return ((double)e.ev_hi * 4294967296.0 + (double)e.ev_lo) * 1e6 / (double)efreq;
}

static const char *vs_src =
    "attribute vec3 a_pos; attribute vec2 a_tc; attribute vec4 a_col;\n"
    "uniform mat4 u_mvp; varying vec2 v_tc; varying vec4 v_col;\n"
    "void main() { v_tc = a_tc; v_col = a_col; gl_Position = u_mvp * vec4(a_pos, 1.0); }\n";
static const char *fs_src[3] = {
    "precision mediump float; uniform sampler2D u_tex; uniform vec4 u_color; varying vec2 v_tc; varying vec4 v_col;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_tc) * v_col * u_color; }\n",
    "precision mediump float; uniform sampler2D u_tex; uniform vec4 u_color; varying vec2 v_tc; varying vec4 v_col;\n"
    "void main() { vec4 c = texture2D(u_tex, v_tc) * v_col; if (c.a < 0.5) discard; gl_FragColor = c * u_color; }\n",
    "precision mediump float; uniform sampler2D u_tex; uniform sampler2D u_light; uniform vec4 u_color;\n"
    "varying vec2 v_tc; varying vec4 v_col;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_tc) * texture2D(u_light, v_tc.yx) * u_color; }\n"};

static GLuint prog[3];
static GLint u_mvp[3], u_color[3];
static GLuint tex[4], vbo, ibo;
static UBYTE vdata[NV * VSTRIDE];
static UWORD idata[NI];
static float mvp[16];

static GLuint link(const char *fs)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    glShaderSource(v, 1, &vs_src, 0);
    glShaderSource(f, 1, &fs, 0);
    glCompileShader(v);
    glCompileShader(f);
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_tc");
    glBindAttribLocation(p, 2, "a_col");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

static void setup(void)
{
    static ULONG texels[64 * 64];
    int k, i;
    for (k = 0; k < 3; k++) {
        GLint s;
        glUseProgram(prog[k]);
        u_mvp[k] = glGetUniformLocation(prog[k], "u_mvp");
        u_color[k] = glGetUniformLocation(prog[k], "u_color");
        if ((s = glGetUniformLocation(prog[k], "u_tex")) >= 0)
            glUniform1i(s, 0);
        if ((s = glGetUniformLocation(prog[k], "u_light")) >= 0)
            glUniform1i(s, 1);
        glUniform4f(u_color[k], 1, 1, 1, 1);
    }
    glGenTextures(4, tex);
    for (k = 0; k < 4; k++) {
        for (i = 0; i < 64 * 64; i++)
            texels[i] = 0xff000000UL | ((ULONG)(i * (k + 3)) & 0xffffff);
        glBindTexture(GL_TEXTURE_2D, tex[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex[3]);
    glActiveTexture(GL_TEXTURE0);
    /* A small fan of triangles in the middle of the picture: few pixels */
    for (i = 0; i < NV; i++) {
        float *p = (float *)(vdata + i * VSTRIDE);
        p[0] = (float)(i % 8) * 0.02f - 0.08f;
        p[1] = (float)(i / 8) * 0.02f - 0.05f;
        p[2] = 0.0f;
        p[3] = (float)(i % 8) / 8.0f;
        p[4] = (float)(i / 8) / 5.0f;
        vdata[i * VSTRIDE + 20] = vdata[i * VSTRIDE + 21] = vdata[i * VSTRIDE + 22] = vdata[i * VSTRIDE + 23] = 255;
    }
    for (i = 0; i < NI; i++)
        idata[i] = (UWORD)((i / 3 + i % 3) % NV);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * NV * VSTRIDE, 0, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 16 * NI * 2, 0, GL_DYNAMIC_DRAW);
    for (k = 0; k < 16; k++) {
        glBufferSubData(GL_ARRAY_BUFFER, k * NV * VSTRIDE, NV * VSTRIDE, vdata);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, k * NI * 2, NI * 2, idata);
    }
    for (k = 0; k < 3; k++)
        glEnableVertexAttribArray(k);
    memset(mvp, 0, sizeof(mvp));
    mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f;
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(prog[0]);
    glUniformMatrix4fv(u_mvp[0], 1, GL_FALSE, mvp);
}

static void pointers(int slice)
{
    ULONG o = (ULONG)slice * NV * VSTRIDE;
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VSTRIDE, (void *)o);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, VSTRIDE, (void *)(o + 12));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, VSTRIDE, (void *)(o + 20));
}

/* ---- the tests: each does `n` of its calls; FALSE: not one at all */
typedef void (*test_fn)(int n);

static void t_vap(int n)
{
    int i;
    for (i = 0; i < n; i++)
        glVertexAttribPointer(i % 3, 3, GL_FLOAT, GL_FALSE, VSTRIDE, (void *)(ULONG)((i & 15) * 4));
}
static void t_enable(int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (i & 1)
            glDisable(GL_BLEND);
        else
            glEnable(GL_BLEND);
}
static void t_bindbuf(int n)
{
    int i;
    for (i = 0; i < n; i++)
        glBindBuffer(GL_ARRAY_BUFFER, (i & 1) ? vbo : ibo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
}
static void t_bindtex(int n)
{
    int i;
    for (i = 0; i < n; i++)
        glBindTexture(GL_TEXTURE_2D, tex[i & 3]);
}
static void t_useprog(int n)
{
    int i;
    for (i = 0; i < n; i++)
        glUseProgram(prog[i % 3]);
    glUseProgram(prog[0]);
}
static void t_uniform4f(int n)
{
    int i;
    for (i = 0; i < n; i++)
        glUniform4f(u_color[0], 1.0f, (float)(i & 1), 1.0f, 1.0f);
}
static void t_matrix(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        mvp[12] = (float)(i & 7) * 0.001f;
        glUniformMatrix4fv(u_mvp[0], 1, GL_FALSE, mvp);
    }
}
static void t_subdata(int n)
{
    int i;
    for (i = 0; i < n; i++)
        glBufferSubData(GL_ARRAY_BUFFER, (i & 15) * NV * VSTRIDE, NV * VSTRIDE, vdata);
}
static void t_bufdata(int n)
{
    static GLuint b;
    int i;
    if (!b)
        glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    for (i = 0; i < n; i++)
        glBufferData(GL_ARRAY_BUFFER, NV * VSTRIDE, vdata, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
}
/* The same draw again and again: the draw's own cost */
static void t_draw(int n)
{
    int i;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    pointers(0);
    for (i = 0; i < n; i++)
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
}
/* Draws from client arrays (gl4es's way, Quake III through it) */
static void t_draw_client(int n)
{
    int i;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VSTRIDE, vdata);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, VSTRIDE, vdata + 12);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, VSTRIDE, vdata + 20);
    for (i = 0; i < n; i++)
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, idata);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
}
/* The same draw with one thing changed before each: blending (every draw
 * sends the whole state again), the program, the texture. */
static void t_draw_blend(int n)
{
    int i;
    t_draw(0);
    for (i = 0; i < n; i++) {
        if (i & 1)
            glDisable(GL_BLEND);
        else
            glEnable(GL_BLEND);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
    }
    glDisable(GL_BLEND);
}
/* A depth clear after a draw of the frame (drawn as a rectangle, as Quake
 * III's view clears): a call is the clear and the draw before it. */
static void t_clear_mid(int n)
{
    int i;
    t_draw(0);
    for (i = 0; i < n; i++) {
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
}
/* As Quake III's GL_State between draws: the blend function (blending on)
 * and the depth function */
static void t_draw_blendfunc(int n)
{
    int i;
    t_draw(0);
    glEnable(GL_BLEND);
    for (i = 0; i < n; i++) {
        if (i & 1)
            glBlendFunc(GL_ONE, GL_ONE);
        else
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
    }
    glDisable(GL_BLEND);
}
static void t_draw_depthfunc(int n)
{
    int i;
    t_draw(0);
    for (i = 0; i < n; i++) {
        glDepthFunc((i & 1) ? GL_LEQUAL : GL_EQUAL);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
    }
    glDepthFunc(GL_LESS);
}
static void t_draw_prog(int n)
{
    int i;
    t_draw(0);
    for (i = 0; i < n; i++) {
        glUseProgram(prog[i & 1]);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
    }
    glUseProgram(prog[0]);
}
static void t_draw_tex(int n)
{
    int i;
    t_draw(0);
    for (i = 0; i < n; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i & 3]);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, 0);
    }
}
/* A draw after a new matrix and new pointers into another part of the
 * buffer (the state changes every draw of a game makes) */
static void t_draw_state(int n)
{
    int i;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    for (i = 0; i < n; i++) {
        mvp[12] = (float)(i & 7) * 0.001f;
        glUniformMatrix4fv(u_mvp[0], 1, GL_FALSE, mvp);
        pointers(i & 15);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, (void *)(ULONG)((i & 15) * NI * 2));
    }
}
/* One frame of Quake III's renderergl2 by its counts: per draw a program
 * now and then, a texture, uniforms, the vertices and indices streamed into
 * the buffers, three pointers, blending toggled, the draw. n: draws. */
static void t_mix(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int k = (i / 3) % 3, s = i & 15;
        if (i % 3 == 0) {
            glUseProgram(prog[k]);
            glUniformMatrix4fv(u_mvp[k], 1, GL_FALSE, mvp);
        }
        glBindTexture(GL_TEXTURE_2D, tex[i & 3]);
        glUniform4f(u_color[k], 1.0f, 1.0f, 1.0f, (i & 1) ? 1.0f : 0.5f);
        if (i & 1)
            glUniformMatrix4fv(u_mvp[k], 1, GL_FALSE, mvp);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, s * NV * VSTRIDE, NV * VSTRIDE, vdata);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, s * NI * 2, NI * 2, idata);
        pointers(s);
        if (i % 4 == 0)
            glEnable(GL_BLEND);
        else if (i % 4 == 2)
            glDisable(GL_BLEND);
        glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, (void *)(ULONG)(s * NI * 2));
    }
}

/* Runs a test in rounds of `per` calls, a swap after each round (outside the
 * time), for about ms milliseconds; the time a call. */
static double run(test_fn f, int per, int ms, ULONG *calls)
{
    double t = 0.0, start = now_us();
    ULONG c = 0;
    f(per); /* once untimed: the first draws compile and load */
    aglSwapBuffers();
    while (now_us() - start < ms * 1000.0) {
        double t0 = now_us();
        f(per);
        t += now_us() - t0;
        c += per;
        aglSwapBuffers();
    }
    glFinish();
    *calls = c;
    return c ? t / c : 0.0;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        test_fn f;
        int per;
    } tests[] = {
        {"glVertexAttribPointer", t_vap, 500},
        {"glEnable/glDisable", t_enable, 500},
        {"glBindBuffer", t_bindbuf, 500},
        {"glBindTexture", t_bindtex, 500},
        {"glUseProgram", t_useprog, 300},
        {"glUniform4f", t_uniform4f, 500},
        {"glUniformMatrix4fv", t_matrix, 500},
        {"glBufferSubData_960B", t_subdata, 200},
        {"glBufferData_960B", t_bufdata, 200},
        {"draw_same", t_draw, 100},
        {"draw_client_arrays", t_draw_client, 100},
        {"draw_blend_toggled", t_draw_blend, 100},
        {"draw_then_depth_clear", t_clear_mid, 20},
        {"draw_blendfunc_switched", t_draw_blendfunc, 100},
        {"draw_depthfunc_switched", t_draw_depthfunc, 100},
        {"draw_program_switched", t_draw_prog, 100},
        {"draw_texture_switched", t_draw_tex, 100},
        {"draw_matrix_pointers", t_draw_state, 100},
        {"frame_mix_per_draw", t_mix, 71},
    };
    struct BitMap *bm = 0;
    void *ctx = 0;
    ULONG err = 0, calls;
    int ms = 800, es = 0, k, i, ok = 0;
    const char *only = 0;
    double sum = 0.0;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "MS=", 3))
            ms = atoi(argv[i] + 3);
        else if (!strncmp(argv[i], "ES=", 3))
            es = atoi(argv[i] + 3);
        else if (!strncmp(argv[i], "TEST=", 5))
            only = argv[i] + 5;
    }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) {
        printf("no timer.device\n");
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    {
        struct EClockVal e;
        efreq = ReadEClock(&e);
    }
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    if (GfxBase)
        bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm || !BOpen()) {
        printf("needs a 32-bit bitmap and %s.library\n", LIB);
        goto out;
    }
#ifdef BENCH_OGLES2
    (void)es;
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_VSYNC, FALSE,
                                TAG_DONE);
#else
    if (es)
        ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_VSYNC,
                                    FALSE, OGLES2_CCT_PISTORM_ES_VERSION, (ULONG)es, TAG_DONE);
    else
        ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, OGLES2_CCT_DEPTH, 24, OGLES2_CCT_VSYNC,
                                    FALSE, TAG_DONE);
#endif
    if (!ctx) {
        printf("aglCreateContext2 failed (%lu)\n", (unsigned long)err);
        goto out;
    }
    aglMakeCurrent(ctx);
    for (k = 0; k < 3; k++)
        if (!(prog[k] = link(fs_src[k]))) {
            printf("program %d does not link\n", k);
            goto out;
        }
    setup();
    printf("drawbench %s %s, %d ms a test\n", LIB, (const char *)glGetString(GL_VERSION), ms);
    for (k = 0; k < (int)(sizeof(tests) / sizeof(tests[0])); k++) {
        double us;
        if (only && strcmp(only, tests[k].name))
            continue;
        us = run(tests[k].f, tests[k].per, ms, &calls);
        printf("TEST %s %-24s %8lu ns (%lu)\n", LIB, tests[k].name, (unsigned long)(us * 1000.0 + 0.5),
               (unsigned long)calls);
        if (k == (int)(sizeof(tests) / sizeof(tests[0])) - 1 && us > 0.0)
            printf("TEST %s %-24s %8lu fps of 71-draw frames (the calls alone)\n", LIB, "frame_mix",
                   (unsigned long)(1e6 / (us * 71) + 0.5));
        sum += us;
    }
    ok = glGetError() == GL_NO_ERROR;
    printf("TEST %s %-24s %s\n", LIB, "gl_error", ok ? "none" : "SET");
out:
    if (ctx)
        aglDestroyContext(ctx);
    BClose();
    if (bm)
        FreeBitMap(bm);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (TimerBase)
        CloseDevice((struct IORequest *)&tr);
    return ok ? 0 : 10;
}
