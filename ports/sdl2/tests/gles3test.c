/*
 * gles3test: OpenGL ES 3 contexts through SDL2 (docs/GLES3_PLAN.md stage C
 * item 6). SDL asked for ES 3.0 gets an ogles2 context of that version,
 * whose GLSL ES 3.00 program draws, or, from a library without ES 3.0
 * contexts (a release before 2.1, or one built without them), SDL's error;
 * ES 3.2 is refused, and ES 2.0 is as before. Prints PASS/FAIL lines;
 * returns 0 when all passed, 10 otherwise.
 *   gles3test [REFUSED]   REFUSED: ES 3.0 must be refused (a release build)
 */
#include <stdio.h>
#include <string.h>
#include "SDL.h"

#define GL_VERSION          0x1F02
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES        0x0004
#define GL_FLOAT            0x1406
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_LINK_STATUS      0x8B82

typedef const unsigned char *(*GetString)(unsigned int);
typedef void (*ClearColor)(float, float, float, float);
typedef void (*Clear)(unsigned int);
typedef void (*ReadPixels)(int, int, int, int, unsigned int, unsigned int, void *);
typedef unsigned int (*CreateShader)(unsigned int);
typedef void (*ShaderSource)(unsigned int, int, const char *const *, const int *);
typedef void (*CompileShader)(unsigned int);
typedef unsigned int (*CreateProgram)(void);
typedef void (*AttachShader)(unsigned int, unsigned int);
typedef void (*BindAttribLocation)(unsigned int, unsigned int, const char *);
typedef void (*LinkProgram)(unsigned int);
typedef void (*GetProgramiv)(unsigned int, unsigned int, int *);
typedef void (*UseProgram)(unsigned int);
typedef void (*VertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void *);
typedef void (*EnableVertexAttribArray)(unsigned int);
typedef void (*DrawArrays)(unsigned int, int, int);

static int failures;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

/* A context of ES major.minor on the window; NULL with SDL's error printed. */
static SDL_GLContext context(SDL_Window *w, int major, int minor)
{
    SDL_GLContext c;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    c = SDL_GL_CreateContext(w);
    if (!c)
        printf("     ES %d.%d: %s\n", major, minor, SDL_GetError());
    return c;
}

/* A GLSL ES 3.00 triangle over the window's lower left half: its pixel
 * (4, 4) green after a red clear. */
static int draws(void)
{
    static const char *vs = "#version 300 es\nin vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs = "#version 300 es\nprecision mediump float;\nout vec4 c;\n"
                            "void main() { c = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    static const float tri[6] = { -1, -1, 1, -1, -1, 1 };
    unsigned char px[4] = { 0, 0, 0, 0 };
    unsigned int p, s;
    int ok = 0;
    p = ((CreateProgram)SDL_GL_GetProcAddress("glCreateProgram"))();
    s = ((CreateShader)SDL_GL_GetProcAddress("glCreateShader"))(GL_VERTEX_SHADER);
    ((ShaderSource)SDL_GL_GetProcAddress("glShaderSource"))(s, 1, &vs, 0);
    ((CompileShader)SDL_GL_GetProcAddress("glCompileShader"))(s);
    ((AttachShader)SDL_GL_GetProcAddress("glAttachShader"))(p, s);
    s = ((CreateShader)SDL_GL_GetProcAddress("glCreateShader"))(GL_FRAGMENT_SHADER);
    ((ShaderSource)SDL_GL_GetProcAddress("glShaderSource"))(s, 1, &fs, 0);
    ((CompileShader)SDL_GL_GetProcAddress("glCompileShader"))(s);
    ((AttachShader)SDL_GL_GetProcAddress("glAttachShader"))(p, s);
    ((BindAttribLocation)SDL_GL_GetProcAddress("glBindAttribLocation"))(p, 0, "pos");
    ((LinkProgram)SDL_GL_GetProcAddress("glLinkProgram"))(p);
    ((GetProgramiv)SDL_GL_GetProcAddress("glGetProgramiv"))(p, GL_LINK_STATUS, &ok);
    if (!ok)
        return 0;
    ((UseProgram)SDL_GL_GetProcAddress("glUseProgram"))(p);
    ((ClearColor)SDL_GL_GetProcAddress("glClearColor"))(1, 0, 0, 1);
    ((Clear)SDL_GL_GetProcAddress("glClear"))(GL_COLOR_BUFFER_BIT);
    ((VertexAttribPointer)SDL_GL_GetProcAddress("glVertexAttribPointer"))(0, 2, GL_FLOAT, 0, 0, tri);
    ((EnableVertexAttribArray)SDL_GL_GetProcAddress("glEnableVertexAttribArray"))(0);
    ((DrawArrays)SDL_GL_GetProcAddress("glDrawArrays"))(GL_TRIANGLES, 0, 3);
    ((ReadPixels)SDL_GL_GetProcAddress("glReadPixels"))(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px[0] == 0 && px[1] == 255 && px[2] == 0;
}

int main(int argc, char **argv)
{
    int refused = argc > 1 && !strcmp(argv[1], "REFUSED");
    SDL_Window *w;
    SDL_GLContext c;
    const char *v;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 10;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    if (!(w = SDL_CreateWindow("gles3test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
                               SDL_WINDOW_OPENGL))) {
        printf("FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 10;
    }
    c = context(w, 3, 0);
    if (refused) {
        check(!c, "ES 3.0 refused by this library, with SDL's error");
    } else {
        v = c ? (const char *)((GetString)SDL_GL_GetProcAddress("glGetString"))(GL_VERSION) : 0;
        if (v)
            printf("     GL_VERSION %s\n", v);
        check(v && !strncmp(v, "OpenGL ES 3.0", 13), "ES 3.0: a context of that version");
        check(c && draws(), "ES 3.0: a GLSL ES 3.00 program draws");
        SDL_GL_SwapWindow(w);
    }
    if (c)
        SDL_GL_DeleteContext(c);
    c = context(w, 3, 2);
    check(!c, "ES 3.2 refused, with SDL's error");
    if (c)
        SDL_GL_DeleteContext(c);
    c = context(w, 2, 0);
    v = c ? (const char *)((GetString)SDL_GL_GetProcAddress("glGetString"))(GL_VERSION) : 0;
    check(v && !strncmp(v, "OpenGL ES 2.0", 13), "ES 2.0 as before");
    if (c)
        SDL_GL_DeleteContext(c);
    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 10 : 0;
}
