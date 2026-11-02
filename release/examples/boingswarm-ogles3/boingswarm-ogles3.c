/*
 * Boing Swarm for OpenGL ES 3.1 (ogles3.library): thousands of tiny lights
 * fly together as a flock, and the flock is a spinning red-and-white Boing
 * ball. It bursts apart, flies as a murmuration, and swarms back.
 *
 * Everything is simulated by the GPU's compute shaders, every frame; the
 * 68k only says where the show is (docs/GLES3_PLAN.md, "Plan: Boing Swarm";
 * measured by tests/perf/swarmbench):
 *   - storage buffers hold every boid (position, speed), two of them swapped
 *     each frame;
 *   - a neighbour grid of 16^3 wrapping cells: the boids counted into their
 *     cells with atomics, the cells' first places summed in shared memory
 *     (scan.comp), the boids sorted into them (scatter.comp), so each boid
 *     looks only at nearby ones (sim.comp, which also counts it into its new
 *     cell for the next frame);
 *   - sim.comp writes the positions into an RGBA32F image, which the draw
 *     reads with texelFetch: one draw of point sprites (a vertex a boid)
 *     with a lens-flare texture, added up straight on the screen; the
 *     trails and the bloom at quarter size, added over it.
 *
 *   boingswarm-ogles3 [N=boids] [WIDTH=w] [HEIGHT=h] [FULLSCREEN] [MSAA]
 *                     [FRAMES=n] [EVERY=n] [TIMING] [SOUND]
 * N: 16384 (a multiple of 128). It opens a 640 x 480 window or, with
 * FULLSCREEN, a 1280 x 720 screen (WIDTH and HEIGHT for another). EVERY:
 * how many frames between a boid's looks at its neighbours (4). TIMING
 * times each pass; SOUND plays the boing and the burst's whoosh. The mouse is
 * a hawk the swarm flees. Keys: Space pauses, Esc quits.
 *
 * MIT License.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <devices/audio.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/graphics.h>
#include <proto/ogles3.h>
#include <GLES3/gl31.h>
#include "boingswarm-ogles3_shaders.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;
struct GfxBase *GfxBase;
static struct timerequest timer_req;
static struct Window *key_window;

#define PI 3.14159265f
#define CELLS 4096 /* 16^3, the shaders' grid */

/* ---- vectors and column-major 4x4 matrices ---- */
typedef struct { float x, y, z; } vec3;
typedef struct { float m[16]; } mat4;

static vec3 v3(float x, float y, float z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static vec3 normalize(vec3 a)
{
    float s = 1.0f / (float)sqrt(dot(a, a));
    return v3(a.x * s, a.y * s, a.z * s);
}

static mat4 mul(mat4 a, mat4 b)
{
    mat4 r;
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a.m[k * 4 + j] * b.m[i * 4 + k];
            r.m[i * 4 + j] = s;
        }
    return r;
}

static mat4 perspective(float fovy, float aspect, float n, float f)
{
    mat4 r;
    float t = 1.0f / (float)tan(fovy * 0.5f);
    memset(&r, 0, sizeof(r));
    r.m[0] = t / aspect;
    r.m[5] = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * f * n / (n - f);
    return r;
}

/* The view matrix; *right and *up: the camera's axes in the world. */
static mat4 look_at(vec3 eye, vec3 at, vec3 up, vec3 *right, vec3 *upv)
{
    vec3 f = normalize(sub(at, eye)), s = normalize(cross(f, up)), u = cross(s, f);
    mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z;
    r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z;
    r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z;
    r.m[12] = -dot(s, eye), r.m[13] = -dot(u, eye), r.m[14] = dot(f, eye);
    r.m[15] = 1.0f;
    *right = s;
    *upv = u;
    return r;
}

/* ---- time and text ---- */
static double now_s(void)
{
    struct TimeVal tv;
    GetSysTime((TimeVal_Type *)&tv);
    return tv.tv_secs + tv.tv_micro / 1e6;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    if (!Cli())
        return; /* from Workbench: no one reads it */
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (key_window)
        ActivateWindow(key_window);
}

/* ---- GL helpers ---- */
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
        say("shader: %s\n", log);
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
    /* the ES 2.0 way's and the text's client arrays (names a program lacks are
     * ignored; no two names on one slot: an aliased program's binary is named
     * by its bindings too, which the shipped binaries are not) */
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glBindAttribLocation(p, 2, "a_col");
    glLinkProgram(p);
    glDeleteShader(a);
    if (b)
        glDeleteShader(b);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        say("link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint compute(const char *src)
{
    return link(compile(GL_COMPUTE_SHADER, src), 0);
}

/* A dispatch, timed into *t (the library waits for it: ogles3/compute.c). */
static void run(GLuint p, int groups, double *t)
{
    double a = now_s();
    glUseProgram(p);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    *t += now_s() - a;
}

/* The boids' light, 64x64 RGBA8, mipmapped: red the white-hot core and its
 * star (four rays and two fainter diagonals, a fifth of the core), green
 * the soft glow. */
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

/* The floor, made once: its 28 x 28 units in 1024 x 1024 RGBA8, mipmapped
 * (floor.frag reads it: a texel a pixel, where working the grid out a
 * pixel cost some 3 ms a frame at 1280x720). Dark checks of 1.5 units, faint
 * violet lines between them (1.5 texels, smoothed), fading into the clear
 * colour beyond 9 units. */
static float smooth(float a, float b, float t);

static GLuint make_floor(void)
{
    enum { S = 1024 };
    UBYTE *px = malloc(S * S * 4);
    GLuint t = 0;
    int x, y;
    if (!px)
        return 0;
    for (y = 0; y < S; y++)
        for (x = 0; x < S; x++) {
            float wx = ((x + 0.5f) / S - 0.5f) * 28.0f, wz = ((y + 0.5f) / S - 0.5f) * 28.0f;
            float qx = wx / 1.5f, qz = wz / 1.5f, fx = (float)fabs(qx - floor(qx) - 0.5f), fz = (float)fabs(qz - floor(qz) - 0.5f);
            float edge = (0.5f - (fx > fz ? fx : fz)) * 1.5f * S / 28.0f; /* in texels */
            float line = 1.0f - smooth(0.0f, 1.5f, edge), check = (float)(((int)floor(qx) + (int)floor(qz)) & 1);
            float fade = smooth(9.0f, 14.0f, (float)sqrt(wx * wx + wz * wz)), c[3];
            UBYTE *p = px + (y * S + x) * 4;
            int k;
            c[0] = 0.045f + 0.03f * check + 0.25f * 0.35f * line;
            c[1] = 0.045f + 0.025f * check + 0.12f * 0.35f * line;
            c[2] = 0.06f + 0.035f * check + 0.4f * 0.35f * line;
            c[0] += (0.01f - c[0]) * fade, c[1] += (0.01f - c[1]) * fade, c[2] += (0.03f - c[2]) * fade;
            for (k = 0; k < 3; k++)
                p[k] = (UBYTE)(c[k] * 255.0f + 0.5f);
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
    free(px);
    return t;
}

/* A render target: a texture and its framebuffer. */
struct rt {
    GLuint tex, fbo;
    int w, h;
};

static BOOL make_rt(struct rt *r, int w, int h, GLenum ifmt, GLenum fmt, GLenum type)
{
    static const GLfloat black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    r->w = w, r->h = h;
    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        say("a %dx%d render target is not complete\n", w, h);
        return FALSE;
    }
    glClearBufferfv(GL_COLOR, 0, black);
    return TRUE;
}

static void free_rt(struct rt *r)
{
    if (r->fbo)
        glDeleteFramebuffers(1, &r->fbo);
    if (r->tex)
        glDeleteTextures(1, &r->tex);
}

/* The full-screen passes' textures: a on unit 0, b (if any) on unit 3. */
static void bind2(GLuint a, GLuint b)
{
    if (b) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, b);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindTexture(GL_TEXTURE_2D, a);
}

/* ---- sound: the boing at each bounce, a whoosh at the burst; synthesised
 * once (Boing 2026's boing), played on a left and a right channel ---- */
#define SND_RATE 16000
#define BOING_LEN 7200
#define WHOOSH_LEN 24000
static BYTE *snd[2];
static const ULONG snd_len[2] = {BOING_LEN, WHOOSH_LEN};
static struct MsgPort *snd_port;
static struct IOAudio *snd_alloc, snd_io[2];
static BOOL snd_busy[2];

static BYTE clip8(double v)
{
    v = tanh(v * 1.6) * 127.0;
    return (BYTE)(v > 127.0 ? 127 : v < -128.0 ? -128 : (int)v);
}

static void synth(void)
{
    double ph = 0.0, seed = 1.0, lp = 0.0;
    int i;
    for (i = 0; i < BOING_LEN; i++) {
        double t = (double)i / SND_RATE;
        double f = (88.0 + 150.0 * exp(-t * 12.0)) * (1.0 + 0.035 * sin(2.0 * PI * 7.0 * t));
        double env = (1.0 - exp(-t * 600.0)) * exp(-t * 6.5), noise;
        seed = fmod(seed * 16807.0, 2147483647.0);
        noise = seed / 2147483647.0 * 2.0 - 1.0;
        ph += 2.0 * PI * f / SND_RATE;
        snd[0][i] = clip8(env * (0.62 * sin(ph) + 0.25 * sin(2.0 * ph + 0.5) + 0.10 * sin(3.01 * ph)) +
                          noise * exp(-t * 90.0) * 0.4);
    }
    /* the whoosh: noise through a low-pass whose corner falls, swelling in
     * fast and dying away over 1.5 s */
    for (i = 0; i < WHOOSH_LEN; i++) {
        double t = (double)i / SND_RATE, k = 0.05 + 0.5 * exp(-t * 2.5);
        double env = (1.0 - exp(-t * 40.0)) * exp(-t * 2.2), noise;
        seed = fmod(seed * 16807.0, 2147483647.0);
        noise = seed / 2147483647.0 * 2.0 - 1.0;
        lp += (noise - lp) * k;
        snd[1][i] = clip8(lp * env * 2.2);
    }
}

