/*
 * Boing Ocean for OpenGL ES 3: Boing Ocean (../boingocean.c) remade with what
 * ogles3.library's OpenGL ES 3.0 contexts give, its ES 2.0 way kept beside
 * it (key E) to compare the two in one run.
 *
 * The ES 3 way:
 *   - the coarse water in half floats (no 8-bit steps in the slope), read
 *     with textureLod in the march; the ripples a small tiling texture
 *     drawn once a frame (two reads a pixel for three sines);
 *   - the sky a half-float cube map (one read for every reflection, the
 *     sun's glow in it), with clouds drifting through a 3D noise texture;
 *   - the ball's checkerboard a mipmapped cube map read with its own
 *     direction (no atan/asin per pixel, no crawling edges);
 *   - the frame's data one uniform block, written once a frame; no vertex
 *     data (gl_VertexID); HDR: the scene in half floats, the sun's glints on
 *     the ripples and the ball's highlight blooming, then ACES and a vignette;
 *   - caustics playing on the ball under the water.
 * Keys: E the ES 2.0 way  R ripples (texture or sines)  G glints  C clouds
 *       U caustics  B bloom  1-4 the render scale (1/1..1/4)  0 75%: the ES 3
 *       way's scene drawn at 3/4 of the window and brought up to it
 *       by the pass that tone maps it and adds the bloom
 *
 * Boing Ocean's own description follows; its keys and arguments still hold
 * (and ES2 starts the ES 2.0 way), but dragging only turns the camera round
 * the ball: its height stays.
 *
 * ogles3.library example 3: Boing Ocean. The Amiga Boing ball floating on a
 * ray-marched ocean, drawn entirely by one fragment shader.
 *
 * Shows a heavy shader on ogles3 and how to make it affordable:
 *   - everything that is the same for every pixel is worked out once a frame
 *     on the CPU and passed as uniforms: the camera, the sun, the sky light,
 *     the ball's float (the mean water height under it and the slope there)
 *     and the orientation of its checkerboard;
 *   - what is the same for many pixels is drawn once a frame into small
 *     textures (framebuffer objects) that the scene reads: the coarse water
 *     height over the sea around the ball (the ray march then takes one
 *     texture read a step instead of eight wave octaves), and a table of the
 *     sky's colour by height and angle to the sun (for every reflection);
 *   - the view ray comes from the vertex shader, interpolated across one
 *     full-screen triangle, instead of a per-pixel camera matrix;
 *   - keys 1 to 4 draw the scene at a fraction of the window's size (1/1 ..
 *     1/4; 1/1 at start) into a texture that a second, tiny shader scales up.
 * Drag with the left mouse button to turn and raise the camera; it also
 * circles the ball by itself. The window's title shows the frame rate.
 * Close the window, press Esc or Ctrl-C to quit (Esc or Ctrl-C full screen).
 *
 *   boingocean [WIDTH=w] [HEIGHT=h] [SCALE=1..4] [FRAMES=n] [FULLSCREEN]
 * FRAMES quits after n frames and prints the average frame rate.
 * FULLSCREEN opens a 32-bit screen of WIDTH x HEIGHT (640 x 480 by default)
 * that ogles3 draws straight into, with no window to blit into; input then
 * comes from the screen's backdrop window (OGLES3_CCT_PISTORM_GET_WINDOW).
 *
 * Build (see the Makefile):
 *   m68k-amigaos-gcc -O2 -fno-builtin -I../include boingocean.c -L../lib -logles3 -lm881 -o boingocean
 *
 * MIT License. The water's wave model and sky are afl_ext's "Very fast
 * procedural ocean" (https://www.shadertoy.com/view/MdXyzX, MIT License,
 * Copyright (c) 2017-2024 afl_ext); see boingocean_shaders.h.
 */
#include <stdio.h>
#include <stdarg.h>
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
#include <GLES3/gl3.h>
#include <string.h>
#include "boingocean_shaders.h" /* Boing Ocean's own (../): the ES 2.0 way */
#include "boingocean-ogles3_shaders.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;
struct GfxBase *GfxBase;
static struct timerequest timer_req;

#define WATER_DEPTH 1.0f
#define DRAG_MULT 0.38f
#define BALL_R 0.8f
#define CAM_DIST 4.5f
#define PI 3.14159265f

typedef struct { float x, y, z; } vec3;

