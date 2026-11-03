/*
 * Juggler RT for OpenGL ES 3.1 (ogles3.library): Eric Graham's Juggler
 * (1987), ray traced live. In 1987 one picture of it took his ray tracer
 * about an hour on an Amiga; here the GPU's compute shaders trace every
 * pixel of every frame (docs/GLES3_PLAN.md, "Plan: Juggler RT"):
 *   - animate.comp moves the figure (robot.dat's spheres, juggling three
 *     mirror balls) in a storage buffer: the 68k only says the time;
 *   - trace.comp is rt1.c's ray tracer, an 8 x 8 tile a work group: the
 *     spheres in a tile's view gathered in shared memory first (atomics),
 *     shadow and reflected rays through each object's bounding sphere;
 *   - 1987: the picture as rt2.c's 4-bit levels at 320x200, then
 *     ham.comp decodes it as rt3.c's HAM6 screen did, fringes and all;
 *   - 2026: the light at the window's size, and once nothing moves (Space,
 *     or a still scene) pictures with jittered rays and lamp points added
 *     up: smooth edges and soft shadows.
 *
 *   juggler-ogles3 [SCENE=file.dat] [WIDTH=w] [HEIGHT=h] [FULLSCREEN]
 *                  [MODERN] [SCALE=percent] [ES2] [FRAMES=n] [TIMING] [VSYNC]
 *
 * SCENE: a scene file of the 1987 ray tracer's (robot.dat, ele.dat and
 * dragon.dat are built in: key N); without it, the juggler. MODERN starts in
 * 2026 (1987 otherwise). SCALE: 2026's picture as a share of the window's
 * size (100; keys + and -). Keys: M 1987/2026, E the ES 2.0 way, N the next
 * scene, O the camera: swinging left and right (at the start), round and
 * round, still; R the camera back, H the keys' text, Space pause, Esc quit. The mouse: drag to turn the camera round the scene.
 *
 * The ray tracer is Eric Graham's ("anyone can do what they want with the
 * code, so long as I get a mention"). This program: MIT License.
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
#include <devices/timer.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/graphics.h>
#include <proto/ogles3.h>
#include <GLES3/gl31.h>
#include "juggler_scene.h"
#include "juggler-ogles3_shaders.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;
struct GfxBase *GfxBase;
static struct timerequest timer_req;
static struct Window *key_window;

static double now_s(void)
{
    struct TimeVal tv;
    GetSysTime((TimeVal_Type *)&tv);
    return tv.tv_secs + tv.tv_micro / 1e6;
}

/* Status lines (from a Shell only), then the window with the keys made
 * active again: a console a launcher opened takes the keyboard. */
static void say(const char *fmt, ...)
{
    va_list ap;
    if (!Cli())
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (key_window)
        ActivateWindow(key_window);
}

#define J_LOG say
#define J_NOW() now_s()
#include "juggler_gl.h"

static struct jgl J;

/* ---- the scenes ---- */
#define N_SCENES 4
static const char *const scene_name[N_SCENES] = {"the juggler", "robot.dat", "ele.dat", "dragon.dat"};
static const char *const scene_upper[N_SCENES] = {"THE JUGGLER", "ROBOT.DAT", "ELE.DAT", "DRAGON.DAT"};

/* Scene n of the built-in ones, or the file (n < 0); 0 if it is not one. */
static int load_scene(int n, const char *file, struct jscene *sc)
{
    const char *text = robot_dat, *err;
    char *buf = NULL;
    int ok;
    if (n < 0) {
        BPTR fh = Open((STRPTR)file, MODE_OLDFILE);
        LONG len;
        if (!fh) {
            say("cannot open %s\n", file);
            return 0;
        }
        Seek(fh, 0, OFFSET_END);
        len = Seek(fh, 0, OFFSET_BEGINNING);
        if (len <= 0 || !(buf = malloc(len + 1)) || Read(fh, buf, len) != len) {
            Close(fh);
            free(buf);
            say("cannot read %s\n", file);
            return 0;
        }
        Close(fh);
        buf[len] = 0;
        text = buf;
    } else if (n == 2)
        text = ele_dat;
    else if (n == 3)
        text = dragon_dat;
    ok = j_parse(sc, text, &err);
    free(buf);
    if (!ok) {
        say("%s is not a scene: %s\n", n < 0 ? file : scene_name[n], err);
        return 0;
    }
    sc->juggler = n == 0;
    return 1;
}