static void begin_io(struct IORequest *io)
{
    register struct IORequest *a1 __asm("a1") = io;
    register struct Device *a6 __asm("a6") = io->io_Device;
    /* BeginIO, the device's vector -30: a1 the request, a6 the device;
     * d0/d1/a0/a1 are the callee's to change. */
    __asm volatile("jsr -30(a6)" : "+r"(a1) : "r"(a6) : "d0", "d1", "a0", "cc", "memory");
}

static BOOL sound_open(void)
{
    static UBYTE chans[] = {3, 5, 10, 12}; /* a left and a right channel */
    ULONG units;
    if (!(snd[0] = AllocMem(BOING_LEN, MEMF_CHIP)) || !(snd[1] = AllocMem(WHOOSH_LEN, MEMF_CHIP)))
        return FALSE;
    synth();
    if (!(snd_port = CreateMsgPort()))
        return FALSE;
    if (!(snd_alloc = (struct IOAudio *)CreateIORequest(snd_port, sizeof(struct IOAudio))))
        return FALSE;
    snd_alloc->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
    snd_alloc->ioa_Request.io_Command = ADCMD_ALLOCATE;
    snd_alloc->ioa_Request.io_Flags = ADIOF_NOWAIT;
    snd_alloc->ioa_Data = chans;
    snd_alloc->ioa_Length = sizeof(chans);
    if (OpenDevice((STRPTR)AUDIONAME, 0, (struct IORequest *)snd_alloc, 0)) {
        DeleteIORequest((struct IORequest *)snd_alloc);
        snd_alloc = NULL;
        return FALSE;
    }
    units = (ULONG)snd_alloc->ioa_Request.io_Unit;
    snd_io[0] = *snd_alloc;
    snd_io[1] = *snd_alloc;
    snd_io[0].ioa_Request.io_Unit = (struct Unit *)(units & 9); /* channels 0, 3: left */
    snd_io[1].ioa_Request.io_Unit = (struct Unit *)(units & 6); /* 1, 2: right */
    return TRUE;
}

/* Sound s (0 the boing, 1 the whoosh), pan -1 (left) .. 1 (right). */
static void sound_play(int s, float pan, float loud)
{
    int c;
    if (!snd_alloc)
        return;
    for (c = 0; c < 2; c++) {
        struct IOAudio *a = &snd_io[c];
        float v = loud * (c ? 0.5f + pan * 0.5f : 0.5f - pan * 0.5f);
        if (snd_busy[c]) {
            if (!CheckIO((struct IORequest *)a))
                AbortIO((struct IORequest *)a);
            WaitIO((struct IORequest *)a);
        }
        a->ioa_Request.io_Command = CMD_WRITE;
        a->ioa_Request.io_Flags = ADIOF_PERVOL;
        a->ioa_Data = (UBYTE *)snd[s];
        a->ioa_Length = snd_len[s];
        a->ioa_Period = 3546895 / SND_RATE;
        a->ioa_Volume = (UWORD)(64.0f * (v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v));
        a->ioa_Cycles = 1;
        begin_io((struct IORequest *)a);
        snd_busy[c] = TRUE;
    }
}

static void sound_close(void)
{
    int c;
    if (snd_alloc) {
        for (c = 0; c < 2; c++)
            if (snd_busy[c]) {
                if (!CheckIO((struct IORequest *)&snd_io[c]))
                    AbortIO((struct IORequest *)&snd_io[c]);
                WaitIO((struct IORequest *)&snd_io[c]);
            }
        CloseDevice((struct IORequest *)snd_alloc);
        DeleteIORequest((struct IORequest *)snd_alloc);
    }
    if (snd_port)
        DeleteMsgPort(snd_port);
    for (c = 0; c < 2; c++)
        if (snd[c])
            FreeMem(snd[c], snd_len[c]);
}

/* ---- the show ---- */
/* The timeline, in seconds; after RETURN_END it goes on from BALL_START
 * (the gathering is the first time only). */
#define GATHER_END   12.0f
#define BALL_START   GATHER_END
#define BURST_AT     35.0f
#define FLOW_START   37.0f
#define RETURN_START 55.0f
#define RETURN_END   65.0f

struct show {
    float pull, flow, burst; /* sim.comp's forces */
    float stripe;            /* the return's progress, 0..1 (< 0: none) */
    vec3 wander;             /* the murmuration's centre */
    const char *name;
};

/* The ball glides round in a spiral: round the middle (0.55 rad/s) on a
 * radius that swells and shrinks between 1.5 and BALL_X (a slow spiral out
 * and in), rising and sinking on a sine (4.3 +- 1.1: its radius 3, so its
 * lowest 0.2 above the floor). Smooth, so the swarm keeps up (a bounce's
 * sudden turns left the boids behind). The low point of each sink is its
 * "landing" (the boing). */
#define BALL_X 4.5f

struct ball {
    float t, x;
    vec3 centre;
    BOOL hit; /* at the low point this step */
};

static void ball_step(struct ball *b, float dt)
{
    float t0 = b->t, a, r;
    b->t += dt;
    a = b->t * 0.55f, r = 3.0f + 1.5f * (float)sin(b->t * 0.21f);
    b->x = (float)cos(a) * r;
    b->centre = v3(b->x, 4.3f + 1.1f * (float)sin(b->t * 1.1f), (float)sin(a) * r);
    b->hit = dt > 0.0f && cos(t0 * 1.1f) < 0.0f && cos(b->t * 1.1f) >= 0.0f; /* the sine's low point passed */
}

static float smooth(float a, float b, float t)
{
    float x = (t - a) / (b - a);
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return x * x * (3.0f - 2.0f * x);
}

static void show_at(float t, struct show *s)
{
    float w = smooth(FLOW_START, FLOW_START + 4.0f, t) * (1.0f - smooth(RETURN_START, RETURN_END - 2.0f, t));
    s->burst = 0.0f;
    s->stripe = -1.0f;
    s->wander = v3((float)sin(t * 0.31f) * 2.5f * w, 3.5f + (float)sin(t * 0.23f) * 1.0f * w,
                   (float)cos(t * 0.27f) * 2.0f * w);
    if (t < GATHER_END) { /* the sparks pour in, wheeling, and fold into the ball */
        s->pull = smooth(3.0f, GATHER_END, t);
        s->flow = 1.0f - s->pull;
        s->name = "the gathering";
    } else if (t < BURST_AT) {
        s->pull = 1.0f, s->flow = 0.0f;
        s->name = "the Boing";
    } else if (t < FLOW_START) { /* burst: free, slowing down */
        s->pull = 0.0f, s->flow = 0.0f;
        s->name = "the burst";
    } else if (t < RETURN_START) {
        s->pull = 0.0f, s->flow = smooth(FLOW_START, FLOW_START + 3.0f, t);
        s->name = "the murmuration";
    } else { /* stripe by stripe, from the bottom (sim.comp, u_stripe) */
        s->pull = 1.0f, s->flow = 1.0f;
        s->stripe = (t - RETURN_START) / (RETURN_END - 1.0f - RETURN_START);
        s->stripe = s->stripe > 1.0f ? 1.0f : s->stripe;
        s->name = "the return";
    }
}

/* ---- the stats' text: a 5x7 font of capitals, figures and a few signs,
 * made into a texture once (64 cells of 6x8: ' ' to '_'), drawn as squares
 * from a client array ---- */
struct glyph {
    char c;
    const char *row[7];
};