static vec3 v3(float x, float y, float z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static vec3 add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static vec3 normalize(vec3 a) { return scale(a, 1.0f / (float)sqrt(dot(a, a))); }

/* The shader's wavesMarch(), on the CPU: the water height the ball floats
 * on matches the surface the GPU draws. */
static float waves(float px, float pz, float t)
{
    float phase = (float)sqrt(px * px + pz * pz) * 0.1f;
    float iter = 0.0f, freq = 1.0f, tmul = 2.0f, weight = 1.0f, sum = 0.0f, weights = 0.0f;
    int i;
    for (i = 0; i < 8; i++) { /* as the shader's wavesMarch */
        float dx = (float)sin(iter), dz = (float)cos(iter);
        float x = (dx * px + dz * pz) * freq + t * tmul + phase;
        float wave = (float)exp(sin(x) - 1.0);
        float d = -wave * (float)cos(x);
        px += dx * d * weight * DRAG_MULT;
        pz += dz * d * weight * DRAG_MULT;
        sum += wave * weight;
        weights += weight;
        weight *= 0.8f;
        freq *= 1.18f;
        tmul *= 1.07f;
        iter += 1232.399963f;
    }
    return sum / weights * WATER_DEPTH - WATER_DEPTH;
}

/* The shader's atmosphere() towards straight up: the ball's sky light. */
static vec3 sky_up(vec3 sun)
{
    float st = 1.0f / 1.1f, st2 = 1.0f / (sun.y * 11.0f + 1.0f);
    float raysundt = sun.y * sun.y;
    float sundt = (float)pow(sun.y > 0.0f ? sun.y : 0.0f, 8.0);
    float mymie = sundt * st * 0.2f;
    float k[3] = {5.5f / 22.4f, 13.0f / 22.4f, 1.0f}, c[3], o[3];
    int i;
    for (i = 0; i < 3; i++) {
        float sc = 1.0f + (((1.0f - k[i]) > 0.0f ? 1.0f - k[i] : 0.0f) - 1.0f) * st2; /* mix(1, max(0, 1-k), st2) */
        float b = k[i] * sc;
        float b2 = b - k[i] * 22.4f * 0.002f * (st - 6.0f * sun.y * sun.y);
        if (b2 < 0.0f)
            b2 = 0.0f;
        b2 *= st * (0.24f + raysundt * 0.24f);
        c[i] = sc;
        o[i] = (b2 * 1.0f + mymie * c[i]) * 0.5f; /* (1 - ray.y)^3 is 0 straight up */
    }
    return v3(o[0] * 0.6f, o[1] * 0.6f, o[2] * 0.6f);
}

struct frame_uniforms {
    vec3 sun, eye, fw, rt, up, ball, px, py, pz, ambient;
};

/* Everything the shader needs that does not change across the screen. */
static void frame_setup(struct frame_uniforms *u, float t, float yaw_drag, float my)
{
    const float s = 0.6f * BALL_R, tilt = 0.35f;
    float turn = 0.6f;
    float h0, hx1, hx0, hz1, hz0, mean, sx, sz, yaw, pitch, ct, st, cs, ss;
    vec3 target = v3(0.0f, -0.45f * WATER_DEPTH, 0.0f), up, t1, t2;

    /* The ball floats: the mean water height across its waterline, tilted
     * with the slope there, drifting a little down it. */
    h0 = waves(0.0f, 0.0f, t);
    hx1 = waves(s, 0.0f, t);
    hx0 = waves(-s, 0.0f, t);
    hz1 = waves(0.0f, s, t);
    hz0 = waves(0.0f, -s, t);
    mean = (h0 * 2.0f + hx1 + hx0 + hz1 + hz0) / 6.0f;
    sx = (hx1 - hx0) / (2.0f * s);
    sz = (hz1 - hz0) / (2.0f * s);
    up = normalize(v3(-sx * 0.7f, 1.0f, -sz * 0.7f));
    u->ball = v3(-sx * 0.08f, mean - 0.2f * BALL_R, -sz * 0.08f);

    /* Its checkerboard's axes: the ball's frame, the pattern leaning
     * (tilt) and turned so the camera sees it side on. */
    t1 = normalize(v3(up.y, -up.x, 0.0f)); /* cross(up, z) */
    t2 = cross(t1, up);
    turn += t * 0.4f; /* it spins round its up axis */
    ct = (float)cos(tilt), st = (float)sin(tilt), cs = (float)cos(turn), ss = (float)sin(turn);
    u->px = add(add(scale(t1, cs * ct), scale(up, -cs * st)), scale(t2, ss));
    u->py = add(scale(t1, st), scale(up, ct));
    u->pz = add(add(scale(t1, -ss * ct), scale(up, ss * st)), scale(t2, cs));

    /* The camera stands still (the mouse turns it) and looks at the ball,
     * which keeps the full shader's rectangle round it small; the sun is
     * low behind the ball, so its glints run across the water to the eye. */
    yaw = yaw_drag + 0.4363f * (float)sin(t * 0.4f); /* 25 degrees either way */
    target = v3(u->ball.x, -0.2f * BALL_R, u->ball.z); /* not its bobbing: that shows */
    pitch = 0.12f + (0.8f - 0.12f) * my;
    u->eye = add(target, scale(v3((float)sin(yaw) * (float)cos(pitch), (float)sin(pitch),
                                  -(float)cos(yaw) * (float)cos(pitch)), CAM_DIST));
    if (u->eye.y < 0.6f)
        u->eye.y = 0.6f;
    u->fw = normalize(sub(target, u->eye));
    u->rt = normalize(v3(u->fw.z, 0.0f, -u->fw.x)); /* cross(y, fw) */
    u->up = cross(u->fw, u->rt);
    u->sun = v3(-(float)sin(yaw_drag), 0.0f, (float)cos(yaw_drag)); /* behind the ball from the sway's middle */
    u->sun = normalize(v3(u->sun.x, 0.32f, u->sun.z));
    u->ambient = sky_up(u->sun);
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

static GLuint link_program(const char *vsrc, const char *fsrc)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc), fs = compile(GL_FRAGMENT_SHADER, fsrc);
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
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

/* An RGBA8 texture to draw into (a framebuffer object), linear filtered,
 * clamped at its edges. */
static BOOL make_rt(GLuint *tex, GLuint *fbo, int w, int h)
{
    if (*fbo) {
        glDeleteFramebuffers(1, fbo);
        glDeleteTextures(1, tex);
        *fbo = *tex = 0;
    }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("a render target (%dx%d) is not complete\n", w, h);
        return FALSE;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return TRUE;
}

/* The coarse water height and slope (the shaders' EXTENT: -30..30 in x and
 * z), drawn each frame, and the sky table, drawn every 16th frame (the sun
 * moves slowly). */
#define HEIGHT_RES 160
#define EXTENT 30.0f
#define SKY_RES 64
static GLuint height_tex, height_fbo, sky_tex, sky_fbo;

/* The texture the scene is drawn into at the window's size / div; none at
 * 1/1, where it goes to the window directly. */
static GLuint target_tex, target_fbo;
static int target_w, target_h;

static BOOL make_target(int w, int h, int div)
{
    target_w = (w + div - 1) / div;
    target_h = (h + div - 1) / div;
    if (target_w < 1)
        target_w = 1;
    if (target_h < 1)
        target_h = 1;
    if (div == 1) {
        if (target_fbo) {
            glDeleteFramebuffers(1, &target_fbo);
            glDeleteTextures(1, &target_tex);
            target_fbo = target_tex = 0;
        }
        return TRUE;
    }
    return make_rt(&target_tex, &target_fbo, target_w, target_h);
}

static double now_s(void)
{
    /* struct TimeVal: the same fields with every compiler's C library
     * (a POSIX one may define its own struct timeval). */
    struct TimeVal tv;
    GetSysTime((TimeVal_Type *)&tv);
    return tv.tv_secs + tv.tv_micro / 1e6;
}

struct input {
    BOOL quit, resized, dragging;
    BOOL es2, ripples, glints, clouds, caustics, bloom;
    int div;           /* render scale: the window's size / div */
    int pct;           /* the ES 3 way's render scale, percent (100 at start) */
    WORD last_x, last_y;
    float yaw, my;     /* the camera, from dragging */
};

/* Status lines: printed, then the window with the keys made active again.
 * Started from an icon or a launcher, stdout is a console window that the
 * first line opens (and opens again after it is closed), and an opened
 * console takes the keyboard: the keys (Esc, E) went to it, not here.
 * Errors stay plain printf. */
static struct Window *key_window; /* set once the window with the keys is open */

static void say(const char *fmt, ...)
{
    va_list ap;
    if (!Cli())
        return; /* from Workbench: no one reads it (libnix's console would open and wait) */
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (key_window)
        ActivateWindow(key_window);
}

static void events(struct Window *w, struct input *in)
{
    struct IntuiMessage *m;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        in->quit = TRUE;
    /* Full screen (the library's backdrop window on a screen of its own):
     * while that screen is in front, the keys are ours. A console a
     * launcher opened on the Workbench took them, and with them Esc. */
    if ((w->Flags & WFLG_BACKDROP) && !(w->Flags & WFLG_WINDOWACTIVE) && w->WScreen == IntuitionBase->FirstScreen)
        ActivateWindow(w);
    while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
        switch (m->Class) {
        case IDCMP_CLOSEWINDOW:
            in->quit = TRUE;
            break;
        case IDCMP_VANILLAKEY:
            if (m->Code == 27)
                in->quit = TRUE;
            else if (m->Code >= '1' && m->Code <= '4') {
                in->div = m->Code - '0';
                in->pct = 100 / in->div;
                in->resized = TRUE;
            } else if (m->Code == '0') { /* the ES 3 way at 75% */
                in->pct = 75;
                in->resized = TRUE;
            } else {
                switch (m->Code | 0x20) { /* either case */
                case 'e': in->es2 = !in->es2; say("%s\n", in->es2 ? "the ES 2.0 way" : "OpenGL ES 3"); break;
                case 'r': in->ripples = !in->ripples; break;
                case 'g': in->glints = !in->glints; break;
                case 'c': in->clouds = !in->clouds; break;
                case 'u': in->caustics = !in->caustics; break;
                case 'b': in->bloom = !in->bloom; break;
                }
            }
            break;
        case IDCMP_NEWSIZE:
            in->resized = TRUE;
            break;
        case IDCMP_MOUSEBUTTONS:
            in->dragging = m->Code == SELECTDOWN;
            in->last_x = m->MouseX;
            in->last_y = m->MouseY;
            break;
        case IDCMP_MOUSEMOVE:
            if (in->dragging) {
                /* Round the ball only: the camera keeps its height. */
                in->yaw += (m->MouseX - in->last_x) * 0.01f;
                in->last_x = m->MouseX;
                in->last_y = m->MouseY;
            }
            break;
        }
        ReplyMsg((struct Message *)m);
    }
}