/* ---- the camera: round the scene's middle, as far as the file's ---- */
struct orbit {
    float target[3], dist; /* what it looks at, how far */
    float alt, az, zoom;   /* now */
    float swing;           /* degrees from az: the camera swinging left and right */
};

static void orbit_init(struct orbit *o, const struct jscene *sc, const float *centre)
{
    float d = 0.0174533f, v[3];
    int k;
    v[0] = (float)(cos(sc->az * d) * cos(sc->alt * d));
    v[1] = (float)(sin(sc->az * d) * cos(sc->alt * d));
    v[2] = (float)sin(sc->alt * d);
    o->dist = (centre[0] - sc->obs[0]) * v[0] + (centre[1] - sc->obs[1]) * v[1] + (centre[2] - sc->obs[2]) * v[2];
    if (o->dist < 1.0f)
        o->dist = 1.0f;
    for (k = 0; k < 3; k++)
        o->target[k] = sc->obs[k] + v[k] * o->dist;
    o->alt = sc->alt, o->az = sc->az, o->zoom = 1.0f, o->swing = 0.0f;
}

static void orbit_camera(const struct orbit *o, const struct jscene *sc, float aspect, struct jcamera *c)
{
    float d = 0.0174533f, eye[3], r = o->dist * o->zoom, az = o->az + o->swing;
    eye[0] = o->target[0] - (float)(cos(az * d) * cos(o->alt * d)) * r;
    eye[1] = o->target[1] - (float)(sin(az * d) * cos(o->alt * d)) * r;
    eye[2] = o->target[2] - (float)sin(o->alt * d) * r;
    if (eye[2] < 0.2f)
        eye[2] = 0.2f;
    j_camera(eye, o->alt, az, sc->focal, aspect, c);
}

/* ---- the keys' text (key H): Boing Swarm's 5x7 font of capitals, figures
 * and a few signs, made into a texture once (64 cells of 6x8: ' ' to '_'),
 * drawn as squares from a client array ---- */
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
    {'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
};

#define FONT_W 384 /* 64 cells of 6 x 8 */
#define FONT_H 8

