/*
 * swarmbench: Boing Swarm's step 1 (C:\GitHub\BOING_SWARM_PLAN.md,
 * docs/GLES3_PLAN.md "Plan: Boing Swarm"): how fast the Pi 4's GPU runs the
 * swarm's compute shaders through ogles3, before any art is made.
 *
 *   swarmbench [N=boids] [MODE=free|grid|vbo|draw] [MAX=n] [FRAMES=n]
 *              [WIDTH=w] [HEIGHT=h] [FULLSCREEN] [MSAA] [EVERY=n] [TIMING]
 *              [SIZE=percent]
 *
 * N boids (4096, 8192 or 16384; a multiple of 128) fly to their places on a
 * spinning ball of radius 3 and are drawn as small lights (textured
 * billboards added up), one instanced draw of 4-vertex strips; MSAA is off
 * unless asked. MODE:
 *   free  one dispatch: the pull to the ball, no neighbours; positions into
 *         an RGBA32F image the vertex shader reads with texelFetch
 *   grid  three: the neighbour grid (scan of the cells' counts in shared
 *         memory, scatter) and flocking over the 27 cells round each boid,
 *         an EVERY-th of the boids a frame (default 3), at most MAX
 *         neighbours looked at (default 8), which also counts each boid
 *         into its new cell with atomics
 *         (a count pass of its own on the first frame); the image as free
 *   vbo   free's dispatch, drawn from its storage buffer bound as an
 *         instanced vertex array (the library copies it back through the
 *         68k: the path the plan must avoid, measured)
 *   draw  no dispatch: the draw alone, from the image
 * Each dispatch is waited for by the library (ogles3/compute.c), so its
 * call's time is the GPU's time plus the call's own; after the swap a
 * glFinish (TIMING) keeps the render's time out of the next frame's first
 * dispatch; without it the render is waited for there.
 * At the end: frames a second and the mean time of each part a frame.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/graphics.h>
#include <proto/ogles3.h>
#include <GLES3/gl31.h>
#include "swarmbench_shaders.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;
struct GfxBase *GfxBase;
static struct timerequest timer_req;

#define CELLS 4096 /* 16^3, the shaders' grid */

enum { M_FREE, M_GRID, M_VBO, M_DRAW };
enum { T_FREE, T_COUNT, T_SCAN, T_SCATTER, T_FLOCK, T_DRAW, T_FINISH, T_N };
static const char *const part_name[T_N] = {"free", "count", "scan", "scatter", "flock", "draw+swap", "finish"};

static double now_s(void)
{
    struct TimeVal tv;
    GetSysTime((TimeVal_Type *)&tv);
    return tv.tv_secs + tv.tv_micro / 1e6;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("shader: %s\n", log);
    }
    return s;
}

static GLuint link(GLuint a, GLuint b)
{
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, a);
    if (b)
        glAttachShader(p, b);
    glBindAttribLocation(p, 0, "a_boid");
    glLinkProgram(p);
    glDeleteShader(a);
    if (b)
        glDeleteShader(b);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint compute(const char *src)
{
    return link(compile(GL_COMPUTE_SHADER, src), 0);
}

/* A dispatch of n boids (128 a group), timed into t. */
static void run(GLuint p, int groups, double *t)
{
    double a = now_s();
    glUseProgram(p);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    *t += now_s() - a;
}

/* The boids' light, 64x64 RGBA8, mipmapped: red the core and its star (four
 * rays and two fainter diagonals, a fifth of the core), green the glow. */