/* ==== The OpenGL ES 3 way ==== */

/* The frame's data, as the shaders' uniform block Frame lays it out (std140,
 * vec4s only): written once a frame, read by every program. */
struct frame_block {
    float sun[4], eye[4], fw[4], rt[4], up[4], ball[4], px[4], py[4], pz[4], ambient[4], mode[4], post[4];
};

#define CHECKER_RES 256
#define RIPPLE_RES 128
#define CAUST_RES 128
#define NOISE_RES 32

struct es3 {
    GLuint height, ripple, caust, sky, ocean, far, copy, bright, blur, final;
    GLint sky_face, blur_dir, ocean_out, far_out;
    GLuint ubo, vao;
    GLuint height_tex, slope_tex, height_fbo; /* the water: height (R16F); slope */
    GLuint ripple_tex, ripple_fbo;    /* the ripples, tiling (ripple3.frag) */
    GLuint caust_tex, caust_fbo;      /* the caustics, tiling (caust3.frag) */
    GLuint sky_tex, sky_fbo[6];
    GLuint checker, noise;
    GLuint scene_tex, scene_fbo, bloom_tex[2], bloom_fbo[2];
    GLuint half_tex, half_fbo; /* the far water at half the size */
    int pct;
    int sw, sh, bw, bh;
};

static void tex_params(GLenum target, GLenum filter, GLenum wrap)
{
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter == GL_LINEAR_MIPMAP_LINEAR ? GL_LINEAR : filter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
    if (target != GL_TEXTURE_2D)
        glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap);
}

/* A framebuffer object drawing into a 2D texture or a cube map's face. */
static BOOL fbo_for(GLuint *fbo, GLenum target, GLuint tex)
{
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("a render target is not complete (0x%x)\n", (unsigned)target);
        return FALSE;
    }
    return TRUE;
}

static GLuint link3(const char *vs, const char *fs)
{
    GLuint p = link_program(vs, fs), idx;
    if (!p)
        return 0;
    idx = glGetUniformBlockIndex(p, "Frame");
    if (idx != GL_INVALID_INDEX)
        glUniformBlockBinding(p, idx, 0);
    return p;
}

static void unit(GLuint p, const char *name, int u)
{
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, name), u);
}

/* The render-scale targets: the HDR scene and the bloom's two quarters. */
static void es3_free_targets(struct es3 *e)
{
    int k;
    if (e->scene_fbo)
        glDeleteFramebuffers(1, &e->scene_fbo), glDeleteTextures(1, &e->scene_tex);
    if (e->half_fbo)
        glDeleteFramebuffers(1, &e->half_fbo), glDeleteTextures(1, &e->half_tex);
    e->half_fbo = e->half_tex = 0;
    for (k = 0; k < 2; k++)
        if (e->bloom_fbo[k])
            glDeleteFramebuffers(1, &e->bloom_fbo[k]), glDeleteTextures(1, &e->bloom_tex[k]);
    e->scene_fbo = e->scene_tex = e->bloom_fbo[0] = e->bloom_fbo[1] = e->bloom_tex[0] = e->bloom_tex[1] = 0;
}

static BOOL es3_targets(struct es3 *e, int w, int h, int pct)
{
    int k;
    es3_free_targets(e);
    e->sw = w * pct / 100, e->sh = h * pct / 100;
    e->bw = (e->sw + 3) / 4, e->bh = (e->sh + 3) / 4;
    e->pct = pct;
    /* The scene in floats (HDR, R11F_G11F_B10F: half RGBA16F's bandwidth),
     * for the bloom and below 1/1; without either it goes straight into the
     * window, tone mapped by its own shader. */
    glGenTextures(1, &e->scene_tex);
    glBindTexture(GL_TEXTURE_2D, e->scene_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R11F_G11F_B10F, e->sw, e->sh);
    tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_CLAMP_TO_EDGE);
    if (!fbo_for(&e->scene_fbo, GL_TEXTURE_2D, e->scene_tex))
        return FALSE;
    /* The far water, away from the ball, at half the size: blown up into
     * the scene with a linear blit, soft as out of focus behind the ball. */
    glGenTextures(1, &e->half_tex);
    glBindTexture(GL_TEXTURE_2D, e->half_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R11F_G11F_B10F, (e->sw + 1) / 2, (e->sh + 1) / 2);
    tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_CLAMP_TO_EDGE);
    if (!fbo_for(&e->half_fbo, GL_TEXTURE_2D, e->half_tex))
        return FALSE;
    for (k = 0; k < 2; k++) {
        glGenTextures(1, &e->bloom_tex[k]);
        glBindTexture(GL_TEXTURE_2D, e->bloom_tex[k]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R11F_G11F_B10F, e->bw, e->bh);
        tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_CLAMP_TO_EDGE);
        if (!fbo_for(&e->bloom_fbo[k], GL_TEXTURE_2D, e->bloom_tex[k]))
            return FALSE;
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return TRUE;
}

/* The direction through texel (s, t) of cube face f, as GL lays a cube out
 * (the shaders' face_dir). */