static GLuint make_font(void)
{
    static unsigned char px[FONT_W * FONT_H * 4];
    GLuint t;
    unsigned g;
    int x, y;
    for (g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++) {
        int cell = glyphs[g].c - ' ';
        for (y = 0; y < 7; y++)
            for (x = 0; x < 5; x++)
                if (glyphs[g].row[y][x] == '#') {
                    unsigned char *p = px + ((6 - y) * FONT_W + cell * 6 + x) * 4; /* rows 0-6 from the bottom */
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
static GLint text_pos = 0, text_uv = 1; /* the text program's attributes */

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
    glVertexAttribPointer(text_pos, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(text_uv, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLES, 0, k);
}

/* A line of text with a dark shadow a font pixel below and right of it: it
 * reads over the bright checks too (col: the program's u_col). */
static void text_line(GLint col, float r, float g, float b, const char *s, float x, float y, float scale)
{
    glUniform4f(col, 0.0f, 0.0f, 0.0f, 0.8f);
    draw_text(s, x + scale, y - scale, scale);
    glUniform4f(col, r, g, b, 1.0f);
    draw_text(s, x, y, scale);
}

/* The radical inverse of i in base b (the still pictures' jitter). */
static float halton(int i, int b)
{
    float f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= b;
        r += f * (i % b);
        i /= b;
    }
    return r;
}

static const char *const part_name[JT_N] = {"animate", "trace", "ham", "show"};

int main(void)
{
    LONG args[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    struct RDArgs *rda;
    struct Window *win = NULL, *iw = NULL;
    void *ctx = NULL;
    ULONG err = 0, width = 0, height = 0;
    int w0 = 640, h0 = 480, rc = 20, scale = 100, scene = 0, still = 0, i;
    unsigned long f, max_frames = 0, title_frames = 0;
    BOOL full, timing, vsync, modern, es2, quit = FALSE, paused = FALSE, timer_open = FALSE, help = TRUE;
    BOOL drag = FALSE, moved = TRUE;
    WORD drag_x = 0, drag_y = 0;
    const char *file = NULL;
    struct jscene sc;
    struct orbit orb;
    double start, last, last_title;
    float show_t = 0.0f, swing_t = 0.0f, shown_fps = 0.0f;
    int cam = 1; /* key O: 0 still, 1 swinging left and right, 2 round and round */
    static const char *const cam_name[3] = {"STILL", "SWINGING", "ROUND"};
    GLuint p_text = 0, font = 0;
    GLint x_screen = -1, x_col = -1;
    double stat_t;
    unsigned long stat_frames = 0;
    static char title[96];

    rda = !Cli() ? NULL : /* no Shell (Workbench): the defaults; ReadArgs would wait on a console */
          ReadArgs((STRPTR) "SCENE/K,WIDTH/N,HEIGHT/N,FULLSCREEN/S,MODERN/S,SCALE/N,ES2/S,FRAMES/N,TIMING/S,VSYNC/S",
                   args, NULL);
    if (!rda && Cli()) {
        printf("usage: juggler-ogles3 [SCENE=file.dat] [WIDTH=w] [HEIGHT=h] [FULLSCREEN] [MODERN] [SCALE=percent] "
               "[ES2] [FRAMES=n] [TIMING] [VSYNC]\n");
        return 20;
    }
    if (args[0] && (file = strdup((const char *)args[0])) != NULL)
        scene = -1;
    if (args[1])
        w0 = *(LONG *)args[1];
    if (args[2])
        h0 = *(LONG *)args[2];
    full = args[3] != 0;
    if (full && !args[1] && !args[2])
        w0 = 1280, h0 = 720;
    modern = args[4] != 0;
    if (args[5])
        scale = *(LONG *)args[5];
    if (scale < 10 || scale > 100)
        scale = 100;
    es2 = args[6] != 0;
    if (args[7])
        max_frames = *(LONG *)args[7];
    timing = args[8] != 0;
    vsync = args[9] != 0;
    if (rda)
        FreeArgs(rda);

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
                                        OGLES3_CCT_DEPTH, 0, OGLES3_CCT_STENCIL, 0, OGLES3_CCT_VSYNC, vsync,
                                        OGLES3_CCT_GET_WIDTH, (ULONG)&width, OGLES3_CCT_GET_HEIGHT, (ULONG)&height,
                                        OGLES3_CCT_PISTORM_ES_VERSION, 31, TAG_DONE);
        }
        if (!ctx || !iw) {
            say("no %dx%d screen mode that ogles3 can draw into (it needs a 32-bit one)\n", w0, h0);
            goto out;
        }
        ModifyIDCMP(iw, IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS);
        ClearPointer(iw); /* ogles3 hides the pointer on its screen: dragging needs it */
    } else {
        win = OpenWindowTags(NULL, WA_Title, (ULONG) "Juggler RT", WA_InnerWidth, w0, WA_InnerHeight, h0,
                             WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_RMBTrap, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
        if (!win) {
            say("cannot open a window\n");
            goto out;
        }
        ctx = aglCreateContextTags2(&err, OGLES3_CCT_WINDOW, (ULONG)win, OGLES3_CCT_DEPTH, 0, OGLES3_CCT_STENCIL, 0,
                                    OGLES3_CCT_VSYNC, vsync, OGLES3_CCT_GET_WIDTH, (ULONG)&width,
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
    say("Juggler RT on %s (%s), %lux%lu\n", (const char *)glGetString(GL_RENDERER),
        (const char *)glGetString(GL_VERSION), (unsigned long)width, (unsigned long)height);
    if (!jgl_init(&J))
        goto out;
    J.timing = timing;
    if ((p_text = jgl_link(jgl_compile(GL_VERTEX_SHADER, text_vert), jgl_compile(GL_FRAGMENT_SHADER, text_frag)))) {
        x_screen = glGetUniformLocation(p_text, "u_screen");
        x_col = glGetUniformLocation(p_text, "u_col");
        text_pos = glGetAttribLocation(p_text, "a_pos");
        text_uv = glGetAttribLocation(p_text, "a_uv");
        glUseProgram(p_text);
        glUniform1i(glGetUniformLocation(p_text, "u_font"), 0);
        font = make_font();
    }
    if (getenv("JUGGLER_DBG")) /* measuring: 1 no shadows, 2 no reflections, 4 no shading */
        J.dbg = atoi(getenv("JUGGLER_DBG"));
    if (!load_scene(scene, file, &sc) && (scene = 0, !load_scene(0, NULL, &sc)))
        goto out;
    jgl_scene(&J, &sc);
    orbit_init(&orb, &J.sc, J.centre);
    say("%s: %d spheres in %d objects; %s, %s\n", scene < 0 ? file : scene_name[scene], sc.nsph, sc.nobj,
        modern ? "2026" : "1987", es2 ? "the ES 2.0 way" : "OpenGL ES 3.1");

    start = last = last_title = stat_t = now_s();
    for (f = 0; (!max_frames || f < max_frames) && !quit; f++) {
        struct IntuiMessage *m;
        struct jframe fr;
        double nw = now_s();
        int pw, ph, vx, vy, vw, vh;
        float gain, aspect;
        GLuint pic;

        if ((iw->Flags & WFLG_BACKDROP) && !(iw->Flags & WFLG_WINDOWACTIVE) && iw->WScreen == IntuitionBase->FirstScreen)
            ActivateWindow(iw); /* full screen: while it is in front, the keys are ours */
        while ((m = (struct IntuiMessage *)GetMsg(iw->UserPort)) != NULL) {
            ULONG cl = m->Class;
            UWORD code = m->Code;
            ReplyMsg((struct Message *)m);
            if (cl == IDCMP_CLOSEWINDOW || (cl == IDCMP_VANILLAKEY && code == 27))
                quit = TRUE;
            else if (cl == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                drag = TRUE;
                drag_x = iw->WScreen->MouseX, drag_y = iw->WScreen->MouseY;
            } else if (cl == IDCMP_MOUSEBUTTONS && code == SELECTUP)
                drag = FALSE;
            else if (cl != IDCMP_VANILLAKEY)
                continue;
            else if (code == ' ')
                paused = !paused, moved = TRUE;
            else if ((code | 0x20) == 'm') {
                modern = !modern, moved = TRUE;
                say("%s\n", modern ? "2026: the light at the window's size; still pictures added up"
                                   : "1987: 320x200, 4-bit levels, HAM6");
            } else if ((code | 0x20) == 'e') {
                es2 = !es2, moved = TRUE;
                say("%s\n", es2 ? "the ES 2.0 way: a fragment shader, every ray against every sphere; the 68k animates"
                                : "OpenGL ES 3.1: compute shaders, tiles' sphere lists; the GPU animates");
            } else if ((code | 0x20) == 'n') {
                int nx = scene < 0 ? 0 : (scene + 1) % N_SCENES;
                if (load_scene(nx, NULL, &sc)) {
                    scene = nx;
                    jgl_scene(&J, &sc);
                    orbit_init(&orb, &J.sc, J.centre);
                    moved = TRUE;
                    say("%s: %d spheres in %d objects\n", scene_name[scene], sc.nsph, sc.nobj);
                }
            } else if ((code | 0x20) == 'o') {
                cam = (cam + 1) % 3, moved = TRUE;
                if (cam == 2) /* round from where the swing is */
                    orb.az += orb.swing, orb.swing = 0.0f;
            } else if ((code | 0x20) == 'r') {
                orbit_init(&orb, &J.sc, J.centre);
                cam = 1, swing_t = 0.0f, moved = TRUE;
            } else if ((code | 0x20) == 'h')
                help = !help; else if ((code == '+' || code == '=') && scale < 100)
                scale += 10, moved = TRUE, say("2026 at %d%% of the window\n", scale);
            else if (code == '-' && scale > 20)
                scale -= 10, moved = TRUE, say("2026 at %d%% of the window\n", scale);
        }
        if (drag) { /* the camera turned as the pointer moved, a third of a degree a pixel */
            WORD mx = iw->WScreen->MouseX, my = iw->WScreen->MouseY;
            if (mx != drag_x || my != drag_y) {
                orb.az -= (mx - drag_x) * 0.35f;
                orb.alt -= (my - drag_y) * 0.35f;
                if (orb.alt < -70.0f)
                    orb.alt = -70.0f;
                if (orb.alt > 30.0f)
                    orb.alt = 30.0f;
                drag_x = mx, drag_y = my;
                moved = TRUE;
            }
        }
        if (!paused) {
            float dt = (float)(nw - last);
            if (dt > 0.1f)
                dt = 0.1f;
            show_t = j_mod(show_t + dt, 6.0f * J_H);
            if (cam == 1) { /* 30 degrees each way, once in 24 s */
                swing_t = j_mod(swing_t + dt, 24.0f);
                orb.swing = 30.0f * (float)sin(2.0f * J_PI * swing_t / 24.0f);
            } else if (cam == 2)
                orb.az += 12.0f * dt;
        }
        last = nw;
        if ((J.sc.juggler || cam) && !paused)
            moved = TRUE;

        /* the picture's size: 1987 the original's 320x200 shown 4:3; 2026 the
         * window's (times SCALE), its view plane as wide as the window */
        if (modern) {
            pw = (int)width * scale / 100, ph = (int)height * scale / 100;
            aspect = (float)width / height;
            vx = vy = 0, vw = width, vh = height;
        } else {
            pw = 320, ph = 200, aspect = 4.0f / 3.0f;
            vw = width, vh = width * 3 / 4;
            if (vh > (int)height)
                vh = height, vw = height * 4 / 3;
            vx = ((int)width - vw) / 2, vy = ((int)height - vh) / 2;
        }
        if (!jgl_size(&J, pw, ph))
            break;
        if (moved)
            still = 0;
        moved = FALSE;
        memset(&fr, 0, sizeof(fr));
        orbit_camera(&orb, &J.sc, aspect, &fr.cam);
        fr.time = show_t;
        fr.levels = !modern;
        fr.es2 = es2;
        fr.bounces = 4;
        fr.blend = 1.0f;
        if (!modern)
            fr.jitter[0] = -0.5f, fr.jitter[1] = 0.5f; /* rt1.c's rays: x = i, y = the row from the top */
        else if (still > 0 && !es2) { /* nothing moved: one more picture to add up */
            float u = halton(still, 2), v = halton(still, 3), s, a;
            fr.jitter[0] = u - 0.5f, fr.jitter[1] = v - 0.5f;
            fr.blend = 1.0f / (still + 1);
            /* a place on the lamp's disc as the scene sees it (uniform in area) */
            s = (float)sqrt(halton(still, 5)) * J.sc.lamp[3], a = 2.0f * J_PI * halton(still, 7);
            fr.lampoff[0] = s * (float)cos(a), fr.lampoff[1] = s * (float)sin(a);
            fr.lampoff[2] = 0.5f * s * (float)sin(3.0f * a);
        }
        if (modern && !es2 && still >= 64) { /* added up: shown again, not traced */
            pic = J.hdr[J.cur];
            gain = 16.0f / 15.0f;
        } else {
            if (modern && !es2)
                still++;
            pic = jgl_frame(&J, &fr, &gain);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        if (!modern) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glViewport(vx, vy, vw, vh);
        jgl_show(&J, pic, gain, es2);

        /* the keys' text (key H), its fps every half second */
        stat_frames++;
        if (nw - stat_t >= 0.5) {
            shown_fps = (float)(stat_frames / (nw - stat_t));
            stat_frames = 0;
            stat_t = nw;
        }
        if (help && p_text) {
            static char line[TEXT_MAX];
            float tsc = height >= 600 ? 2.0f : 1.0f, y = height - 12.0f * tsc;
            glViewport(0, 0, width, height);
            glUseProgram(p_text);
            glUniform2f(x_screen, 2.0f / width, 2.0f / height);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, font);
            glEnableVertexAttribArray(text_pos);
            glEnableVertexAttribArray(text_uv);
            text_line(x_col, 1.0f, 0.35f, 0.3f, es2 ? "JUGGLER - REALTIME RAYTRACING OF ERIC GRAHAM'S 1987 DEMO - OPENGL ES 2.0"
                          : "JUGGLER - REALTIME RAYTRACING OF ERIC GRAHAM'S 1987 DEMO - OPENGL ES 3.1",
                      8.0f * tsc, y, tsc);
            sprintf(line, "%s  %s %dX%d  %d SPHERES", scene < 0 ? "SCENE FILE" : scene_upper[scene],
                    modern ? "2026" : "1987 HAM", pw, ph, sc.nsph);
            text_line(x_col, 0.9f, 0.92f, 1.0f, line, 8.0f * tsc, y - 10.0f * tsc, tsc);
            sprintf(line, "%.1f FPS    CAMERA %s", shown_fps, cam_name[cam]);
            text_line(x_col, 0.9f, 0.92f, 1.0f, line, 8.0f * tsc, y - 20.0f * tsc, tsc);
            text_line(x_col, 0.85f, 0.85f, 0.95f,
                      "M: 1987/2026  E: ES 2/3  N: SCENE  O: CAMERA  R: RESET  +/-: SIZE  H: TEXT  SPACE: PAUSE",
                      8.0f * tsc, 6.0f * tsc, tsc);
            glDisableVertexAttribArray(text_uv);
            glDisableVertexAttribArray(text_pos);
            glDisable(GL_BLEND);
        }
        aglSwapBuffers();

        title_frames++;
        if (nw - last_title >= 1.0) {
            if (win) {
                sprintf(title, "Juggler RT: %s %dx%d, %s, %.1f fps", modern ? "2026" : "1987", pw, ph,
                        es2 ? "ES 2.0" : "ES 3.1", title_frames / (nw - last_title));
                SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
            }
            title_frames = 0;
            last_title = nw;
        }
    }
    if (max_frames || timing) {
        double a = now_s() - start;
        say("%lu frames in %.2f s: %.1f fps\n", f, a, f / a);
        if (timing && f) {
            say("a frame (ms):");
            for (i = 0; i < JT_N; i++)
                say(" %s %.2f", part_name[i], J.t[i] * 1000.0 / f);
            say("\n");
        }
    }
    rc = 0;

out:
    if (ctx) {
        GLuint progs[6];
        progs[0] = J.p_anim, progs[1] = J.p_trace, progs[2] = J.p_ham, progs[3] = J.p_show, progs[4] = J.p_es2,
        progs[5] = J.p_es2show;
        if (p_text)
            glDeleteProgram(p_text);
        if (font)
            glDeleteTextures(1, &font);
        for (i = 0; i < 6; i++)
            if (progs[i])
                glDeleteProgram(progs[i]);
        jgl_free_size(&J);
        if (J.b_sph) {
            glDeleteBuffers(1, &J.b_sph);
            glDeleteBuffers(1, &J.b_grp);
            glDeleteBuffers(1, &J.b_chain);
            glDeleteBuffers(1, &J.vbo);
        }
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
