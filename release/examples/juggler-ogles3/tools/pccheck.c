/*
 * Juggler RT's check on a PC (Linux, Mesa's EGL without a window: llvmpipe
 * or a GPU): the program's own renderer (juggler_gl.h) and shaders run as
 * on the Amiga, and the pictures go into PPM files.
 *
 *   pccheck OUTDIR [REFERENCE.rgb]
 *
 * It writes, into OUTDIR:
 *   robot-levels.ppm   robot.dat at 320x200 as rt2.c's 4-bit levels;
 *                      compared with REFERENCE (the original program's
 *                      robot.rgb, 320x200 RGB levels times 17, rows top first)
 *   robot-ham.ppm      the same through ham.comp, compared with the C HAM
 *   juggler-NN.ppm     the juggler at 12 times of the 3 s loop (2026, 640x480)
 *   juggler-es2.ppm    the ES 2.0 way at one of them, compared with ES 3.1's
 *   ele.ppm, dragon.ppm
 * and prints each comparison. Make: tools/pccheck.sh.
 *
 * MIT License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include "../juggler_scene.h"
#include "../juggler-ogles3_shaders.h"

#define J_LOG(...) fprintf(stderr, __VA_ARGS__)
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
#define J_NOW() now_s()
#define JGL_NO_SHOW /* the screen is the Amiga program's */
#include "../juggler_gl.h"


static struct jgl J;
static const char *outdir;

/* The texture t (w x h, RGBA8 or RGBA16F) as 8-bit RGB, rows top first. */
static unsigned char *grab(GLuint t, int w, int h, int fl, float gain)
{
    GLuint fbo;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    int x, y;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    if (fl) {
        float *px = malloc((size_t)w * h * 16);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int k;
                for (k = 0; k < 3; k++) {
                    float v = px[((size_t)(h - 1 - y) * w + x) * 4 + k] * gain;
                    rgb[((size_t)y * w + x) * 3 + k] = v >= 1.0f ? 255 : v <= 0.0f ? 0 : (unsigned char)(v * 255.0f + 0.5f);
                }
            }
        free(px);
    } else {
        unsigned char *px = malloc((size_t)w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                memcpy(rgb + ((size_t)y * w + x) * 3, px + ((size_t)(h - 1 - y) * w + x) * 4, 3);
        free(px);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return rgb;
}

static void save(const char *name, const unsigned char *rgb, int w, int h)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", outdir, name);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
}

/* Pixels that differ, and those more than one 4-bit level (17) apart. */
static void compare(const char *what, const unsigned char *a, const unsigned char *b, int n)
{
    int i, k, diff = 0, far = 0;
    for (i = 0; i < n; i++) {
        int d = 0;
        for (k = 0; k < 3; k++) {
            int e = abs(a[3 * i + k] - b[3 * i + k]);
            if (e > d)
                d = e;
        }
        diff += d > 0;
        far += d > 17;
    }
    printf("%s: %d of %d pixels differ (%.2f%%), %d by more than a level\n", what, diff, n, 100.0 * diff / n, far);
}

static int load(struct jscene *sc, const char *text, int juggler)
{
    const char *err;
    if (!j_parse(sc, text, &err)) {
        printf("scene: %s\n", err);
        return 0;
    }
    sc->juggler = juggler;
    return 1;
}

static void frame_of(const struct jscene *sc, float aspect, struct jframe *f)
{
    memset(f, 0, sizeof(*f));
    j_camera(sc->obs, sc->alt, sc->az, sc->focal, aspect, &f->cam);
    f->bounces = 4;
    f->blend = 1.0f;
}