static vec3 cube_dir(int f, float s, float t)
{
    float c = s * 2.0f - 1.0f, d = t * 2.0f - 1.0f;
    switch (f) {
    case 0: return v3(1.0f, -d, -c);
    case 1: return v3(-1.0f, -d, c);
    case 2: return v3(c, 1.0f, d);
    case 3: return v3(c, -1.0f, -d);
    case 4: return v3(c, -d, 1.0f);
    default: return v3(-c, -d, -1.0f);
    }
}

static BOOL es3_setup(struct es3 *e, int w, int h, int pct)
{
    static UBYTE noise[NOISE_RES * NOISE_RES * NOISE_RES];
    static UBYTE face[CHECKER_RES * CHECKER_RES];
    ULONG seed = 2026;
    int f, i, j;

    if (!(e->height = link3(screen3_vert, height3_frag)) || !(e->sky = link3(screen3_vert, sky3_frag)) ||
        !(e->ocean = link3(ocean3_vert, ocean3_frag)) || !(e->far = link3(ocean3_vert, oceanfar3_frag)) ||
        !(e->ripple = link3(screen3_vert, ripple3_frag)) || !(e->caust = link3(screen3_vert, caust3_frag)) ||
        !(e->copy = link3(screen3_vert, copy3_frag)) || !(e->bright = link3(screen3_vert, bright3_frag)) || !(e->blur = link3(screen3_vert, blur3_frag)) ||
        !(e->final = link3(screen3_vert, final3_frag)))
        return FALSE;
    e->sky_face = glGetUniformLocation(e->sky, "u_face");
    e->blur_dir = glGetUniformLocation(e->blur, "u_dir");
    /* Texture units: 0 the height (or a pass's source), 1 the sky, 2 the
     * unused, 3 the checkerboard, 4 the noise, 5 the bloom, 6 the
     * water's slope, 7 the ripples, 8 the caustics. */
    unit(e->ocean, "u_caust", 8);
    unit(e->sky, "u_noise", 4);
    unit(e->ocean, "u_height", 0);
    unit(e->ocean, "u_sky", 1);
    unit(e->ocean, "u_checker", 3);
    unit(e->ocean, "u_slope", 6);
    e->ocean_out = glGetUniformLocation(e->ocean, "u_out");
    e->far_out = glGetUniformLocation(e->far, "u_out");
    unit(e->ocean, "u_ripple", 7);
    unit(e->far, "u_height", 0);
    unit(e->far, "u_sky", 1);
    unit(e->far, "u_slope", 6);
    unit(e->far, "u_ripple", 7);
    unit(e->bright, "u_tex", 0);
    unit(e->copy, "u_tex", 0);
    unit(e->blur, "u_tex", 0);
    unit(e->final, "u_scene", 0);
    unit(e->final, "u_bloom", 5);

    glGenBuffers(1, &e->ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, e->ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(struct frame_block), NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, e->ubo);
    glGenVertexArrays(1, &e->vao); /* no arrays: gl_VertexID, gl_InstanceID */

    /* The coarse water: two half-float targets drawn at once, the height
     * alone (16 bits a texel: the march reads it 8 times a pixel, and a
     * 64-bit texel is read at half the rate) and the slope (read
     * once, where the ray hits). */
    {
        static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glGenTextures(1, &e->height_tex);
        glBindTexture(GL_TEXTURE_2D, e->height_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16F, HEIGHT_RES, HEIGHT_RES);
        tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_CLAMP_TO_EDGE);
        glGenTextures(1, &e->slope_tex);
        glBindTexture(GL_TEXTURE_2D, e->slope_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, HEIGHT_RES, HEIGHT_RES);
        tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_CLAMP_TO_EDGE);
        if (!fbo_for(&e->height_fbo, GL_TEXTURE_2D, e->height_tex))
            return FALSE;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, e->slope_tex, 0);
        glDrawBuffers(2, both);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            printf("the water's targets are not complete\n");
            return FALSE;
        }
    }

    /* The sky: a half-float cube map, a framebuffer a face. */
    glGenTextures(1, &e->sky_tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, e->sky_tex);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA16F, SKY_RES, SKY_RES);
    tex_params(GL_TEXTURE_CUBE_MAP, GL_LINEAR, GL_CLAMP_TO_EDGE);
    for (f = 0; f < 6; f++)
        if (!fbo_for(&e->sky_fbo[f], GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, e->sky_tex))
            return FALSE;

    /* The checkerboard by direction: 16 squares round, 8 from pole to
     * pole (as boingocean_fs works them out per pixel), as the distance to
     * its edges, mipmapped. */
    glGenTextures(1, &e->checker);
    glBindTexture(GL_TEXTURE_CUBE_MAP, e->checker);
    for (f = 0; f < 6; f++) {
        for (j = 0; j < CHECKER_RES; j++)
            for (i = 0; i < CHECKER_RES; i++) {
                vec3 d = normalize(cube_dir(f, (i + 0.5f) / CHECKER_RES, (j + 0.5f) / CHECKER_RES));
                float lon = (float)atan2(d.z, d.x) * (16.0f / (2.0f * PI));
                float lat = (float)asin(d.y < -1.0f ? -1.0f : d.y > 1.0f ? 1.0f : d.y) * (8.0f / PI);
                /* Signed distance to the nearest edge, as an angle on the
                 * ball (+ red, - white), 0.5 at the edge: the shader cuts
                 * it at 0.5 a pixel wide, sharp at any size. */
                float fl = lon - (float)floor(lon), ft = lat - (float)floor(lat), dl, dt, dd;
                dl = (fl < 0.5f ? fl : 1.0f - fl) * (2.0f * PI / 16.0f) * (float)sqrt(1.0f - d.y * d.y);
                dt = (ft < 0.5f ? ft : 1.0f - ft) * (PI / 8.0f);
                dd = (dl < dt ? dl : dt) * 2.5f;
                if (!(((int)floor(lon) + (int)floor(lat)) & 1))
                    dd = -dd;
                dd = 0.5f + dd;
                face[j * CHECKER_RES + i] = (UBYTE)(dd < 0.0f ? 0 : dd > 1.0f ? 255 : (int)(dd * 255.0f + 0.5f));
            }
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_R8, CHECKER_RES, CHECKER_RES, 0, GL_RED,
                     GL_UNSIGNED_BYTE, face);
    }
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    tex_params(GL_TEXTURE_CUBE_MAP, GL_LINEAR_MIPMAP_LINEAR, GL_CLAMP_TO_EDGE);

    /* Noise for the clouds and the caustics: random values,
     * smoothed by the trilinear filter, tiling. */
    for (i = 0; i < (int)sizeof(noise); i++) {
        seed = seed * 1103515245UL + 12345UL;
        noise[i] = (UBYTE)(seed >> 16);
    }
    glGenTextures(1, &e->noise);
    glBindTexture(GL_TEXTURE_3D, e->noise);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, NOISE_RES, NOISE_RES, NOISE_RES, 0, GL_RED, GL_UNSIGNED_BYTE, noise);
    tex_params(GL_TEXTURE_3D, GL_LINEAR, GL_REPEAT);

    /* The ripples: a small half-float tile, repeating, drawn once a frame. */
    glGenTextures(1, &e->ripple_tex);
    glBindTexture(GL_TEXTURE_2D, e->ripple_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16F, RIPPLE_RES, RIPPLE_RES);
    tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_REPEAT);
    if (!fbo_for(&e->ripple_fbo, GL_TEXTURE_2D, e->ripple_tex))
        return FALSE;
    /* The caustics: a small float tile, repeating, drawn once a frame. */
    glGenTextures(1, &e->caust_tex);
    glBindTexture(GL_TEXTURE_2D, e->caust_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16F, CAUST_RES, CAUST_RES);
    tex_params(GL_TEXTURE_2D, GL_LINEAR, GL_REPEAT);
    if (!fbo_for(&e->caust_fbo, GL_TEXTURE_2D, e->caust_tex))
        return FALSE;
    return es3_targets(e, w, h, pct);
}