static GLuint make_flare(void)
{
    enum { S = 64 };
    static UBYTE px[S * S * 4];
    GLuint t;
    int x, y;
    for (y = 0; y < S; y++)
        for (x = 0; x < S; x++) {
            float u = (x + 0.5f) / S * 2.0f - 1.0f, v = (y + 0.5f) / S * 2.0f - 1.0f;
            float r2 = u * u + v * v, fade = r2 < 1.0f ? 1.0f - (float)sqrt(r2) : 0.0f;
            float du = (float)fabs(u + v) * 0.7071f, dv = (float)fabs(u - v) * 0.7071f;
            float star = ((float)exp(-fabs(v) * 45.0f) + (float)exp(-fabs(u) * 45.0f) +
                          0.5f * ((float)exp(-du * 45.0f) + (float)exp(-dv * 45.0f))) * fade;
            float core = (float)exp(-r2 * 70.0f) + star * 0.2f;
            float glow = r2 < 1.0f ? (1.0f - r2) * (1.0f - r2) : 0.0f;
            UBYTE *p = px + (y * S + x) * 4;
            p[0] = (UBYTE)(core > 1.0f ? 255 : core * 255.0f + 0.5f);
            p[1] = (UBYTE)(glow * 255.0f + 0.5f);
            p[2] = 0;
            p[3] = 255;
        }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void set_sim(GLuint p, int n, float dt, float spin, int max)
{
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_n"), n);
    glUniform1f(glGetUniformLocation(p, "u_dt"), dt);
    glUniform1f(glGetUniformLocation(p, "u_spin"), spin);
    glUniform3f(glGetUniformLocation(p, "u_centre"), 0.0f, 0.0f, 0.0f);
    glUniform1i(glGetUniformLocation(p, "u_max"), max);
}

int main(void)
{
    LONG args[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    struct RDArgs *rda;
    struct Window *win = NULL, *iw = NULL;
    void *ctx = NULL;
    ULONG err = 0, width = 0, height = 0;
    int n = 16384, mode = M_FREE, max = 8, every = 3, size = 100, w0 = 640, h0 = 480, rc = 20, i, f, max_frames = 300;
    BOOL full, msaa, timing, quit = FALSE, timer_open = FALSE;
    GLuint p_draw = 0, p_free = 0, p_count = 0, p_scan = 0, p_scatter = 0, p_flock = 0;
    GLuint boids[2] = {0, 0}, counts = 0, starts = 0, cells = 0, sorted = 0, img = 0, flare = 0;
    GLint u_vbo, u_vp, u_size;
    double t[T_N], start, a;
    float vp[16], *init;
    const char *mname[] = {"free", "grid", "vbo", "draw"};

    memset(t, 0, sizeof(t));
    rda = ReadArgs((STRPTR) "N/N,MODE/K,MAX/N,FRAMES/N,WIDTH/N,HEIGHT/N,FULLSCREEN/S,MSAA/S,EVERY/N,TIMING/S,SIZE/N", args, NULL);
    if (!rda) {
        printf("usage: swarmbench [N=boids] [MODE=free|grid|vbo|draw] [MAX=n] [FRAMES=n] [WIDTH=w] [HEIGHT=h] "
               "[FULLSCREEN] [MSAA] [EVERY=n]\n");
        return 20;
    }
    if (args[0])
        n = *(LONG *)args[0];
    if (args[1])
        for (i = 0; i < 4; i++)
            if (!strcmp((const char *)args[1], mname[i]))
                mode = i;
    if (args[2])
        max = *(LONG *)args[2];
    if (args[3])
        max_frames = *(LONG *)args[3];
    if (args[4])
        w0 = *(LONG *)args[4];
    if (args[5])
        h0 = *(LONG *)args[5];
    full = args[6] != 0;
    msaa = args[7] != 0; /* lights added up need no MSAA: off unless asked */
    timing = args[9] != 0;
    if (args[10])
        size = *(LONG *)args[10];
    if (args[8] && *(LONG *)args[8] >= 1)
        every = *(LONG *)args[8];
    FreeArgs(rda);
    n = (n + 127) & ~127;
    if (n < 128 || n > 65536) {
        printf("N: 128 to 65536\n");
        return 20;
    }

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39)))
        return 20;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0))
        goto out;
    timer_open = TRUE;
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES3Open()) {
        printf("needs %s\n", OGLES3NAME);
        goto out;
    }
    if (full) {
        ULONG id = INVALID_ID;
        if (!(GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39)))
            goto out;
        while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
            struct DimensionInfo dims;
            if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) || dims.MaxDepth < 24 ||
                dims.Nominal.MaxX - dims.Nominal.MinX + 1 != w0 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != h0)
                continue;
            ctx = aglCreateContextTags2(&err, OGLES3_CCT_MODEID, id, OGLES3_CCT_PISTORM_GET_WINDOW, (ULONG)&iw,
                                        OGLES3_CCT_DEPTH, 24, OGLES3_CCT_STENCIL, 0, OGLES3_CCT_PISTORM_MSAA,
                                        msaa ? 4 : 0, OGLES3_CCT_GET_WIDTH, (ULONG)&width, OGLES3_CCT_GET_HEIGHT,
                                        (ULONG)&height, OGLES3_CCT_PISTORM_ES_VERSION, 31, TAG_DONE);
        }
        if (!ctx || !iw) {
            printf("no %dx%d 32-bit screen mode\n", w0, h0);
            goto out;
        }
        ModifyIDCMP(iw, IDCMP_VANILLAKEY);
    } else {
        win = OpenWindowTags(NULL, WA_Title, (ULONG) "swarmbench", WA_InnerWidth, w0, WA_InnerHeight, h0,
                             WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY, TAG_DONE);
        if (!win)
            goto out;
        ctx = aglCreateContextTags2(&err, OGLES3_CCT_WINDOW, (ULONG)win, OGLES3_CCT_DEPTH, 24, OGLES3_CCT_STENCIL, 0,
                                    OGLES3_CCT_PISTORM_MSAA, msaa ? 4 : 0, OGLES3_CCT_GET_WIDTH, (ULONG)&width,
                                    OGLES3_CCT_GET_HEIGHT, (ULONG)&height, OGLES3_CCT_PISTORM_ES_VERSION, 31,
                                    TAG_DONE);
        if (!ctx) {
            printf("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
            goto out;
        }
        iw = win;
    }
    ActivateWindow(iw);
    aglMakeCurrent(ctx);
    printf("swarmbench: %d boids, MODE=%s, %lux%lu, MSAA %s, on %s\n", n, mname[mode], (unsigned long)width, (unsigned long)height, msaa ? "4x" : "off",
           (const char *)glGetString(GL_VERSION));
    if (mode == M_GRID)
        printf("  at most %d neighbours looked at a boid, each every %d frames\n", max, every);

    if (!(p_draw = link(compile(GL_VERTEX_SHADER, boid_vert), compile(GL_FRAGMENT_SHADER, boid_frag))) ||
        !(p_free = compute(free_comp)))
        goto out;
    if (mode == M_GRID && (!(p_count = compute(count_comp)) || !(p_scan = compute(scan_comp)) ||
                           !(p_scatter = compute(scatter_comp)) || !(p_flock = compute(flock_comp))))
        goto out;

    /* The boids start scattered round the ball (a fixed pseudo-random cloud). */
    if (!(init = (float *)calloc(n, 8 * sizeof(float))))
        goto out;
    {
        ULONG s = 12345;
        for (i = 0; i < n * 8; i += 8) {
            for (f = 0; f < 3; f++) {
                s = s * 1664525UL + 1013904223UL;
                init[i + f] = ((s >> 8) & 0xffff) / 65535.0f * 12.0f - 6.0f;
            }
        }
    }
    glGenBuffers(2, boids);
    for (i = 0; i < 2; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, boids[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, n * 32, init, GL_DYNAMIC_COPY);
    }
    free(init);
    if (mode == M_GRID) {
        glGenBuffers(1, &counts);
        glGenBuffers(1, &starts);
        glGenBuffers(1, &cells);
        glGenBuffers(1, &sorted);
        if (!(init = (float *)calloc(CELLS + 1, 4)))
            goto out;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, counts);
        glBufferData(GL_SHADER_STORAGE_BUFFER, CELLS * 4, init, GL_DYNAMIC_COPY);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, starts);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (CELLS + 1) * 4, init, GL_DYNAMIC_COPY);
        free(init);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, cells);
        glBufferData(GL_SHADER_STORAGE_BUFFER, n * 8, NULL, GL_DYNAMIC_COPY);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sorted);
        glBufferData(GL_SHADER_STORAGE_BUFFER, n * 4, NULL, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, counts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, starts);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, cells);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sorted);
    }
    glGenTextures(1, &img);
    glBindTexture(GL_TEXTURE_2D, img);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 128, n / 128);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindImageTexture(0, img, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);

    /* The camera 9 units in front of the ball, 45 degrees; the boid's
     * size from its share of the ball's surface. */
    memset(vp, 0, sizeof(vp));
    {
        float c = 1.0f / (float)tan(22.5 * 3.14159265 / 180.0), asp = (float)width / height, zn = 1.0f, zf = 30.0f;
        vp[0] = c / asp;
        vp[5] = c;
        vp[10] = (zf + zn) / (zn - zf);
        vp[11] = -1.0f;
        vp[14] = 2.0f * zf * zn / (zn - zf);
        /* the view, a step of -9 in z, folded in: column 3 += -9 * column 2 */
        vp[12] += -9.0f * vp[8];
        vp[13] += -9.0f * vp[9];
        vp[14] += -9.0f * vp[10];
        vp[15] += -9.0f * vp[11];
    }
    glUseProgram(p_draw);
    u_vbo = glGetUniformLocation(p_draw, "u_vbo");
    u_vp = glGetUniformLocation(p_draw, "u_vp");
    u_size = glGetUniformLocation(p_draw, "u_size");
    glUniformMatrix4fv(u_vp, 1, GL_FALSE, vp);
    /* Each boid's light with its glow 1.6 times as wide as its share of
     * the ball's surface (4 pi 3^2 / n): the cores apart, the glows met. */
    glUniform1f(u_size, size * 0.008f * (float)sqrt(4.0 * 3.14159265 * 9.0 / n));
    glUniform1i(u_vbo, mode == M_VBO);
    glUniform1i(glGetUniformLocation(p_draw, "u_boids"), 1);
    glUniform1i(glGetUniformLocation(p_draw, "u_flare"), 0);
    /* the camera looks down -z: its right and up are the world's x and y */
    glUniform3f(glGetUniformLocation(p_draw, "u_right"), 1.0f, 0.0f, 0.0f);
    glUniform3f(glGetUniformLocation(p_draw, "u_up"), 0.0f, 1.0f, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    flare = make_flare();
    /* The boids are lights: added up, no depth test (the far side shows
     * through the near one). */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glViewport(0, 0, width, height);
    glClearColor(0.02f, 0.02f, 0.05f, 1.0f);

    start = now_s();
    for (f = 0; f < max_frames && !quit; f++) {
        struct IntuiMessage *m;
        float spin = f * 0.02f, dt = 1.0f / 60.0f;
        GLuint src = boids[f & 1], dst = boids[(f + 1) & 1];
        while ((m = (struct IntuiMessage *)GetMsg(iw->UserPort)) != NULL) {
            if (m->Class == IDCMP_CLOSEWINDOW || (m->Class == IDCMP_VANILLAKEY && m->Code == 27))
                quit = TRUE;
            ReplyMsg((struct Message *)m);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, src);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dst);
        if (mode == M_FREE || mode == M_VBO) {
            set_sim(p_free, n, dt, spin, max);
            run(p_free, n / 128, &t[T_FREE]);
        } else if (mode == M_GRID) {
            if (f == 0) { /* later flocking counts the boids where they moved */
                glUseProgram(p_count);
                glUniform1i(glGetUniformLocation(p_count, "u_n"), n);
                run(p_count, n / 128, &t[T_COUNT]);
            }
            run(p_scan, 1, &t[T_SCAN]);
            glUseProgram(p_scatter);
            glUniform1i(glGetUniformLocation(p_scatter, "u_n"), n);
            run(p_scatter, n / 128, &t[T_SCATTER]);
            set_sim(p_flock, n, dt, spin, max);
            glUniform1i(glGetUniformLocation(p_flock, "u_frame"), f);
            glUniform1i(glGetUniformLocation(p_flock, "u_every"), every);
            run(p_flock, n / 128, &t[T_FLOCK]);
        }
        a = now_s();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(p_draw);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, img);
        glActiveTexture(GL_TEXTURE0);
        if (mode == M_VBO) {
            glBindBuffer(GL_ARRAY_BUFFER, dst);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, (const void *)0);
            glVertexAttribDivisor(0, 1);
        }
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
        aglSwapBuffers();
        t[T_DRAW] += now_s() - a;
        if (timing) { /* the render waited for here, not in the next dispatch */
            a = now_s();
            glFinish();
            t[T_FINISH] += now_s() - a;
        }
    }
    a = now_s() - start;
    printf("  %d frames in %.2f s: %.1f fps\n", f, a, f / a);
    printf("  a frame (ms):");
    for (i = 0; i < T_N; i++)
        if (t[i] > 0.0)
            printf(" %s %.2f", part_name[i], t[i] * 1000.0 / f);
    printf("\n");
    {
        double sim = 0.0;
        for (i = T_FREE; i <= T_FLOCK; i++)
            sim += t[i];
        printf("  simulation %.2f ms a frame (%.0f ns a boid)\n", sim * 1000.0 / f, sim * 1e9 / f / n);
    }
    rc = 0;

out:
    if (ctx) {
        GLuint progs[6] = {p_draw, p_free, p_count, p_scan, p_scatter, p_flock};
        for (i = 0; i < 6; i++)
            if (progs[i])
                glDeleteProgram(progs[i]);
        glDeleteBuffers(2, boids);
        if (counts) {
            glDeleteBuffers(1, &counts);
            glDeleteBuffers(1, &starts);
            glDeleteBuffers(1, &cells);
            glDeleteBuffers(1, &sorted);
        }
        if (img)
            glDeleteTextures(1, &img);
        if (flare)
            glDeleteTextures(1, &flare);
        aglDestroyContext(ctx);
    }
    if (win)
        CloseWindow(win);
    OGLES3Close();
    if (timer_open)
        CloseDevice((struct IORequest *)&timer_req);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