int main(int argc, char **argv)
{
    static const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    static const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLint n;
    struct jscene sc;
    struct jframe f;
    float gain;
    GLuint t;
    unsigned char *a, *b;
    int i;
    double t0;

    if (argc < 2) {
        fprintf(stderr, "usage: pccheck OUTDIR [robot.rgb]\n");
        return 2;
    }
    outdir = argv[1];
    dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "no surfaceless EGL display (0x%x)\n", eglGetError());
        return 1;
    }
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
        cfg = EGL_NO_CONFIG_KHR; /* EGL_KHR_no_config_context */
    if (!(ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr)) ||
        !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "no OpenGL ES 3.1 context without a surface (0x%x)\n", eglGetError());
        return 1;
    }
    printf("%s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (!jgl_init(&J))
        return 1;

    /* robot.dat as the original drew it: 320x200, the view plane 4:3 */
    if (!load(&sc, robot_dat, 0))
        return 1;
    printf("robot.dat: %d objects, %d spheres\n", sc.nobj, sc.nsph);
    jgl_scene(&J, &sc);
    printf("  lamp colour after exposure %.1f %.1f %.1f\n", J.sc.lampcol[0], J.sc.lampcol[1], J.sc.lampcol[2]);
    jgl_size(&J, 320, 200);
    frame_of(&J.sc, 4.0f / 3.0f, &f);
    f.levels = 1;
    f.jitter[0] = -0.5f, f.jitter[1] = 0.5f; /* rt1.c's rays: x = i, y = the row from the top */
    t0 = now_s();
    t = jgl_frame(&J, &f, &gain);
    glFinish();
    printf("  1987 picture (trace + HAM): %.1f ms here; palette %d colours\n", (now_s() - t0) * 1e3, J.pal.n);
    a = grab(J.lv, 320, 200, 0, 1.0f);
    save("robot-levels.ppm", a, 320, 200);
    if (argc > 2) {
        FILE *rf = fopen(argv[2], "rb");
        unsigned char *ref = malloc(320 * 200 * 3);
        if (rf && fread(ref, 3, 320 * 200, rf) == 320 * 200) {
            /* ssg's dump is its light times 128, plus 4: back to rt2.c's
             * levels (16 times the light, rounded) */
            int k;
            save("robot-reference.ppm", ref, 320, 200);
            for (k = 0; k < 320 * 200 * 3; k++) {
                int v = (ref[k] - 4 + 4) / 8;
                ref[k] = (unsigned char)((v > 15 ? 15 : v < 0 ? 0 : v) * 17);
            }
            compare("  levels against the original's (ssg's robot.rgb)", a, ref, 320 * 200);
        } else
            printf("  no reference %s\n", argv[2]);
        if (rf)
            fclose(rf);
        free(ref);
    }
    b = grab(t, 320, 200, 0, 1.0f);
    save("robot-ham.ppm", b, 320, 200);
    { /* ham.comp against juggler_scene.h's HAM with the same palette */
        int *lv = malloc(320 * 3 * sizeof(int)), *out = malloc(320 * 3 * sizeof(int)), x, y, k;
        unsigned char *c = malloc(320 * 200 * 3);
        for (y = 0; y < 200; y++) {
            for (x = 0; x < 320 * 3; x++)
                lv[x] = a[y * 320 * 3 + x] / 17;
            j_ham_row(&J.pal, lv, 320, out, 0);
            for (x = 0; x < 320; x++)
                for (k = 0; k < 3; k++)
                    c[(y * 320 + x) * 3 + k] = (unsigned char)(out[3 * x + k] * 17);
        }
        compare("  ham.comp against the C HAM", b, c, 320 * 200);
        free(lv), free(out), free(c);
    }
    free(a), free(b);

    /* the juggling's clearances over the loop: ball to ball, ball to head */
    load(&sc, robot_dat, 1);
    {
        float pts[J_POINTS][4], bb = 1e9f, bh = 1e9f, top = 0.0f;
        int s, p, q;
        for (s = 0; s < 1200; s++) {
            j_juggle((const float(*)[4])sc.pt, s * 6.0f * J_H / 1200, pts);
            for (p = 0; p < 3; p++) {
                float h = j_dist(pts[p], pts[3]) - pts[p][3] - pts[3][3];
                if (h < bh)
                    bh = h;
                if (pts[p][2] + pts[p][3] > top)
                    top = pts[p][2] + pts[p][3];
                for (q = 0; q < p; q++) {
                    float d = j_dist(pts[p], pts[q]) - pts[p][3] - pts[q][3];
                    if (d < bb)
                        bb = d;
                }
            }
        }
        printf("juggling: balls at least %.2f apart, %.2f from the head; their top at z %.2f\n", bb, bh, top);
    }

    /* the juggler, 2026, 640x480 */
    jgl_scene(&J, &sc);
    jgl_size(&J, 640, 480);
    for (i = 0; i < 12; i++) {
        char name[32];
        frame_of(&J.sc, 4.0f / 3.0f, &f);
        f.time = i * 6.0f * J_H / 12.0f;
        t0 = now_s();
        t = jgl_frame(&J, &f, &gain);
        glFinish();
        if (i == 0)
            printf("juggler 640x480: %.1f ms a picture here\n", (now_s() - t0) * 1e3);
        a = grab(t, 640, 480, 1, gain);
        snprintf(name, sizeof(name), "juggler-%02d.ppm", i);
        save(name, a, 640, 480);
        if (i == 4) { /* the ES 2.0 way at the same time */
            f.es2 = 1;
            t = jgl_frame(&J, &f, &gain);
            b = grab(t, 640, 480, 0, 1.0f);
            save("juggler-es2.ppm", b, 640, 480);
            compare("  ES 2.0 way against ES 3.1 (a third into the loop)", a, b, 640 * 480);
            free(b);
            /* the 68k's juggling against animate.comp's */
            {
                struct jsphere g[J_MAX_SPHERES];
                float m = 0.0f;
                int s, k;
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, J.b_sph);
                f.es2 = 0;
                jgl_frame(&J, &f, &gain);
                memcpy(g, glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, J.sc.nsph * sizeof(struct jsphere),
                                           GL_MAP_READ_BIT),
                       J.sc.nsph * sizeof(struct jsphere));
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                for (s = 0; s < J.sc.nsph; s++)
                    for (k = 0; k < 4; k++) {
                        float e = g[s].p[k] - J.sp[s].p[k];
                        if (e < 0)
                            e = -e;
                        if (e > m)
                            m = e;
                    }
                printf("  animate.comp against j_juggle: %d spheres, largest difference %g\n", J.sc.nsph, m);
            }
        }
        free(a);
    }

    /* the other two scenes, still, 2026 */
    {
        static const char *const names[2] = {"ele.ppm", "dragon.ppm"};
        const char *texts[2];
        texts[0] = ele_dat, texts[1] = dragon_dat;
        for (i = 0; i < 2; i++) {
            if (!load(&sc, texts[i], 0))
                continue;
            jgl_scene(&J, &sc);
            frame_of(&J.sc, 4.0f / 3.0f, &f);
            t0 = now_s();
            t = jgl_frame(&J, &f, &gain);
            glFinish();
            printf("%s: %d objects, %d spheres, %.1f ms\n", names[i], sc.nobj, sc.nsph, (now_s() - t0) * 1e3);
            a = grab(t, 640, 480, 1, gain);
            save(names[i], a, 640, 480);
            free(a);
        }
    }
    printf("GL error 0x%x\n", glGetError());
    return 0;
}