static void es3_free(struct es3 *e)
{
    GLuint *p[] = {&e->height, &e->ripple, &e->caust, &e->sky, &e->ocean, &e->far, &e->copy, &e->bright, &e->blur, &e->final};
    int i;
    for (i = 0; i < 9; i++)
        if (*p[i])
            glDeleteProgram(*p[i]);
    es3_free_targets(e);
    for (i = 0; i < 6; i++)
        if (e->sky_fbo[i])
            glDeleteFramebuffers(1, &e->sky_fbo[i]);
    if (e->height_fbo)
        glDeleteFramebuffers(1, &e->height_fbo);
    if (e->caust_fbo)
        glDeleteFramebuffers(1, &e->caust_fbo);
    if (e->ripple_fbo)
        glDeleteFramebuffers(1, &e->ripple_fbo);
    {
        GLuint t[] = {e->height_tex, e->slope_tex, e->ripple_tex, e->caust_tex, e->sky_tex, e->checker, e->noise};
        for (i = 0; i < 7; i++)
            if (t[i])
                glDeleteTextures(1, &t[i]);
    }
    if (e->ubo)
        glDeleteBuffers(1, &e->ubo);
    if (e->vao)
        glDeleteVertexArrays(1, &e->vao);
}

static void set4(float *d, vec3 v, float w)
{
    d[0] = v.x, d[1] = v.y, d[2] = v.z, d[3] = w;
}

/* TIMING: the GPU finishing each pass (glFinish), every 120th frame: ms[0]
 * the water, ripples and sky, [1] the scene, [2] the bloom, [3] to the
 * window. */
static BOOL timing_now;
static double mark_t, ms[4];
static double mark(void)
{
    double n, d;
    if (!timing_now)
        return 0.0;
    glFinish();
    n = now_s();
    d = (n - mark_t) * 1000.0;
    mark_t = n;
    return d;
}

/* The scene: the full shader (ocean3.frag) only over the screen rectangle
 * round the ball, where its reflection, its refraction, its waterline and
 * the sandbank can show (NEAR_R round it); everywhere else the far one
 * (oceanfar3.frag: the same file without that work, which leaves it a
 * third of the size and fewer registers, so the GPU runs twice the threads).
 * The scissor test keeps each to its part: no pixel is shaded twice. */
/* The far water: a band (x0, y0)-(x1, y1) of the scene, at 1/div into the
 * half-size target (copy 0; a half texel wider each way, so the linear
 * reads at its edge find the far water too, not last frame's), or copied
 * from there into the scene (copy 1). */
static void far_band(int div, int x0, int y0, int x1, int y1, int copy)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    if (copy)
        glScissor(x0, y0, x1 - x0, y1 - y0);
    else
        glScissor(x0 / div - 1, y0 / div - 1, (x1 - x0) / div + 3, (y1 - y0) / div + 3);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Everything but the sand's band at the bottom (up to sy) and the ball's
 * rectangle (rx0, sy)-(rx1, ry1) above it. */
static void far_bands(struct es3 *e, int div, int sy, int rx0, int rx1, int ry1, int copy)
{
    far_band(div, 0, ry1, e->sw, e->sh, copy); /* above */
    far_band(div, 0, sy, rx0, ry1, copy);      /* left */
    far_band(div, rx1, sy, e->sw, ry1, copy);  /* right */
}

/* The full shader (ocean3.frag) where the ball's and the sand's work can
 * show: across the bottom third (SAND_TOP: the sand, faded out below its
 * edge in the shader) and in the rectangle round the ball above it (its
 * reflection, refraction and waterline). Everywhere else the far one
 * (oceanfar3.frag: the same file without that work, a third of the size,
 * fewer registers, so the GPU runs twice the threads), at half the size,
 * blown up linear: soft, as out of focus behind the ball. The scissor test
 * keeps each to its part. */