static const struct glyph glyphs[] = {
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'%', {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
};

/* The text ring: OGLES3 (the ES 2.0 way: OGLES2) spelt by the last eighth
 * of the boids, in lights on a ring of radius 5.4 round the ball
 * (sim.comp's text_home). Each boid gets a pixel of the font's letters in
 * turn and a place within it: its angle on the ring (the letters left to
 * right as seen from outside: the angle falls), height and depth, four
 * floats a boid, 128 boids a row (the GPU's texture, the 68k's table).
 * tx_orbit turns the ring. */
#define TEXT_PX 0.15f /* a font pixel's size */
static GLfloat *tx_place[2];     /* the places: [0] OGLES3 (the GPU's), [1] OGLES2 (the 68k's) */
static float tx_orbit;           /* the text's turn on its ring */
static float tx_ring, tx_ringt;  /* the text turned into Saturn's rings (0..1); the rings' time */
static float tx_storm;           /* the text boids let go into a storm round the ball (0..1) */
static float tx_pull;            /* the text boids' pull: forming in 1.5 s as the text scrolls in */

static GLfloat *text_places(int nt, const char *text)
{
    int px[6 * 7 * 5][2], np = 0, c, x, y, k, s;
    unsigned g;
    float cols = 6.0f * strlen(text) - 1.0f;
    GLfloat *place;
    for (c = 0; text[c]; c++)
        for (g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++)
            if (glyphs[g].c == text[c])
                for (y = 0; y < 7; y++)
                    for (x = 0; x < 5; x++)
                        if (glyphs[g].row[y][x] == '#')
                            px[np][0] = c * 6 + x, px[np][1] = 6 - y, np++;
    if (!(place = calloc(((nt + 127) & ~127) * 4, sizeof(GLfloat))))
        return NULL;
    /* Each font pixel an s x s grid of places, the grids of neighbouring
     * pixels in line (the letters one even grid of lights); the places a
     * boid short spread evenly over all (boid k at place k * all / nt). */
    for (s = 1; s * s * np < nt; s++)
        ;
    for (k = 0; k < nt; k++) {
        int cell = (int)((float)k * (s * s * np) / nt), p = cell / (s * s), q = cell % (s * s);
        float u = ((q % s) + 0.5f + ((float)fmod(k * 0.7548777, 1.0) - 0.5f) * 0.3f) / s;
        float v = ((q / s) + 0.5f + ((float)fmod(k * 0.5698403, 1.0) - 0.5f) * 0.3f) / s;
        GLfloat *t = place + k * 4;
        t[0] = (cols * 0.5f - (px[p][0] + u)) * TEXT_PX / 5.4f + PI * 0.5f;
        t[1] = (px[p][1] + v - 3.5f) * TEXT_PX;
        t[2] = ((float)fmod(k * 0.6180340, 1.0) - 0.5f) * 0.06f;
    }
    return place;
}

static BOOL text_init(int nt)
{
    return (tx_place[0] = text_places(nt, "OGLES3")) && (tx_place[1] = text_places(nt, "OGLES2"));
}

#define FONT_W 384 /* 64 cells of 6 x 8 */
#define FONT_H 8

static GLuint make_font(void)
{
    static UBYTE px[FONT_W * FONT_H * 4];
    GLuint t;
    unsigned g;
    int x, y;
    for (g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++) {
        int cell = glyphs[g].c - ' ';
        for (y = 0; y < 7; y++)
            for (x = 0; x < 5; x++)
                if (glyphs[g].row[y][x] == '#') {
                    UBYTE *p = px + ((6 - y) * FONT_W + cell * 6 + x) * 4; /* rows 0-6 from the bottom */
                    p[0] = p[1] = p[2] = p[3] = 255;
                }
    }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FONT_W, FONT_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

#define TEXT_MAX 96 /* letters a line */

/* A line of text at pixel (x, y) from the bottom left, each font pixel
 * `scale` screen pixels (the text program in use, its font bound). */
static void draw_text(const char *s, float x, float y, float scale)
{
    static GLfloat v[TEXT_MAX * 6 * 4];
    int k = 0, i;
    for (; *s && k < TEXT_MAX * 6; s++, x += 6.0f * scale) {
        int c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        float u0, u1, w = 5.0f * scale, h = 7.0f * scale;
        static const float cx[6] = {0, 1, 0, 0, 1, 1}, cy[6] = {0, 0, 1, 1, 0, 1};
        if (c <= ' ' || c > '_')
            continue;
        u0 = (float)((c - ' ') * 6) / FONT_W;
        u1 = u0 + 5.0f / FONT_W;
        for (i = 0; i < 6; i++, k++) {
            GLfloat *p = v + k * 4;
            p[0] = x + cx[i] * w, p[1] = y + cy[i] * h;
            p[2] = cx[i] ? u1 : u0, p[3] = cy[i] * 7.0f / FONT_H;
        }
    }
    if (!k)
        return;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLES, 0, k);
}

/* ---- how busy the 68k is: a task at the lowest priority counts while
 * nothing else wants the CPU; its count against the count of an idle 68k
 * (measured at the start) is the idle share ---- */
static volatile ULONG idle_count;
static volatile BOOL idle_stop;
static struct Task *idle_parent;

static void idle_entry(void)
{
    while (!idle_stop)
        idle_count++;
    Forbid(); /* the parent may unload the code only once this has ended */
    Signal(idle_parent, SIGBREAKF_CTRL_F);
}

static BOOL idle_start(void)
{
    idle_parent = FindTask(NULL);
    SetSignal(0, SIGBREAKF_CTRL_F);
    return CreateNewProcTags(NP_Entry, (ULONG)idle_entry, NP_Name, (ULONG) "Boing Swarm idle", NP_Priority, -127,
                             TAG_DONE) != NULL;
}

static void idle_end(void)
{
    idle_stop = TRUE;
    Wait(SIGBREAKF_CTRL_F);
}

/* ---- the ES 2.0 way (key E): the swarm simulated by the 68k ---- */
/* As many boids as the GPU's (N2), with the same neighbour grid: 16^3
 * cells of 0.5 that wrap, sorted by a count, a sum and a scatter, at most 8
 * neighbours looked at, an EVERY-th of the boids looking a frame. */
static int N2, cpu_every = 4;
#define BALL_OF(n) ((n) - (n) / 8) /* the boids of the ball; the last eighth spells the text before it */
#define N2_BALL BALL_OF(N2)

struct cpu_boid {
    float p[3], v[3];
};

static struct cpu_boid *cb, *cb_old;
static GLfloat *cb_verts; /* six vertices a boid: position, corner, colour */
static int *cb_cell, *cb_sorted, cb_starts[4097];

static int cpu_cell(const float *p)
{
    return ((int)floor(p[0] * 2.0f) & 15) + 16 * (((int)floor(p[1] * 2.0f) & 15) + 16 * ((int)floor(p[2] * 2.0f) & 15));
}

static void cpu_free(void)
{
    free(cb), free(cb_old), free(cb_verts), free(cb_cell), free(cb_sorted), free(tx_place[0]), free(tx_place[1]);
}

static BOOL cpu_init(int n, int every)
{
    ULONG s = 777;
    int i, k;
    N2 = n, cpu_every = every;
    cb = malloc(n * sizeof *cb), cb_old = malloc(n * sizeof *cb);
    cb_verts = malloc(n * 6 * 9 * sizeof(GLfloat));
    cb_cell = malloc(n * sizeof(int)), cb_sorted = malloc(n * sizeof(int));
    if (!cb || !cb_old || !cb_verts || !cb_cell || !cb_sorted)
        return FALSE;
    for (i = 0; i < N2; i++)
        for (k = 0; k < 3; k++) {
            s = s * 1664525UL + 1013904223UL;
            cb[i].p[k] = (((s >> 8) & 0xffff) / 32767.5f - 1.0f) * (k == 1 ? 4.0f : 10.0f) + (k == 1 ? 5.0f : 0.0f);
            cb[i].v[k] = 0.0f;
        }
    return TRUE;
}

/* sim.comp's home() and text_home(), for the 68k's boids: *red 1 on a red check. */
static void cpu_home(int i, float spin, vec3 c, float *h, float *red)
{
    float y = 1.0f - 2.0f * (i + 0.5f) / N2_BALL, r = (float)sqrt(1.0f - y * y);
    if (i >= N2_BALL) { /* the text */
        const GLfloat *tp = tx_place[1] + (i - N2_BALL) * 4; /* OGLES2 */
        float a = tp[0] + tx_orbit, rr = 5.4f + tp[2];
        float tx = (float)cos(a) * rr, ty = tp[1], tz = (float)sin(a) * rr;
        if (tx_ring > 0.0f) { /* sim.comp's Saturn's rings */
            int k = i - N2_BALL;
            float u = (float)fmod(k * 0.7548777, 1.0), v = (float)fmod(k * 0.5698403, 1.0);
            float w = (float)fmod(k * 0.6180340, 1.0);
            float r = u < 0.62f ? 3.7f + 1.15f * (u / 0.62f) : 5.1f + 0.65f * ((u - 0.62f) / 0.38f);
            float ra = v * 6.2831853f + tx_ringt * (float)pow(3.7f / r, 1.5f);
            float qx = (float)cos(ra) * r, qy = (w - 0.5f) * 0.06f, qz = (float)sin(ra) * r;
            const float ct = 0.9553365f, st = 0.2955202f;
            tx += (ct * qx - st * qy - tx) * tx_ring;
            ty += (st * qx + ct * qy - ty) * tx_ring;
            tz += (qz - tz) * tx_ring;
        }
        h[0] = c.x + tx, h[1] = c.y + ty, h[2] = c.z + tz;
        *red = 0.0f;
        return;
    }
    float phi = (float)fmod(i * 2.3999632, 6.2831853), lon = phi + spin;
    float px = (float)cos(lon) * r * 3.0f, py = y * 3.0f, pz = (float)sin(lon) * r * 3.0f;
    const float ct = 0.9553365f, st = 0.2955202f;
    *red = (float)(((int)(phi * 2.5464791f) + (int)((y + 1.0f) * 4.0f)) & 1);
    h[0] = c.x + ct * px - st * py;
    h[1] = c.y + st * px + ct * py;
    h[2] = c.z + pz;
}

/* One step of the 68k's swarm: sim.comp's forces (pull, flow, burst, the
 * stripe-by-stripe return, the hawk, the floor) and sim.comp's flocking
 * through the grid, read from the last step's boids (cb_old). */
static void cpu_step(const struct show *sh, float dt, float t, float spin, vec3 centre, const float *hawk, int frame)
{
    int i, j, k;
    struct cpu_boid *sw;
    /* the grid: each boid counted into its cell, the counts summed, sorted */
    memset(cb_starts, 0, sizeof cb_starts);
    for (i = 0; i < N2; i++)
        cb_starts[(cb_cell[i] = cpu_cell(cb[i].p)) + 1]++;
    for (i = 0; i < 4096; i++)
        cb_starts[i + 1] += cb_starts[i];
    for (i = 0; i < N2; i++)
        cb_sorted[cb_starts[cb_cell[i]]++] = i;
    for (i = 4096; i > 0; i--) /* the scatter moved each start to its end */
        cb_starts[i] = cb_starts[i - 1];
    cb_starts[0] = 0;
    sw = cb_old, cb_old = cb, cb = sw;
    memcpy(cb, cb_old, N2 * sizeof *cb);
    for (i = 0; i < N2; i++) {
        struct cpu_boid *b = &cb[i];
        float h[3], red, a[3], pull = sh->pull, fl = sh->flow, sep[3] = {0, 0, 0}, av[3] = {0, 0, 0},
                                ap[3] = {0, 0, 0}, seen = 0.0f, d[3], s;
        BOOL text = i >= N2_BALL, storm;
        cpu_home(i, spin, centre, h, &red);
        if (sh->stripe >= 0.0f) {
            float band = text ? 0.875f : (float)floor((2.0f - 2.0f * (i + 0.5f) / N2_BALL) * 4.0f) / 8.0f;
            pull = smooth(band * 0.75f, band * 0.75f + 0.25f, sh->stripe);
            fl = 1.0f - pull;
        }
        if (text) { /* the text forms on its own clock (sim.comp) */
            pull = tx_pull;
            fl = 1.0f - pull;
        }
        storm = text && tx_storm > 0.0f; /* sim.comp's text storm */
        if (storm)
            fl += pull * tx_storm, pull *= 1.0f - tx_storm;
        /* flocking (sim.comp): not when held fast; the octant's 8 cells, at most 6 */
        if (pull < 0.999f && (i >> 7) % cpu_every == frame % cpu_every) {
            int gx = (int)floor(b->p[0] * 2.0f), gy = (int)floor(b->p[1] * 2.0f), gz = (int)floor(b->p[2] * 2.0f);
            int ox = b->p[0] * 2.0f - (float)gx >= 0.5f ? 1 : -1, oy = b->p[1] * 2.0f - (float)gy >= 0.5f ? 1 : -1;
            int oz = b->p[2] * 2.0f - (float)gz >= 0.5f ? 1 : -1, dx, dy, dz, looked = 0;
            for (dz = 0; dz < 2 && looked < 6; dz++)
                for (dy = 0; dy < 2 && looked < 6; dy++)
                    for (dx = 0; dx < 2 && looked < 6; dx++) {
                        int c = ((gx + dx * ox) & 15) + 16 * (((gy + dy * oy) & 15) + 16 * ((gz + dz * oz) & 15)), e = cb_starts[c + 1];
                        for (j = cb_starts[c]; j < e && looked < 6; j++, looked++) {
                            const struct cpu_boid *o = &cb_old[cb_sorted[j]];
                            float d2;
                            for (k = 0; k < 3; k++)
                                d[k] = o->p[k] - b->p[k];
                            d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                            /* the ball's boids and the text's flock apart */
                            if (cb_sorted[j] == i || d2 > 0.25f || (cb_sorted[j] >= N2_BALL) != text)
                                continue;
                            for (k = 0; k < 3; k++) {
                                sep[k] -= d[k] / (d2 + 0.01f);
                                av[k] += o->v[k];
                                ap[k] += d[k];
                            }
                            seen += 1.0f;
                        }
                    }
        }
        {
            /* the murmuration's flow (sim.comp's flow(); the storm's round the ball) */
            vec3 fc = storm ? centre : sh->wander;
            float dx = b->p[0] - fc.x, dy = b->p[1] - fc.y, dz = b->p[2] - fc.z;
            float l = (float)sqrt(dx * dx + dz * dz) + 0.0001f, far = (float)sqrt(dx * dx + dy * dy + dz * dz) - 6.0f;
            float f[3];
            far = far > 0.0f ? far : 0.0f;
            f[0] = -dz / l * 2.0f + (float)sin(b->p[1] * 0.9f + t * 0.7f) * 2.8f - dx * far * 0.3f;
            f[1] = (float)sin(b->p[2] * 0.8f - t * 0.5f) * 0.8f * 2.8f - dy * far * 0.3f;
            f[2] = dx / l * 2.0f + (float)cos(b->p[0] * 0.7f + t * 0.6f) * 2.8f - dz * far * 0.3f;
            for (k = 0; k < 3; k++)
                a[k] = (h[k] - b->p[k]) * 9.0f * pull - b->v[k] * (0.8f + 3.2f * pull) + (f[k] - b->v[k]) * 1.2f * fl;
            if (storm) { /* kept out of the ball */
                float ox = b->p[0] - centre.x, oy = b->p[1] - centre.y, oz = b->p[2] - centre.z;
                float ol = (float)sqrt(ox * ox + oy * oy + oz * oz) + 0.001f;
                if (ol < 3.8f)
                    a[0] += ox / ol * (3.8f - ol) * 30.0f * tx_storm, a[1] += oy / ol * (3.8f - ol) * 30.0f * tx_storm,
                    a[2] += oz / ol * (3.8f - ol) * 30.0f * tx_storm;
            }
        }
        if (seen > 0.0f)
            for (k = 0; k < 3; k++)
                a[k] += (sep[k] * 0.02f + (av[k] / seen - b->v[k]) * 0.5f + ap[k] / seen * 0.5f) * cpu_every;
        if (hawk[3] > 0.0f) {
            float l;
            for (k = 0; k < 3; k++)
                d[k] = b->p[k] - hawk[k];
            l = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l < 2.2f)
                for (k = 0; k < 3; k++)
                    a[k] += d[k] / (l + 0.05f) * 60.0f * (1.0f - l / 2.2f);
        }
        if (sh->burst > 0.0f) {
            float l;
            for (k = 0; k < 3; k++)
                d[k] = b->p[k] - (k == 0 ? centre.x : k == 1 ? centre.y : centre.z);
            l = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 0.0001f;
            for (k = 0; k < 3; k++)
                b->v[k] += d[k] / l * sh->burst * (0.4f + 0.6f * (float)fmod(i * 0.4142136, 1.0));
        }
        for (k = 0; k < 3; k++)
            b->v[k] += a[k] * dt;
        s = (float)sqrt(b->v[0] * b->v[0] + b->v[1] * b->v[1] + b->v[2] * b->v[2]);
        if (s > 9.0f)
            for (k = 0; k < 3; k++)
                b->v[k] *= 9.0f / s;
        for (k = 0; k < 3; k++)
            b->p[k] += b->v[k] * dt;
        if (b->p[1] < 0.05f) {
            b->p[1] = 0.05f;
            b->v[1] = (float)fabs(b->v[1]) * 0.3f;
        }
    }
}

