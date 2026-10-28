/*
 * dEQP-GLES31 draw_indirect's restarts (docs/GLES3_PLAN.md stage D item 3,
 * "Parked"): the cases' draws over and over, as glsDrawTest.cpp makes them,
 * to make the restart come on demand and narrow it. A 256x256 window
 * context (dEQP's platform, tcuAmigaPlatform.cpp), es31di's program; each
 * iteration: two vertex buffers of random floats in [-127, 127] (a_0 four
 * a vertex read as a vec2, a_1 two), bound in a vertex array object,
 * cleared, 1, 5 or 25 triangles from vertex 0 or 1 drawn through a draw
 * indirect buffer made for the draw and deleted after it, the window read
 * back, the buffers deleted. A line per iteration (stdout: the results
 * file, as dEQP's log), flushed.
 *   usage: m8_indirect [N] [direct] [noread] [nodelete] [one] [first0]
 *   N         iterations (500)
 *   direct    glDrawArrays instead of glDrawArraysIndirect
 *   noread    no glReadPixels
 *   nodelete  the indirect buffer kept (one made at the start)
 *   one       one attribute (a_0), as single_attribute (which passes)
 *   first0    always from vertex 0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ogles3.h>
#include <GLES3/gl31.h>

/* RAM:deqp-progress.txt as dEQP's progress patch writes it ("S case",
 * "E status"; tools/amictl/deqp_watch.py reads it while this runs), shared
 * (MODE_READWRITE: the station reads it meanwhile). */
static BPTR progress;

/* A step before the iterations, as a case started and passed, then 3 s for
 * the watcher to read it (a restart in the step shows which). */
static void stage(const char *what)
{
    BPTR f = Open((STRPTR)"RAM:deqp-progress.txt", MODE_READWRITE);
    char line[64];
    LONG len = snprintf(line, sizeof(line), "S %s\nE Pass\n", what);
    if (f) {
        Seek(f, 0, OFFSET_END);
        Write(f, line, len);
        Close(f);
    }
    Delay(150);
}

static void progress_line(const char *kind, int i)
{
    char line[48];
    LONG len;
    if (!progress)
        return;
    len = kind[0] == 'S' ? snprintf(line, sizeof(line), "S iteration %d\n", i)
                         : snprintf(line, sizeof(line), "%s\n", kind);
    Write(progress, line, len);
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

static GLuint program(const char *name)
{
    char fn[32], log[256];
    char *src;
    GLuint p = glCreateProgram(), s;
    GLint ok = 0, k;
    for (k = 0; k < 2; k++) {
        snprintf(fn, sizeof(fn), "%s.%s", name, k ? "frag" : "vert");
        if (!(src = read_text(fn))) {
            printf("no shaders/%s\n", fn);
            return 0;
        }
        s = glCreateShader(k ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource(s, 1, (const GLchar *const *)&src, 0);
        glCompileShader(s);
        glAttachShader(p, s);
        glDeleteShader(s);
        free(src);
    }
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), 0, log);
        printf("%s: link failed: %s\n", name, log);
        return 0;
    }
    return p;
}

static unsigned long seed = 12345;

static float random_float(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (float)((long)((seed >> 8) % 25401UL) - 12700L) / 100.0f; /* -127 to 127 */
}