#define SAND_TOP 0.34f /* = ocean3.frag's */
static float near_share; /* TIMING: the share of the screen the full shader covers */
static void ocean_draw(struct es3 *e, const struct frame_uniforms *fu, int out)
{
    float aspect = (float)e->sw / (float)e->sh, x0 = 1.0f, y0 = 1.0f, x1 = -1.0f, y1 = -1.0f;
    const float m = BALL_R * 1.4f; /* the ball, its reflection's reach round it */
    int i, sy, rx0, rx1, ry1;
    BOOL whole = FALSE;
    /* The box round the ball, its corners projected as ocean3.vert projects
     * (forward * 1.5, the right and up axes). */
    for (i = 0; i < 8; i++) {
        vec3 c = v3(fu->ball.x + (i & 1 ? m : -m), fu->ball.y + (i & 2 ? BALL_R + 0.1f : -BALL_R),
                    fu->ball.z + (i & 4 ? m : -m));
        vec3 rel = sub(c, fu->eye);
        float z = dot(rel, fu->fw), x, y;
        if (z < 0.3f) {
            whole = TRUE; /* round the eye: all of it */
            break;
        }
        x = dot(rel, fu->rt) / (0.5f * aspect) * 1.5f / z;
        y = dot(rel, fu->up) / 0.5f * 1.5f / z;
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    }
    if (whole)
        x0 = y0 = -1.0f, x1 = y1 = 1.0f;
    sy = ((int)(SAND_TOP * e->sh) + 1) & ~1;
    rx0 = (int)((x0 < -1.0f ? -1.0f : x0) * 0.5f * e->sw + 0.5f * e->sw) & ~1;
    rx1 = ((int)((x1 > 1.0f ? 1.0f : x1) * 0.5f * e->sw + 0.5f * e->sw) + 2) & ~1;
    ry1 = ((int)((y1 > 1.0f ? 1.0f : y1) * 0.5f * e->sh + 0.5f * e->sh) + 2) & ~1;
    rx0 = rx0 < 0 ? 0 : rx0, rx1 = rx1 > e->sw ? e->sw : rx1;
    ry1 = ry1 > e->sh ? e->sh : ry1 < sy ? sy : ry1;
    if (rx1 < rx0)
        rx1 = rx0;
    near_share = (float)(e->sw * sy + (rx1 - rx0) * (ry1 - sy)) / (float)(e->sw * e->sh);

    glEnable(GL_SCISSOR_TEST);
    glUseProgram(e->ocean);
    glUniform1i(e->ocean_out, out);
    glScissor(0, 0, e->sw, sy);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (rx1 > rx0 && ry1 > sy) {
        glScissor(rx0, sy, rx1 - rx0, ry1 - sy);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glUseProgram(e->far);
    glUniform1i(e->far_out, out);
    if (!out) { /* into the window: at full size */
        far_bands(e, 1, sy, rx0, rx1, ry1, 0);
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, e->half_fbo);
    glViewport(0, 0, (e->sw + 1) / 2, (e->sh + 1) / 2);
    far_bands(e, 2, sy, rx0, rx1, ry1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, e->scene_fbo);
    glViewport(0, 0, e->sw, e->sh);
    glBindTexture(GL_TEXTURE_2D, e->half_tex); /* unit 0: the height map, put back below */
    glUseProgram(e->copy);
    far_bands(e, 2, sy, rx0, rx1, ry1, 1);
    glBindTexture(GL_TEXTURE_2D, e->height_tex);
    glDisable(GL_SCISSOR_TEST);
}

/* A frame the ES 3 way, into the window (width x height). */
static void es3_frame(struct es3 *e, const struct frame_uniforms *fu, const struct input *in, float t,
                      BOOL sky_now, int width, int height)
{
    static struct frame_block fb;
    int f, k;

    set4(fb.sun, fu->sun, t);
    set4(fb.eye, fu->eye, (float)e->sw / (float)e->sh);
    set4(fb.fw, fu->fw, 0.0f);
    set4(fb.rt, fu->rt, 1.0f / e->sw);
    set4(fb.up, fu->up, 1.0f / e->sh);
    set4(fb.ball, fu->ball, BALL_R);
    set4(fb.px, fu->px, 0.0f);
    set4(fb.py, fu->py, 0.0f);
    set4(fb.pz, fu->pz, 0.0f);
    set4(fb.ambient, fu->ambient, 0.0f);
    fb.mode[0] = in->ripples, fb.mode[1] = in->glints, fb.mode[2] = in->clouds, fb.mode[3] = in->caustics;
    fb.post[0] = 1.0f / width, fb.post[1] = 1.0f / height, fb.post[2] = in->bloom ? 0.6f : 0.0f;
    fb.post[3] = (float)width / (float)height;
    glBindBuffer(GL_UNIFORM_BUFFER, e->ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(fb), &fb);
    glBindVertexArray(e->vao);
    mark();
    ms[1] = ms[2] = ms[3] = 0.0;

    /* The coarse water (its height and slope at once), the ripples' tile,
     * then (now and then) the sky's six faces and its blurred copy's. */
    glBindFramebuffer(GL_FRAMEBUFFER, e->height_fbo);
    glViewport(0, 0, HEIGHT_RES, HEIGHT_RES);
    glUseProgram(e->height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (in->ripples) {
        glBindFramebuffer(GL_FRAMEBUFFER, e->ripple_fbo);
        glViewport(0, 0, RIPPLE_RES, RIPPLE_RES);
        glUseProgram(e->ripple);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (in->caustics) {
        glBindFramebuffer(GL_FRAMEBUFFER, e->caust_fbo);
        glViewport(0, 0, CAUST_RES, CAUST_RES);
        glUseProgram(e->caust);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_3D, e->noise);
    if (sky_now) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0); /* drawn into: not read */
        glUseProgram(e->sky);
        glViewport(0, 0, SKY_RES, SKY_RES);
        for (f = 0; f < 6; f++) {
            glBindFramebuffer(GL_FRAMEBUFFER, e->sky_fbo[f]);
            glUniform1i(e->sky_face, f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, e->sky_tex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_CUBE_MAP, e->checker);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, e->slope_tex);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, e->ripple_tex);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, e->caust_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e->height_tex);
    ms[0] = mark();

    if (0) { /* the far water at half size needs the float scene */
        /* Without the bloom, at 1/1: the scene tone mapped by its own
         * shader, straight into the window (as the ES 2.0 way). */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, e->sw, e->sh);
        ocean_draw(e, fu, 0);
        ms[1] = mark();
    } else {
        /* The scene in floats at the render scale. */
        glBindFramebuffer(GL_FRAMEBUFFER, e->scene_fbo);
        glViewport(0, 0, e->sw, e->sh);
        ocean_draw(e, fu, 1);
        ms[1] = mark();
        /* The bloom: the part brighter than white brought down to a
         * quarter, blurred twice each way. */
        if (in->bloom) {
            glBindFramebuffer(GL_FRAMEBUFFER, e->bloom_fbo[0]);
            glViewport(0, 0, e->bw, e->bh);
            glBindTexture(GL_TEXTURE_2D, e->scene_tex);
            glUseProgram(e->bright);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glUseProgram(e->blur);
            for (k = 0; k < 2; k++) {
                float spread = k ? 2.2f : 1.0f; /* the first at its own spacing: wider alone would copy a bright point into a grid */
                glBindFramebuffer(GL_FRAMEBUFFER, e->bloom_fbo[1]);
                glBindTexture(GL_TEXTURE_2D, e->bloom_tex[0]);
                glUniform2f(e->blur_dir, spread / e->bw, 0.0f);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindFramebuffer(GL_FRAMEBUFFER, e->bloom_fbo[0]);
                glBindTexture(GL_TEXTURE_2D, e->bloom_tex[1]);
                glUniform2f(e->blur_dir, 0.0f, spread / e->bh);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        ms[2] = mark();
        /* To the window: tone mapped, the bloom added. */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, e->bloom_tex[0]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, e->scene_tex);
        glUseProgram(e->final);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        ms[3] = mark();
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, 0); /* drawn into next frame */
        glActiveTexture(GL_TEXTURE0);
    }
    glBindVertexArray(0);
}