/* The 68k's boids as squares facing the camera, six vertices each (no
 * instancing in ES 2.0), coloured and lit as boid.vert/boid.frag do. */
static void cpu_verts(float spin, vec3 centre, vec3 eye, vec3 right, vec3 up)
{
    static const float cx[6] = {-1, 1, -1, -1, 1, 1}, cy[6] = {-1, -1, 1, 1, -1, 1};
    float size = 0.8f * (float)sqrt(4.0 * PI * 9.0 / N2_BALL); /* as boid.vert's u_size */
    float near = (float)sqrt(dot(sub(centre, eye), sub(centre, eye)));
    int i, k;
    for (i = 0; i < N2; i++) {
        struct cpu_boid *b = &cb[i];
        float h[3], red, col[3], dist, bright, sp;
        vec3 d;
        cpu_home(i, spin, centre, h, &red);
        d = v3(b->p[0] - eye.x, b->p[1] - eye.y, b->p[2] - eye.z);
        dist = (float)sqrt(dot(d, d));
        bright = smooth(0.0f, 1.0f, (near + 3.0f - dist) / 6.0f) * 2.8f + 0.2f;
        sp = (float)sqrt(b->v[0] * b->v[0] + b->v[1] * b->v[1] + b->v[2] * b->v[2]) / 12.0f;
        sp = sp > 0.99f ? 0.99f : sp;
        col[0] = red ? 1.0f : 0.75f, col[1] = red ? 0.15f : 0.85f, col[2] = red ? 0.08f : 1.0f;
        for (k = 0; k < 3; k++)
            col[k] += ((k == 0 ? 1.0f : k == 1 ? 0.95f : 0.85f) - col[k]) * sp * 0.8f;
        bright *= 0.8f + 1.6f * sp;
        for (k = 0; k < 6; k++) {
            GLfloat *p = cb_verts + (i * 6 + k) * 9;
            float u = cx[k] * 0.7f, v = cy[k] * 0.7f;
            p[0] = b->p[0] + (right.x * u + up.x * v) * size;
            p[1] = b->p[1] + (right.y * u + up.y * v) * size;
            p[2] = b->p[2] + (right.z * u + up.z * v) * size;
            p[3] = u * 0.5f + 0.5f, p[4] = v * 0.5f + 0.5f;
            p[5] = col[0], p[6] = col[1], p[7] = col[2], p[8] = bright;
        }
    }
}

/* TIMING: the draw passes each finished and timed (*a: when the last ended). */
static void lap(BOOL timing, double *t, double *a)
{
    double n;
    if (!timing)
        return;
    glFinish();
    n = now_s();
    *t += n - *a;
    *a = n;
}

enum { T_COUNT, T_SCAN, T_SCATTER, T_WAVE, T_SIM, T_SCENE, T_POST, T_COMP, T_DRAW, T_FINISH, T_N };
static const char *const part_name[T_N] = {"count", "scan", "scatter", "wave", "sim", "scene",
                                           "trail+blur", "picture", "swap", "finish"};