int main(int argc, char **argv)
{
    static const int counts[3] = { 1, 5, 25 };
    GLuint p, vao, vb[2], ib = 0;
    GLubyte *pixels;
    ULONG err = 0;
    void *ctx;
    int n = 500, direct = 0, noread = 0, nodelete = 0, one = 0, first0 = 0, i, a, k;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "direct")) direct = 1;
        else if (!strcmp(argv[i], "noread")) noread = 1;
        else if (!strcmp(argv[i], "nodelete")) nodelete = 1;
        else if (!strcmp(argv[i], "one")) one = 1;
        else if (!strcmp(argv[i], "first0")) first0 = 1;
        else n = atoi(argv[i]);
    }
    stage("start");
    if (!OGLES3Open()) {
        printf("needs %s\n", OGLES3NAME);
        return 20;
    }
    stage("library opened");
    ctx = aglCreateContextTags2(&err, OGLES3_CCT_PISTORM_WIDTH, 256UL, OGLES3_CCT_PISTORM_HEIGHT, 256UL,
                                OGLES3_CCT_DEPTH, 24UL, OGLES3_CCT_STENCIL, 8UL, OGLES3_CCT_VSYNC, (ULONG)FALSE,
                                OGLES3_CCT_PISTORM_ES_VERSION, 31UL, TAG_DONE);
    if (!ctx) {
        printf("FAIL: no ES 3.1 window context (error %lu)\n", (unsigned long)err);
        OGLES3Close();
        return 20;
    }
    stage("context made");
    aglMakeCurrent(ctx);
    stage("context current");
    p = program("es31di");
    stage("program linked");
    if (!p || !(pixels = malloc(256 * 256 * 4))) {
        aglDestroyContext(ctx);
        OGLES3Close();
        return 20;
    }
    printf("m8_indirect: %d iterations,%s%s%s%s%s\n", n, direct ? " direct" : " indirect",
           noread ? ", no read" : "", nodelete ? ", indirect buffer kept" : "", one ? ", one attribute" : "",
           first0 ? ", from vertex 0" : "");
    fflush(stdout);
    if ((progress = Open((STRPTR)"RAM:deqp-progress.txt", MODE_READWRITE)) != 0)
        Seek(progress, 0, OFFSET_END);
    glGenVertexArrays(1, &vao);
    if (nodelete)
        glGenBuffers(1, &ib);
    for (i = 0; i < n; i++) {
        int count = counts[i % 3] * 3, first = first0 ? 0 : (i / 3) % 2, verts = first + count;
        float *data[2];
        GLuint cmd[4];
        data[0] = malloc((size_t)verts * 4 * sizeof(float));
        data[1] = malloc((size_t)verts * 2 * sizeof(float));
        if (!data[0] || !data[1]) {
            printf("FAIL: out of memory at %d\n", i);
            break;
        }
        for (k = 0; k < verts * 4; k++)
            data[0][k] = random_float();
        for (k = 0; k < verts * 2; k++)
            data[1][k] = random_float();
        progress_line("S", i);
        glViewport(0, 0, 256, 256);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "u_coordScale"), 1.0f / (127.0f * 1.1f));
        glUniform1f(glGetUniformLocation(p, "u_colorScale"), 1.0f / (127.0f * 1.1f));
        glBindVertexArray(vao);
        glGenBuffers(2, vb);
        for (a = 0; a < (one ? 1 : 2); a++) {
            GLint loc = glGetAttribLocation(p, a ? "a_1" : "a_0");
            glBindBuffer(GL_ARRAY_BUFFER, vb[a]);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)verts * (a ? 2 : 4) * sizeof(float), data[a], GL_STATIC_DRAW);
            glEnableVertexAttribArray((GLuint)loc);
            glVertexAttribPointer((GLuint)loc, a ? 2 : 4, GL_FLOAT, GL_FALSE, 0, 0);
        }
        if (one)
            glDisableVertexAttribArray((GLuint)glGetAttribLocation(p, "a_1"));
        cmd[0] = (GLuint)count;
        cmd[1] = 1;
        cmd[2] = (GLuint)first;
        cmd[3] = 0;
        if (direct) {
            glDrawArrays(GL_TRIANGLES, first, count);
        } else {
            if (!nodelete)
                glGenBuffers(1, &ib);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ib);
            glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), cmd, GL_STATIC_DRAW);
            glDrawArraysIndirect(GL_TRIANGLES, 0);
            if (!nodelete)
                glDeleteBuffers(1, &ib);
        }
        if (!noread)
            glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindVertexArray(0);
        glDeleteBuffers(2, vb);
        free(data[0]);
        free(data[1]);
        printf("%d: %d triangles from %d, error 0x%x\n", i, count / 3, first, (unsigned)glGetError());
        fflush(stdout);
        progress_line("E Pass", i);
    }
    if (progress)
        Close(progress);
    glDeleteVertexArrays(1, &vao);
    if (nodelete)
        glDeleteBuffers(1, &ib);
    glDeleteProgram(p);
    free(pixels);
    aglDestroyContext(ctx);
    OGLES3Close();
    printf("PASSED: %d iterations\n", i);
    return 0;
}