int main(void)
{
    static const GLfloat tri[] = {-1, -1, 3, -1, -1, 3}; /* covers the screen */
    static char title[120];
    static struct es3 e3;
    LONG args[7] = {0, 0, 0, 0, 0, 0, 0};
    BOOL timing;
    struct RDArgs *rda;
    struct Window *win = NULL, *iw = NULL; /* our window; the input window */
    void *ctx = NULL;
    BOOL full;
    ULONG err = 0, width = 0, height = 0;
    GLuint sea = 0, blit = 0, hprog = 0, sprog = 0, buf = 0;
    GLint u_time, u_sun, u_eye, u_ball, u_px, u_py, u_pz, u_ambient, u_fw, u_rt, u_up, u_aspect;
    GLint uh_time, us_sun;
    struct input in;
    struct frame_uniforms fu;
    double start, last_title;
    unsigned long frames = 0, title_frames = 0, max_frames = 0;
    int w0 = 640, h0 = 480, rc = 20;
    BOOL timer_open = FALSE;

    memset(&in, 0, sizeof(in));
    in.div = 1, in.pct = 100, in.my = 0.3f;
    in.ripples = in.glints = in.clouds = in.caustics = in.bloom = TRUE;

    rda = !Cli() ? NULL : /* no Shell (Workbench): the defaults; ReadArgs would wait on a console */
          ReadArgs((STRPTR) "WIDTH/N,HEIGHT/N,SCALE/N,FRAMES/N,FULLSCREEN/S,ES2/S,TIMING/S", args, NULL);
    if (!rda && Cli()) {
        printf("usage: boingocean-ogles3 [WIDTH=w] [HEIGHT=h] [SCALE=1..4] [FRAMES=n] [FULLSCREEN] [ES2]\n");
        return 20;
    }
    if (args[0])
        w0 = *(LONG *)args[0];
    if (args[1])
        h0 = *(LONG *)args[1];
    if (args[2] && *(LONG *)args[2] >= 1 && *(LONG *)args[2] <= 4)
        in.div = *(LONG *)args[2], in.pct = 100 / in.div;
    if (args[3])
        max_frames = *(LONG *)args[3];
    full = args[4] != 0;
    /* Full screen: 1280 x 720 unless a size is given; a window 640 x 480. */
    if (full && !args[0] && !args[1])
        w0 = 1280, h0 = 720;
    in.es2 = args[5] != 0;
    timing = args[6] != 0;
    if (rda)
        FreeArgs(rda);

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39))) {
        printf("needs intuition.library 39\n");
        return 20;
    }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0)) {
        printf("needs timer.device\n");
        goto out;
    }
    timer_open = TRUE;
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES3Open()) {
        printf("needs %s 1.0 or newer: in LIBS:, or in this program's drawer or its libs/\n"
               "(an older one that another program is using cannot be replaced)\n", OGLES3NAME);
        goto out;
    }
    if (full) {
        /* A true-colour mode of the size asked for, which ogles3 opens as a
         * screen. graphics.library cannot tell a 24-bit mode (3 bytes a
         * pixel) from a 32-bit one, and ogles3 takes only 32-bit ones: try
         * each mode of that size with 24 or more bits until ogles3 accepts
         * one. (An ASL screen-mode requester is the friendlier way.) */
        ULONG id = INVALID_ID;
        if (!(GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39))) {
            printf("FULLSCREEN needs graphics.library 39\n");
            goto out;
        }
        while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
            struct DimensionInfo dims;
            if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) || dims.MaxDepth < 24 ||
                dims.Nominal.MaxX - dims.Nominal.MinX + 1 != w0 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != h0)
                continue;
            ctx = aglCreateContextTags2(&err, OGLES3_CCT_MODEID, id, OGLES3_CCT_PISTORM_GET_WINDOW, (ULONG)&iw,
                                        OGLES3_CCT_DEPTH, 0, OGLES3_CCT_STENCIL, 0, OGLES3_CCT_GET_WIDTH,
                                        (ULONG)&width, OGLES3_CCT_GET_HEIGHT, (ULONG)&height,
                                        OGLES3_CCT_PISTORM_ES_VERSION, 30, TAG_DONE);
        }
        if (!ctx || !iw) {
            printf("no %dx%d screen mode that ogles3 can draw into (it needs a 32-bit one)\n", w0, h0);
            goto out;
        }
        /* The backdrop window has no messages until asked for. */
        ModifyIDCMP(iw, IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE);
        iw->Flags |= WFLG_REPORTMOUSE | WFLG_RMBTRAP;
    } else {
    win = OpenWindowTags(NULL, WA_Title, (ULONG) "ogles3: Boing Ocean ES3", WA_InnerWidth, w0, WA_InnerHeight, h0,
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE,
                         WA_SizeBBottom, TRUE, WA_MinWidth, 100, WA_MinHeight, 80, WA_MaxWidth, -1,
                         WA_MaxHeight, -1, WA_Activate, TRUE, WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_NEWSIZE | IDCMP_MOUSEBUTTONS |
                             IDCMP_MOUSEMOVE,
                         TAG_DONE);
    if (!win) {
        printf("cannot open a window\n");
        goto out;
    }
    /* No depth or stencil buffer: the shader does all the depth there is. */
    ctx = aglCreateContextTags2(&err, OGLES3_CCT_WINDOW, (ULONG)win, OGLES3_CCT_DEPTH, 0, OGLES3_CCT_STENCIL, 0,
                                OGLES3_CCT_GET_WIDTH, (ULONG)&width, OGLES3_CCT_GET_HEIGHT, (ULONG)&height,
                                OGLES3_CCT_PISTORM_ES_VERSION, 30, TAG_DONE);
    if (!ctx) {
        printf("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
        goto out;
    }
    iw = win;
    }
    key_window = iw;
    ActivateWindow(iw); /* a launcher's console may hold the keyboard */
    aglMakeCurrent(ctx);
    if (!(sea = link_program(boingocean_vs, boingocean_fs)) || !(blit = link_program(blit_vs, blit_fs)) ||
        !(hprog = link_program(blit_vs, height_fs)) || !(sprog = link_program(blit_vs, sky_fs)))
        goto out;
    glUseProgram(hprog);
    uh_time = glGetUniformLocation(hprog, "u_time");
    glUniform1f(glGetUniformLocation(hprog, "u_texel"), 2.0f * EXTENT / HEIGHT_RES);
    glUniform1f(glGetUniformLocation(hprog, "u_extent"), EXTENT);
    glUseProgram(sprog);
    us_sun = glGetUniformLocation(sprog, "u_sun");
    glUseProgram(sea);
    glUniform1i(glGetUniformLocation(sea, "u_height"), 0);
    glUniform1i(glGetUniformLocation(sea, "u_sky"), 1);
    u_time = glGetUniformLocation(sea, "u_time");
    u_sun = glGetUniformLocation(sea, "u_sun");
    u_eye = glGetUniformLocation(sea, "u_eye");
    u_ball = glGetUniformLocation(sea, "u_ball");
    u_px = glGetUniformLocation(sea, "u_px");
    u_py = glGetUniformLocation(sea, "u_py");
    u_pz = glGetUniformLocation(sea, "u_pz");
    u_ambient = glGetUniformLocation(sea, "u_ambient");
    u_fw = glGetUniformLocation(sea, "u_fw");
    u_rt = glGetUniformLocation(sea, "u_rt");
    u_up = glGetUniformLocation(sea, "u_up");
    u_aspect = glGetUniformLocation(sea, "u_aspect");
    glUseProgram(blit);
    glUniform1i(glGetUniformLocation(blit, "u_tex"), 0);

    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
    glEnableVertexAttribArray(0);
    if (!make_rt(&height_tex, &height_fbo, HEIGHT_RES, HEIGHT_RES) || !make_rt(&sky_tex, &sky_fbo, SKY_RES, SKY_RES) ||
        !make_target(width, height, in.div))
        goto out;
    /* The ES 3 way's programs, block, textures and targets; the ES 2.0 way
     * keeps vertex array object 0 and its buffer. */
    if (!es3_setup(&e3, width, height, in.pct))
        goto out;

    start = last_title = now_s();
    while (!in.quit) {
        double t;
        events(iw, &in);
        if (in.resized && full) {
            /* A screen does not change size: only the render scale. */
            if (!make_target(width, height, in.div) || !es3_targets(&e3, width, height, in.pct))
                break;
            in.resized = FALSE;
        } else if (in.resized) {
            /* The surface takes the window's new inner size, the render
             * target the new fraction of it. */
            aglSetParamsTags2(OGLES3_CCT_WINDOW, (ULONG)win, OGLES3_CCT_GET_WIDTH, (ULONG)&width,
                              OGLES3_CCT_GET_HEIGHT, (ULONG)&height, TAG_DONE);
            if (!make_target(width, height, in.div) || !es3_targets(&e3, width, height, in.pct))
                break;
            in.resized = FALSE;
        }
        t = now_s() - start;
        frame_setup(&fu, (float)t, in.yaw, in.my);

        if (!in.es2) {
            es3_frame(&e3, &fu, &in, (float)t, (frames & 15) == 0, width, height);
            goto shown;
        }

        mark();
        ms[2] = ms[3] = 0.0;
        /* The ES 2.0 way, as Boing Ocean draws it: the coarse water height
         * and slope, then (now and then) the sky table. */
        glBindFramebuffer(GL_FRAMEBUFFER, height_fbo);
        glViewport(0, 0, HEIGHT_RES, HEIGHT_RES);
        glUseProgram(hprog);
        glUniform1f(uh_time, (float)t);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if ((frames & 15) == 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, sky_fbo);
            glViewport(0, 0, SKY_RES, SKY_RES);
            glUseProgram(sprog);
            glUniform3f(us_sun, fu.sun.x, fu.sun.y, fu.sun.z);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        ms[0] = mark();
        /* The scene: into the window, or into the small target. */
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, target_w, target_h);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sky_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, height_tex);
        glUseProgram(sea);
        glUniform1f(u_time, (float)t);
        glUniform3f(u_sun, fu.sun.x, fu.sun.y, fu.sun.z);
        glUniform3f(u_eye, fu.eye.x, fu.eye.y, fu.eye.z);
        glUniform3f(u_ball, fu.ball.x, fu.ball.y, fu.ball.z);
        glUniform3f(u_px, fu.px.x, fu.px.y, fu.px.z);
        glUniform3f(u_py, fu.py.x, fu.py.y, fu.py.z);
        glUniform3f(u_pz, fu.pz.x, fu.pz.y, fu.pz.z);
        glUniform3f(u_ambient, fu.ambient.x, fu.ambient.y, fu.ambient.z);
        glUniform3f(u_fw, fu.fw.x, fu.fw.y, fu.fw.z);
        glUniform3f(u_rt, fu.rt.x, fu.rt.y, fu.rt.z);
        glUniform3f(u_up, fu.up.x, fu.up.y, fu.up.z);
        glUniform1f(u_aspect, (float)target_w / (float)target_h);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        ms[1] = mark();
        /* Scaled up to the window. */
        if (target_fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            glUseProgram(blit);
            glBindTexture(GL_TEXTURE_2D, target_tex);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    shown:
        aglSwapBuffers();
        if (timing_now) {
            say("%s ms: water and sky %.2f  scene %.2f (ball's rectangle %.0f%%)  bloom %.2f  to the window %.2f  swap %.2f\n",
                   in.es2 ? "es2" : "es3", ms[0], ms[1], near_share * 100.0f, ms[2], ms[3], mark());
        }
        timing_now = timing && frames % 120 == 60;

        frames++;
        title_frames++;
        /* The frame rate and the effects, in the title; full screen (no
         * title), printed to the Shell every 2 seconds. */
        if (now_s() - last_title >= (full ? 2.0 : 1.0)) {
            double n = now_s();
            sprintf(title, "Boing Ocean %s  %.1f fps  %dx%d  ripples:%s glints:%s clouds:%s caustics:%s bloom:%s",
                    in.es2 ? "ES2" : "ES3", title_frames / (n - last_title), target_w, target_h, in.ripples ? "texture" : "sines",
                    in.glints ? "on" : "off", in.clouds ? "on" : "off", in.caustics ? "on" : "off",
                    in.bloom ? "on" : "off");
            /* Fullscreen has no title bar: nothing (a line a second to a
             * launcher's console kept opening it and took the keyboard). */
            if (!full)
                SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
            last_title = n;
            title_frames = 0;
        }
        if (max_frames && frames >= max_frames)
            break;
    }
    /* The run's summary when it was asked for (a benchmark): a launcher's
     * console would open for it after the window closed, and wait. */
    if (frames && (max_frames || timing)) {
        double s = now_s() - start;
        say("%lu frames in %.1f s: %.2f fps at %dx%d (%s %lux%lu)\n", frames, s, frames / s, target_w, target_h,
               full ? "screen" : "window", (unsigned long)width, (unsigned long)height);
    }
    rc = 0;
out:
    if (ctx) {
        es3_free(&e3);
        if (target_fbo)
            glDeleteFramebuffers(1, &target_fbo);
        if (target_tex)
            glDeleteTextures(1, &target_tex);
        if (height_fbo)
            glDeleteFramebuffers(1, &height_fbo);
        if (height_tex)
            glDeleteTextures(1, &height_tex);
        if (sky_fbo)
            glDeleteFramebuffers(1, &sky_fbo);
        if (sky_tex)
            glDeleteTextures(1, &sky_tex);
        if (hprog)
            glDeleteProgram(hprog);
        if (sprog)
            glDeleteProgram(sprog);
        if (buf)
            glDeleteBuffers(1, &buf);
        if (sea)
            glDeleteProgram(sea);
        if (blit)
            glDeleteProgram(blit);
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