int main(void)
{
    LONG args[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    BOOL floor_on;            /* key F: the floor and the lights' reflection in it */
    struct RDArgs *rda;
    struct Window *win = NULL, *iw = NULL;
    void *ctx = NULL;
    ULONG err = 0, width = 0, height = 0;
    int n = 16384, every = 4, w0 = 640, h0 = 480, rc = 20, i;
    unsigned long f, max_frames = 0, title_frames = 0;
    BOOL full, msaa, timing, sound, vsync, quit = FALSE, paused = FALSE, timer_open = FALSE, burst_done = FALSE;
    GLuint p_draw = 0, p_floor = 0, p_count = 0, p_scan = 0, p_scatter = 0, p_sim = 0, p_wave = 0;
    GLuint p_trail = 0, p_blur = 0, p_comp = 0;
    struct rt glow = {0, 0, 0, 0}, trail[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}}, bloom[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    GLuint boids[2] = {0, 0}, counts = 0, starts = 0, cells = 0, sorted = 0, img = 0, flare = 0, floor_tex = 0, skin[2] = {0, 0};
    GLint u_vp, u_eye, u_near, f_vp, b_dir, b_addw, c_fx;
    GLint s_spin, s_pull, s_flow, s_burst, s_wander, s_time, s_look, s_dt, s_stripe, s_hawk, s_centre, w_drop, w_poke, s_skin, s_orbit, s_ring, s_ringt, s_storm, s_tpull, s_count;
    double t[T_N], start, last, last_title, a;
    float show_t = 0.0f, spin = 0.0f, *init, hawk_x = 0.0f, hawk_y = 0.0f;
    double hawk_t = -10.0; /* when the mouse last moved */
    vec3 poke = {0.0f, 0.0f, 0.0f}; /* the mouse's dent in the skin (wave.comp) */
    ULONG seed = 2463534242UL; /* the drops' places and times (xorshift) */
    float next_drop = 0.0f;   /* seconds to the next drop */
    float formed = 0.0f;      /* seconds the ball has been whole (held, not returning) */
    float fx = 1.0f;          /* the trails and the bloom: 1 while the swarm changes shape */
    BOOL post_on = TRUE;      /* they were drawn last frame */
    float pscale;             /* the lights' point size: pixels a unit at distance 1 */
    BOOL counted = TRUE;      /* sim.comp counted the boids into the grid last frame */
    BOOL es2 = FALSE;         /* key E: the ES 2.0 way (the 68k simulates N2 boids) */
    BOOL stats = TRUE;        /* key S: the stats' text */
    BOOL capture = FALSE;     /* key C: no text, no pointer, a steady 1/60 s a frame */
    BOOL idle_ok = FALSE;     /* the idle task runs (the 68k's busy share) */
    double idle_rate = 1.0, stat_t = 0.0;
    ULONG idle_last = 0, stat_frames = 0;
    float shown_fps = 0.0f, shown_busy = -1.0f, cpu_ms = 0.0f;
    float own = 0.0f, own_ms = 0.0f; /* the 68k's own work a frame (smoothed), ms */
    float busy_sum = 0.0f;
    int busy_n = 0;
    WORD last_mx = -1, last_my = -1;
    UWORD *blank = NULL;      /* the empty pointer (capture) */
    GLuint p_es2b = 0, p_es2f = 0, p_text = 0, font = 0, text_tex = 0;
    GLint d_mirror, d_direct, d_pscale, d_step, d_gain, e_vp, e_mirror, ef_vp, x_screen, x_col;
    float hawk4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    struct ball ball;
    const char *last_name = "";
    static char title[96];

    memset(t, 0, sizeof(t));
    rda = !Cli() ? NULL : /* no Shell (Workbench): the defaults; ReadArgs would wait on a console */
          ReadArgs((STRPTR) "N/N,WIDTH/N,HEIGHT/N,FULLSCREEN/S,MSAA/S,FRAMES/N,EVERY/N,TIMING/S,SOUND/S,ES2/S,CAPTURE/S,VSYNC/S,NOFLOOR/S", args, NULL);
    if (!rda && Cli()) {
        printf("usage: boingswarm-ogles3 [N=boids] [WIDTH=w] [HEIGHT=h] [FULLSCREEN] [MSAA] [FRAMES=n] "
               "[EVERY=n] [TIMING]\n");
        return 20;
    }
    if (args[0])
        n = *(LONG *)args[0];
    if (args[1])
        w0 = *(LONG *)args[1];
    if (args[2])
        h0 = *(LONG *)args[2];
    full = args[3] != 0;
    if (full && !args[1] && !args[2])
        w0 = 1280, h0 = 720;
    msaa = args[4] != 0; /* lights added up need no MSAA: off unless asked */
    if (args[5])
        max_frames = *(LONG *)args[5];
    if (args[6] && *(LONG *)args[6] >= 1)
        every = *(LONG *)args[6];
    timing = args[7] != 0;
    sound = args[8] != 0; /* silent unless asked */
    es2 = args[9] != 0;
    capture = args[10] != 0;
    vsync = args[11] != 0;
    floor_on = !args[12];
    if (rda)
        FreeArgs(rda);
    n = (n + 127) & ~127;
    if (n < 128 || n > 65536) {
        say("N: 128 to 65536\n");
        return 20;
    }

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39)))
        return 20;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0)) {
        say("needs timer.device\n");
        goto out;
    }
    timer_open = TRUE;
    TimerBase = timer_req.tr_node.io_Device;
    if (!OGLES3Open()) {
        say("needs %s 1.0 or newer: in LIBS:, or in this program's drawer or its libs/\n", OGLES3NAME);
        goto out;
    }
    if (full) {
        /* The first 32-bit mode of the size asked for that ogles3 accepts. */
        ULONG id = INVALID_ID;
        if (!(GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39))) {
            say("FULLSCREEN needs graphics.library 39\n");
            goto out;
        }
        while (!ctx && (id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
            struct DimensionInfo dims;
            if (!GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, id) || dims.MaxDepth < 24 ||
                dims.Nominal.MaxX - dims.Nominal.MinX + 1 != w0 || dims.Nominal.MaxY - dims.Nominal.MinY + 1 != h0)
                continue;
            ctx = aglCreateContextTags2(&err, OGLES3_CCT_MODEID, id, OGLES3_CCT_PISTORM_GET_WINDOW, (ULONG)&iw,
                                        OGLES3_CCT_DEPTH, 0, OGLES3_CCT_STENCIL, 0, OGLES3_CCT_PISTORM_MSAA,
                                        msaa ? 4 : 0, OGLES3_CCT_VSYNC, vsync, OGLES3_CCT_GET_WIDTH, (ULONG)&width, OGLES3_CCT_GET_HEIGHT,
                                        (ULONG)&height, OGLES3_CCT_PISTORM_ES_VERSION, 31, TAG_DONE);
        }
        if (!ctx || !iw) {
            say("no %dx%d screen mode that ogles3 can draw into (it needs a 32-bit one)\n", w0, h0);
            goto out;
        }
        ModifyIDCMP(iw, IDCMP_VANILLAKEY | IDCMP_MOUSEMOVE);
        iw->Flags |= WFLG_REPORTMOUSE;
    } else {
        win = OpenWindowTags(NULL, WA_Title, (ULONG) "Boing Swarm", WA_InnerWidth, w0, WA_InnerHeight, h0,
                             WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_ReportMouse, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_MOUSEMOVE, TAG_DONE);
        if (!win) {
            say("cannot open a window\n");
            goto out;
        }
        ctx = aglCreateContextTags2(&err, OGLES3_CCT_WINDOW, (ULONG)win, OGLES3_CCT_DEPTH, 0, OGLES3_CCT_STENCIL, 0,
                                    OGLES3_CCT_PISTORM_MSAA, msaa ? 4 : 0, OGLES3_CCT_VSYNC, vsync, OGLES3_CCT_GET_WIDTH, (ULONG)&width,
                                    OGLES3_CCT_GET_HEIGHT, (ULONG)&height, OGLES3_CCT_PISTORM_ES_VERSION, 31,
                                    TAG_DONE);
        if (!ctx) {
            say("aglCreateContextTags2 failed (error %lu)\n", (unsigned long)err);
            goto out;
        }
        iw = win;
    }
    key_window = iw;
    ActivateWindow(iw); /* a launcher's console may hold the keyboard */
    aglMakeCurrent(ctx);
    say("Boing Swarm on %s (%s), %lux%lu, %d boids, MSAA %s\n", (const char *)glGetString(GL_RENDERER),
        (const char *)glGetString(GL_VERSION), (unsigned long)width, (unsigned long)height, n, msaa ? "4x" : "off");

    if (!(p_draw = link(compile(GL_VERTEX_SHADER, boid_vert), compile(GL_FRAGMENT_SHADER, boid_frag))) ||
        !(p_count = compute(count_comp)) || !(p_scan = compute(scan_comp)) || !(p_scatter = compute(scatter_comp)) ||
        !(p_sim = compute(sim_comp)) || !(p_wave = compute(wave_comp)) ||
        !(p_floor = link(compile(GL_VERTEX_SHADER, floor_vert), compile(GL_FRAGMENT_SHADER, floor_frag))) ||
        !(p_trail = link(compile(GL_VERTEX_SHADER, screen_vert), compile(GL_FRAGMENT_SHADER, trail_frag))) ||
        !(p_blur = link(compile(GL_VERTEX_SHADER, screen_vert), compile(GL_FRAGMENT_SHADER, blur_frag))) ||
        !(p_comp = link(compile(GL_VERTEX_SHADER, screen_vert), compile(GL_FRAGMENT_SHADER, comp_frag))) ||
        !(p_es2b = link(compile(GL_VERTEX_SHADER, es2boid_vert), compile(GL_FRAGMENT_SHADER, es2boid_frag))) ||
        !(p_es2f = link(compile(GL_VERTEX_SHADER, es2floor_vert), compile(GL_FRAGMENT_SHADER, es2floor_frag))) ||
        !(p_text = link(compile(GL_VERTEX_SHADER, text_vert), compile(GL_FRAGMENT_SHADER, text_frag))))
        goto out;
    if (sound && !sound_open())
        say("no sound (audio.device)\n");

    /* The boids start far out round the scene, still (a fixed pseudo-random
     * shell 14 to 20 units out), and pour in. */
    if (!(init = (float *)calloc(n, 8 * sizeof(float))))
        goto out;
    {
        ULONG s = 12345;
        for (i = 0; i < n; i++) {
            float d[3], r, l;
            int k;
            for (k = 0; k < 3; k++) {
                s = s * 1664525UL + 1013904223UL;
                d[k] = ((s >> 8) & 0xffff) / 32767.5f - 1.0f;
            }
            l = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 0.001f;
            r = 14.0f + 6.0f * ((s >> 4) & 0xff) / 255.0f;
            for (k = 0; k < 3; k++)
                init[i * 8 + k] = d[k] / l * r * (k == 1 ? 0.5f : 1.0f);
        }
    }
    glGenBuffers(2, boids);
    for (i = 0; i < 2; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, boids[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, n * 32, init, GL_DYNAMIC_COPY);
    }
    free(init);
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
    /* The ball's skin (wave.comp): 128 x 64 cells of (height, the one a
     * step before), two buffers swapped each frame, still at first. */
    if (!(init = (float *)calloc(128 * 64, 8)))
        goto out;
    glGenBuffers(2, skin);
    for (i = 0; i < 2; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, skin[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 128 * 64 * 8, init, GL_DYNAMIC_COPY);
    }
    free(init);

    glGenTextures(1, &img);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, img);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 128, n / 128);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindImageTexture(0, img, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glActiveTexture(GL_TEXTURE0);
    font = make_font();
    flare = make_flare();
    glActiveTexture(GL_TEXTURE5); /* the floor on unit 5 */
    floor_tex = make_floor();
    glActiveTexture(GL_TEXTURE0);

    glUseProgram(p_sim);
    glUniform1i(glGetUniformLocation(p_sim, "u_n"), n);
    glUniform1i(glGetUniformLocation(p_sim, "u_ball"), BALL_OF(n));
    glUniform1i(glGetUniformLocation(p_sim, "u_max"), 6);
    glUniform1i(glGetUniformLocation(p_sim, "u_every"), every);
    glUniform1f(glGetUniformLocation(p_sim, "u_einv"), 1.0f / every);
    s_look = glGetUniformLocation(p_sim, "u_look");
    s_centre = glGetUniformLocation(p_sim, "u_centre");
    w_drop = glGetUniformLocation(p_wave, "u_drop");
    w_poke = glGetUniformLocation(p_wave, "u_poke");
    s_stripe = glGetUniformLocation(p_sim, "u_stripe");
    s_skin = glGetUniformLocation(p_sim, "u_skin");
    s_hawk = glGetUniformLocation(p_sim, "u_hawk");
    s_spin = glGetUniformLocation(p_sim, "u_spin");
    s_pull = glGetUniformLocation(p_sim, "u_pull");
    s_flow = glGetUniformLocation(p_sim, "u_flow");
    s_burst = glGetUniformLocation(p_sim, "u_burst");
    s_wander = glGetUniformLocation(p_sim, "u_wander");
    s_time = glGetUniformLocation(p_sim, "u_time");
    s_orbit = glGetUniformLocation(p_sim, "u_orbit");
    s_ring = glGetUniformLocation(p_sim, "u_ring");
    s_ringt = glGetUniformLocation(p_sim, "u_ringt");
    s_storm = glGetUniformLocation(p_sim, "u_storm");
    s_tpull = glGetUniformLocation(p_sim, "u_tpull");
    s_count = glGetUniformLocation(p_sim, "u_count");
    s_dt = glGetUniformLocation(p_sim, "u_dt");
    glUseProgram(p_count);
    glUniform1i(glGetUniformLocation(p_count, "u_n"), n);
    glUseProgram(p_scatter);
    glUniform1i(glGetUniformLocation(p_scatter, "u_n"), n);
    glUseProgram(p_draw);
    u_vp = glGetUniformLocation(p_draw, "u_vp");
    /* the point sprites' size: pixels a unit at distance 1 (45 degrees high) */
    pscale = height * 0.5f / (float)tan(22.5 * PI / 180.0);
    d_pscale = glGetUniformLocation(p_draw, "u_pscale");
    d_step = glGetUniformLocation(p_draw, "u_step");
    d_gain = glGetUniformLocation(p_draw, "u_gain");
    glUniform1f(d_pscale, pscale);
    u_eye = glGetUniformLocation(p_draw, "u_eye");
    u_near = glGetUniformLocation(p_draw, "u_near");
    d_mirror = glGetUniformLocation(p_draw, "u_mirror");
    d_direct = glGetUniformLocation(p_draw, "u_direct");
    glUniform2f(glGetUniformLocation(p_draw, "u_res"), 1.0f / width, 1.0f / height);
    glUniform1i(glGetUniformLocation(p_draw, "u_boids"), 1);
    glUniform1i(glGetUniformLocation(p_draw, "u_flare"), 0);
    /* Each light 1.6 times as wide as its share of the ball's surface
     * (4 pi 3^2 / n): the cores apart, the glows met. */
    glUniform1f(glGetUniformLocation(p_draw, "u_size"), 0.8f * (float)sqrt(4.0 * PI * 9.0 / BALL_OF(n)));
    glUseProgram(p_floor);
    f_vp = glGetUniformLocation(p_floor, "u_vp");
    glUniform2f(glGetUniformLocation(p_floor, "u_res"), 1.0f / width, 1.0f / height);
    glUniform1i(glGetUniformLocation(p_floor, "u_floor"), 5);
    glUseProgram(p_trail);
    glUniform1i(glGetUniformLocation(p_trail, "u_glow"), 0);
    glUniform1i(glGetUniformLocation(p_trail, "u_prev"), 3);
    glUniform1f(glGetUniformLocation(p_trail, "u_decay"), 0.92f); /* long streaks (0.8: they were lost) */
    glUseProgram(p_blur);
    glUniform1i(glGetUniformLocation(p_blur, "u_src"), 0);
    b_dir = glGetUniformLocation(p_blur, "u_dir");
    b_addw = glGetUniformLocation(p_blur, "u_addw");
    glUniform1i(glGetUniformLocation(p_blur, "u_add"), 3);
    glUseProgram(p_comp);
    glUniform1i(glGetUniformLocation(p_comp, "u_bloom"), 0);
    c_fx = glGetUniformLocation(p_comp, "u_fx");
    glUniform2f(glGetUniformLocation(p_comp, "u_res"), 1.0f / width, 1.0f / height);
    glUseProgram(p_es2b);
    e_vp = glGetUniformLocation(p_es2b, "u_vp");
    e_mirror = glGetUniformLocation(p_es2b, "u_mirror");
    glUniform1i(glGetUniformLocation(p_es2b, "u_flare"), 0);
    glUseProgram(p_es2f);
    ef_vp = glGetUniformLocation(p_es2f, "u_vp");
    glUniform3f(glGetUniformLocation(p_es2f, "u_clear"), 0.01f, 0.01f, 0.03f);
    glUseProgram(p_text);
    x_screen = glGetUniformLocation(p_text, "u_screen");
    x_col = glGetUniformLocation(p_text, "u_col");
    glUniform1i(glGetUniformLocation(p_text, "u_font"), 3);
    glUniform2f(x_screen, 2.0f / width, 2.0f / height);
    if (!cpu_init(n, every) || !text_init(n - BALL_OF(n))) {
        printf("Not enough memory for the 68k's boids\n");
        goto out;
    }
    /* the text boids' places for sim.comp: a float texture on unit 6 */
    glActiveTexture(GL_TEXTURE6);
    glGenTextures(1, &text_tex);
    glBindTexture(GL_TEXTURE_2D, text_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 128, (n - BALL_OF(n) + 127) / 128, 0, GL_RGBA, GL_FLOAT, tx_place[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(p_sim);
    glUniform1i(glGetUniformLocation(p_sim, "u_text"), 6);

    /* The targets, all at quarter size (RGBA16F): the glow (the lights
     * alone), the trails (two swapped each frame) and the bloom. The scene
     * itself goes straight to the screen. */
    if (!make_rt(&glow, width / 4, height / 4, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT))
        goto out;
    for (i = 0; i < 2; i++)
        if (!make_rt(&trail[i], width / 4, height / 4, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT) ||
            !make_rt(&bloom[i], width / 4, height / 4, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT))
            goto out;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, flare);
    memset(&ball, 0, sizeof(ball));
    ball_step(&ball, 0.0f);
    /* The boids are lights: added up, no depth test (the far side shows
     * through the near one). */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glViewport(0, 0, width, height);
    glClearColor(0.01f, 0.01f, 0.03f, 1.0f);

    /* The 68k's busy share: the idle task's count while this one sleeps
     * 0.6 s (after 0.1 s for it to start) is an idle 68k's rate. */
    if ((idle_ok = idle_start()) != 0) {
        double t0;
        ULONG c0;
        Delay(5);
        t0 = now_s(), c0 = idle_count;
        Delay(30);
        idle_rate = (idle_count - c0) / (now_s() - t0);
        say("an idle 68k: %.0f counts a second (the busy share's measure)\n", idle_rate);
        idle_last = idle_count;
    }
    if ((blank = (UWORD *)AllocMem(16, MEMF_CHIP | MEMF_CLEAR)) != NULL && capture)
        SetPointer(iw, blank, 1, 1, 0, 0);
    else if (full)
        ClearPointer(iw); /* ogles3 hides the pointer on its screen: the hawk needs it */
    start = last = last_title = now_s();
    stat_t = start;
    for (f = 0; (!max_frames || f < max_frames) && !quit; f++) {
        struct IntuiMessage *m;
        struct show sh;
        float dt, cam;
        double nw = now_s();
        vec3 eye, right, up;
        mat4 vp;
        GLuint src = boids[f & 1], dst = boids[(f + 1) & 1];

        while ((m = (struct IntuiMessage *)GetMsg(iw->UserPort)) != NULL) {
            if (m->Class == IDCMP_CLOSEWINDOW || (m->Class == IDCMP_VANILLAKEY && m->Code == 27))
                quit = TRUE;
            else if (m->Class == IDCMP_VANILLAKEY && m->Code == ' ')
                paused = !paused;
            else if (m->Class == IDCMP_VANILLAKEY && (m->Code | 0x20) == 'e') {
                es2 = !es2;
                say("%s\n", es2 ? "the ES 2.0 way: the 68k simulates the swarm" : "OpenGL ES 3.1: the GPU simulates the swarm");
            } else if (m->Class == IDCMP_VANILLAKEY && (m->Code | 0x20) == 's')
                stats = !stats;
            else if (m->Class == IDCMP_VANILLAKEY && (m->Code | 0x20) == 'f')
                floor_on = !floor_on;
            else if (m->Class == IDCMP_VANILLAKEY && (m->Code | 0x20) == 'c') {
                capture = !capture;
                if (capture && blank)
                    SetPointer(iw, blank, 1, 1, 0, 0);
                else
                    ClearPointer(iw);
            }
            ReplyMsg((struct Message *)m);
        }
        /* The mouse: where the pointer is on the screen, read each frame
         * (whatever moves it: events come only to an active window that
         * asked for them); -1..1 across the drawing. */
        {
            WORD mx = iw->WScreen->MouseX - iw->LeftEdge - iw->BorderLeft;
            WORD my = iw->WScreen->MouseY - iw->TopEdge - iw->BorderTop;
            if (last_mx >= 0 && (mx != last_mx || my != last_my)) {
                hawk_x = (float)mx / width * 2.0f - 1.0f;
                hawk_y = 1.0f - (float)my / height * 2.0f;
                hawk_t = nw;
            }
            last_mx = mx, last_my = my;
        }
        /* The show runs on the clock, a step at most 1/30 s (a slow frame
         * slows it instead of tearing the flock apart). */
        dt = (float)(nw - last);
        last = nw;
        dt = dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt;
        if (capture) /* steady for a recording: every frame 1/60 s of the show */
            dt = 1.0f / 60.0f;
        if (paused)
            dt = 0.0f;
        show_t += dt;
        if (show_t >= RETURN_END)
            show_t = BALL_START, burst_done = FALSE;
        spin += dt * 1.2f;
        ball_step(&ball, dt);
        show_at(show_t, &sh);
        /* The ball bursts at BURST_AT; until then a held ball boings at the
         * low point of each sink. */
        if (show_t >= BURST_AT && !burst_done) {
            sh.burst = 5.0f;
            burst_done = TRUE;
            if (snd_alloc)
                sound_play(1, ball.x / BALL_X * 0.6f, 1.0f);
        } else if (ball.hit && sh.pull > 0.5f && sh.stripe < 0.0f && snd_alloc)
            sound_play(0, ball.x / BALL_X * 0.8f, 0.9f);
        if (sh.name != last_name) { /* each part of the show, and the last one's frame rate */
            static unsigned long part_f;
            static double part_t, part_pass[T_N];
            if (*last_name && nw > part_t && f > part_f) {
                say("        (%.1f fps", (f - part_f) / (nw - part_t));
                for (i = 0; timing && i < T_N; i++)
                    say("%s %s %.2f", i ? "" : ";", part_name[i], (t[i] - part_pass[i]) * 1000.0 / (f - part_f));
                say(")\n");
            }
            memcpy(part_pass, t, sizeof(part_pass));
            say("%5.1f s: %s\n", show_t, sh.name);
            last_name = sh.name, part_f = f, part_t = nw;
        }

        /* The camera circles slowly, above the floor, looking at the
         * middle of the ball's bounce. */
        cam = (float)(nw - start) * 0.06f;
        eye = v3((float)sin(cam) * 17.0f, 6.0f, (float)cos(cam) * 17.0f);
        {
            vec3 at = v3(0.0f, 3.5f, 0.0f), fwd = normalize(sub(at, eye)), dir, hawk;
            float th = (float)tan(22.5 * PI / 180.0), asp = (float)width / height, k;
            vp = mul(perspective(45.0f * PI / 180.0f, asp, 1.0f, 60.0f),
                     look_at(eye, at, v3(0.0f, 1.0f, 0.0f), &right, &up));
            /* The hawk: the mouse's ray met with the plane facing the
             * camera 2.6 units before the ball's centre (just before its near
             * side, where the seen boids are); it hunts for 1.5 s after the
             * mouse last moved. */
            dir = v3(fwd.x + (right.x * hawk_x * asp + up.x * hawk_y) * th,
                     fwd.y + (right.y * hawk_x * asp + up.y * hawk_y) * th,
                     fwd.z + (right.z * hawk_x * asp + up.z * hawk_y) * th);
            k = (dot(sub(ball.centre, eye), fwd) - 2.6f) / dot(dir, fwd);
            hawk = v3(eye.x + dir.x * k, eye.y + dir.y * k, eye.z + dir.z * k);
            hawk4[0] = hawk.x, hawk4[1] = hawk.y, hawk4[2] = hawk.z;
            hawk4[3] = nw - hawk_t < 1.5 ? 1.0f : 0.0f;
            glUseProgram(p_sim);
            glUniform4f(s_hawk, hawk.x, hawk.y, hawk.z, hawk4[3]);
            /* Where the hunting hawk is near the ball it dents the skin:
             * its direction from the centre as the skin's cell. */
            {
                vec3 d = sub(hawk, ball.centre);
                float l = (float)sqrt(dot(d, d));
                poke.z = 0.0f;
                if (nw - hawk_t < 1.5 && l < 4.0f && l > 0.01f) {
                    poke.x = ((float)atan2(d.z, d.x) / (2.0f * PI) + 0.5f) * 128.0f - 0.5f;
                    poke.y = ((float)asin(d.y / l) / PI + 0.5f) * 64.0f - 0.5f;
                    poke.z = 0.06f;
                }
            }
        }
        {
            /* The text, on the camera's side: from the start of its
             * forming (in 1.5 s) it scrolls round the ball, 20 degrees a second, from
             * 60 degrees right (sharp from about 30) past the front to 45
             * left at 5.25 s, and on the same way (the way the rings turn) while
             * it becomes Saturn's rings (over 2 s), held 0.5 s, then the storm. */
            static float clock;
            float near = (float)atan2(eye.z - ball.centre.z, eye.x - ball.centre.x);
            clock = sh.pull > 0.1f && sh.stripe < 0.0f ? clock + dt : 0.0f;
            tx_pull = smooth(0.0f, 1.5f, clock);
            tx_orbit = near - PI * 0.5f - PI / 3.0f + PI / 9.0f * clock;
            tx_ring = smooth(5.25f, 7.25f, clock);
            tx_storm = smooth(7.75f, 9.25f, clock);
            tx_ringt += dt;
        }

        /* the 68k's own work this frame so far (the show, the camera; the
         * ES 2.0 way adds its swarm below) */
        own = (float)(now_s() - nw) * 1000.0f;
        if (!es2) { /* OpenGL ES 3.1: the GPU simulates and draws */
            /* The skin: a step of its waves a frame (the two buffers swapped
             * each frame; the newest at binding 7, where sim.comp reads it).
             * A drop falls on it at a random place every 3 seconds, its ring
             * running across the ball. */
            glUseProgram(p_wave);
            glUniform3f(w_poke, poke.x, poke.y, poke.z);
            /* The waves wait until the ball has been whole for 3 s, then fade
             * in over 2 s. */
            formed = sh.pull >= 1.0f && sh.stripe < 0.0f ? formed + dt : 0.0f;
            next_drop -= dt;
            {
                float dx = 0.0f, dy = 0.0f, ds = 0.0f;
                if (next_drop <= 0.0f && formed > 3.0f) {
                    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
                    dx = (float)(seed & 127);
                    dy = 8.0f + (float)((seed >> 7) % 48);
                    ds = 1.5f;
                    next_drop = 3.0f;
                }
                glUniform3f(w_drop, dx, dy, ds);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, skin[f & 1]);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, skin[!(f & 1)]);
                if (formed > 2.5f) /* (unseen before: not run) */
                    run(p_wave, 64, &t[T_WAVE]);
            }

            /* The grid only while boids look round: none does while all are
             * held fast (the formed ball, the text), and then sim.comp counts
             * none into it (u_count) and the scan and the scatter are
             * skipped. The first frame looking round again uses the last
             * grid (only other neighbours for a frame). */
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, src);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dst);
            if (f == 0) /* later sim.comp counts the boids where they moved */
                run(p_count, n / 128, &t[T_COUNT]);
            if (counted) {
                run(p_scan, 1, &t[T_SCAN]);
                run(p_scatter, n / 128, &t[T_SCATTER]);
            }
            counted = sh.pull < 0.999f || sh.stripe >= 0.0f || tx_pull < 0.999f || tx_storm > 0.0f;
            glUseProgram(p_sim);
            glUniform1f(s_spin, spin);
            glUniform1f(s_pull, sh.pull);
            glUniform1f(s_flow, sh.flow);
            glUniform1f(s_burst, sh.burst);
            glUniform3f(s_wander, sh.wander.x, sh.wander.y, sh.wander.z);
            glUniform1f(s_time, show_t);
            glUniform1i(s_look, (int)(f % every));
            glUniform1f(s_dt, dt);
            glUniform3f(s_centre, ball.centre.x, ball.centre.y, ball.centre.z);
            glUniform1f(s_orbit, tx_orbit);
            glUniform1f(s_ring, tx_ring);
            glUniform1f(s_ringt, tx_ringt);
            glUniform1f(s_storm, tx_storm);
            glUniform1f(s_tpull, tx_pull);
            glUniform1i(s_count, counted);
            glUniform1f(s_stripe, sh.stripe);
            glUniform1f(s_skin, smooth(3.0f, 5.0f, formed));
            run(p_sim, n / 128, &t[T_SIM]);

            a = now_s();
            /* The trails and the bloom (3, 4) only while the swarm changes
             * shape: they fade out as the ball comes together (the gathering's
             * or the return's last stretch: too much on the ball), gone before
             * it is whole, and are skipped while out, the glow with them. */
            fx = 1.0f - smooth(0.6f, 0.9f, sh.stripe >= 0.0f ? sh.stripe : sh.pull);
            glUseProgram(p_draw);
            glUniformMatrix4fv(u_vp, 1, GL_FALSE, vp.m);
            glUniform3f(u_eye, eye.x, eye.y, eye.z);
            glUniform1f(u_near, (float)sqrt(dot(sub(ball.centre, eye), sub(ball.centre, eye))));
            glUniform1f(d_gain, 1.0f - 0.45f * fx); /* the lights dimmer while the trails add theirs */
            /* 2. While the trails and the bloom show, first the glow: the
             * lights alone, small, into a quarter-size target. 3. The trails
             * there: the glow added to the last frame's, faded (trail.frag).
             * 4. The bloom: the trails blurred across, then down. Back from
             * being skipped, the stale trails are cleared first. All before
             * the screen's pass, which then runs unbroken. */
            if (fx > 0.0f && !post_on) {
                static const GLfloat black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (i = 0; i < 2; i++) {
                    glBindFramebuffer(GL_FRAMEBUFFER, trail[i].fbo);
                    glClearBufferfv(GL_COLOR, 0, black);
                }
            }
            post_on = fx > 0.0f;
            if (post_on) {
                glBindFramebuffer(GL_FRAMEBUFFER, glow.fbo);
                glViewport(0, 0, glow.w, glow.h);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_BLEND);
                glUniform1f(d_direct, 0.0f);
                glUniform1f(d_mirror, 0.0f);
                glUniform1i(d_step, 4); /* every fourth light, four times as bright: blurred anyway */
                glUniform1f(d_pscale, pscale * glow.h / height);
                glDrawArrays(GL_POINTS, 0, n / 4);
                glDisable(GL_BLEND);
                glUseProgram(p_trail);
                glBindFramebuffer(GL_FRAMEBUFFER, trail[f & 1].fbo);
                bind2(glow.tex, trail[!(f & 1)].tex);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glUseProgram(p_blur);
                glBindFramebuffer(GL_FRAMEBUFFER, bloom[0].fbo);
                glUniform2f(b_dir, 1.0f / trail[0].w, 0.0f);
                glUniform1f(b_addw, 0.0f);
                bind2(trail[f & 1].tex, 0);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindFramebuffer(GL_FRAMEBUFFER, bloom[1].fbo);
                glUniform2f(b_dir, 0.0f, 1.0f / bloom[0].h);
                glUniform1f(b_addw, 0.6f); /* the sharp trails added: one read on the screen */
                bind2(bloom[0].tex, trail[f & 1].tex);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindTexture(GL_TEXTURE_2D, flare); /* unit 0 again: the lights' texture */
                glUseProgram(p_draw);
                glUniform1f(d_pscale, pscale);
            }
            lap(timing, &t[T_POST], &a);
            /* 5. The screen, in one pass: the floor, the lights' reflections
             * in it (boid.vert, u_mirror), the lights, all added up (the 8-bit
             * screen's saturating adds clamp the light), the picture's gain
             * and vignette done by each (u_direct); then the trails and the
             * bloom added (comp.frag). Floats and a picture pass reading them
             * back cost some 10 ms a frame at 1280x720. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            glClearColor(0.01f, 0.01f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (floor_on) {
                glUseProgram(p_floor);
                glUniformMatrix4fv(f_vp, 1, GL_FALSE, vp.m);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glUseProgram(p_draw);
            }
            glEnable(GL_BLEND);
            glUniform1f(d_direct, 1.0f);
            if (floor_on) {
                glUniform1f(d_mirror, 1.0f);
                glUniform1i(d_step, 2); /* every second light, twice as bright */
                glDrawArrays(GL_POINTS, 0, n / 2);
            }
            glUniform1f(d_mirror, 0.0f);
            glUniform1i(d_step, 1);
            glDrawArrays(GL_POINTS, 0, n);
            lap(timing, &t[T_SCENE], &a);
            if (post_on) {
                glUseProgram(p_comp);
                glUniform1f(c_fx, fx);
                bind2(bloom[1].tex, 0);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindTexture(GL_TEXTURE_2D, flare); /* unit 0 again: the lights' texture */
            }
            lap(timing, &t[T_COMP], &a);
        } else {
            /* The ES 2.0 way: the 68k simulates N2 boids (cpu_step), builds
             * their squares (cpu_verts), and they are drawn from client
             * arrays straight to the screen with GLSL ES 1.00 shaders: no
             * compute, no instancing, no float targets, no bloom. */
            static const GLfloat floor_xz[8] = {-14, -14, 14, -14, -14, 14, 14, 14};
            double c0 = now_s();
            cpu_step(&sh, dt, show_t, spin, ball.centre, hawk4, (int)f);
            cpu_verts(spin, ball.centre, eye, right, up);
            cpu_ms = cpu_ms * 0.9f + (float)(now_s() - c0) * 100.0f; /* ms, smoothed */
            own += (float)(now_s() - c0) * 1000.0f;
            a = now_s();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            glClearColor(0.01f, 0.01f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_BLEND);
            glEnableVertexAttribArray(0);
            if (floor_on) {
                glUseProgram(p_es2f);
                glUniformMatrix4fv(ef_vp, 1, GL_FALSE, vp.m);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, floor_xz);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            glEnable(GL_BLEND);
            glUseProgram(p_es2b);
            glUniformMatrix4fv(e_vp, 1, GL_FALSE, vp.m);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, cb_verts);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, cb_verts + 3);
            glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, cb_verts + 5);
            if (floor_on) { /* the reflections first, as the ES 3.1 way's */
                glUniform1f(e_mirror, 1.0f);
                glDrawArrays(GL_TRIANGLES, 0, N2 * 6);
                glUniform1f(e_mirror, 0.0f);
            }
            glDrawArrays(GL_TRIANGLES, 0, N2 * 6);
            glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(0);
        }

        /* The stats (key S; none in capture mode), every half second: who
         * simulates how many boids, frames a second, the 68k's busy share
         * (the idle task's count against an idle 68k's). */
        own_ms = own_ms * 0.95f + own * 0.05f;
        stat_frames++;
        if (nw - stat_t >= 0.5) {
            shown_fps = (float)(stat_frames / (nw - stat_t));
            if (idle_ok) {
                float idle = (float)((idle_count - idle_last) / (nw - stat_t) / idle_rate);
                shown_busy = 100.0f * (1.0f - (idle > 1.0f ? 1.0f : idle));
                busy_sum += shown_busy, busy_n++;
                idle_last = idle_count;
            }
            stat_frames = 0;
            stat_t = nw;
        }
        if (stats && !capture) {
            static char line[TEXT_MAX];
            float sc = height >= 600 ? 2.0f : 1.0f, y = height - 12.0f * sc;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            glUseProgram(p_text);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, font);
            glActiveTexture(GL_TEXTURE0);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glUniform4f(x_col, 1.0f, 0.35f, 0.3f, 0.9f);
            draw_text(es2 ? "BOING SWARM - THE OPENGL ES 2.0 WAY" : "BOING SWARM - OPENGL ES 3.1", 8.0f * sc, y, sc);
            glUniform4f(x_col, 0.9f, 0.92f, 1.0f, 0.85f);
            if (es2)
                sprintf(line, "%d BOIDS SIMULATED BY THE 68K (%.1f MS A FRAME)", N2, cpu_ms);
            else
                sprintf(line, "%d BOIDS SIMULATED BY THE GPU (PRESS E TO SWITCH TO CPU)", n);
            draw_text(line, 8.0f * sc, y - 10.0f * sc, sc);
            sprintf(line, "%.1f FPS    68K WORK %.1f MS A FRAME (%d%%)", shown_fps, own_ms,
                    (int)(own_ms * shown_fps / 10.0f + 0.5f));
            draw_text(line, 8.0f * sc, y - 20.0f * sc, sc);
            glUniform4f(x_col, 0.6f, 0.6f, 0.7f, 0.7f);
            draw_text("E: ES 2/3   S: STATS   F: FLOOR   C: CAPTURE   SPACE: PAUSE", 8.0f * sc, 6.0f * sc, sc);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(0);
            glBlendFunc(GL_ONE, GL_ONE);
        }
        aglSwapBuffers();
        t[T_DRAW] += now_s() - a;
        if (timing) { /* the render waited for here, not in the next dispatch */
            a = now_s();
            glFinish();
            t[T_FINISH] += now_s() - a;
        }

        title_frames++;
        if (nw - last_title >= 1.0) {
            if (win) {
                sprintf(title, "Boing Swarm: %d boids, %.1f fps", n, title_frames / (nw - last_title));
                SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
            }
            title_frames = 0;
            last_title = nw;
        }
    }
    a = now_s() - start;
    say("%lu frames in %.2f s: %.1f fps\n", f, a, f / a);
    if (busy_n)
        say("the 68k busy %.0f%% on average (never idle: ogles3 waits for the GPU on it)\n", busy_sum / busy_n);
    say("its own work %.2f ms a frame\n", own_ms);
    if (timing && f) {
        say("a frame (ms):");
        for (i = 0; i < T_N; i++)
            say(" %s %.2f", part_name[i], t[i] * 1000.0 / f);
        say("\n");
    }
    rc = 0;

out:
    if (idle_ok)
        idle_end();
    if (ctx) {
        GLuint progs[13] = {p_draw, p_floor, p_count, p_scan, p_scatter, p_sim, p_wave, p_trail, p_blur, p_comp,
                            p_es2b, p_es2f, p_text};
        for (i = 0; i < 13; i++)
            if (progs[i])
                glDeleteProgram(progs[i]);
        glDeleteBuffers(2, boids);
        glDeleteBuffers(2, skin);
        if (counts) {
            glDeleteBuffers(1, &counts);
            glDeleteBuffers(1, &starts);
            glDeleteBuffers(1, &cells);
            glDeleteBuffers(1, &sorted);
        }
        if (img)
            glDeleteTextures(1, &img);
        if (floor_tex)
            glDeleteTextures(1, &floor_tex);
        if (flare)
            glDeleteTextures(1, &flare);
        if (font)
            glDeleteTextures(1, &font);
        if (text_tex)
            glDeleteTextures(1, &text_tex);
        for (i = 0; i < 2; i++)
            free_rt(&trail[i]), free_rt(&bloom[i]);
        free_rt(&glow);
        if (blank && capture && iw)
            ClearPointer(iw); /* before the context closes a full screen's window */
        aglDestroyContext(ctx);
    }
    sound_close();
    cpu_free();
    if (blank)
        FreeMem(blank, 16);
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
