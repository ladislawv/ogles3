/*
 * Shared by v3dglslc.c (m68k or x86-64, Mesa entry points called directly) and
 * s1ref.c (x86-64, EGL + libGLESv2 under Mesa's drm-shim): the same GLES2
 * calls on the same shader files, so the V3D driver sees the same work.
 *
 * Everything is printed to stderr, where the V3D driver prints its QPU code
 * (V3D_DEBUG=precompile,qpu), so the two land in one ordered stream; AmigaOS
 * merges stderr into stdout anyway.
 *
 * The includer defines the gl* names (directly or via the Khronos headers).
 * With V3DC_WRITER it also provides program_path() and write_program(),
 * called after each successful link that has somewhere to write to (-o, or
 * a server request's path).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef V3DC_WRITER
static const char *out_dir; /* -o: write .v3dp files here */
#endif

/* The link state of the next link (V3DGLSL_LINK_BOUND): glBindAttribLocation's
 * bindings, "<index> <name>\n" lines; then, with transform feedback
 * varyings, the line "XFBV" and the text the program's hash takes for them
 * (v3dp/FORMAT.md, format 2): the buffer mode as four hex digits and "\n",
 * then each varying's name and "\n" in the order given. NULL: none. */
static const char *link_bindings;

#define XFBV_MARK "XFBV\n"

/* The transform feedback part of link state `b` (after its "XFBV" line), or NULL. */
static const char *xfbv_part(const char *b)
{
    const char *at = b;
    while (at && *at) {
        if (!strncmp(at, XFBV_MARK, sizeof(XFBV_MARK) - 1))
            return at + sizeof(XFBV_MARK) - 1;
        at = strchr(at, '\n');
        if (at)
            at++;
    }
    return NULL;
}

static void apply_bindings(GLuint p, const char *b)
{
    char name[256];
    const char *end = xfbv_part(b);
    if (end)
        end -= sizeof(XFBV_MARK) - 1;
    while (b && *b && b != end) {
        unsigned long index = strtoul(b, (char **)&b, 10);
        size_t n = 0;
        while (*b == ' ')
            b++;
        while (*b && *b != '\n' && n + 1 < sizeof(name))
            name[n++] = *b++;
        name[n] = 0;
        while (*b && *b != '\n')
            b++;
        if (*b == '\n')
            b++;
        if (n)
            glBindAttribLocation(p, (GLuint)index, name);
    }
}

#ifdef V3DC_WRITER
/* glTransformFeedbackVaryings from link state `b`'s "XFBV" part. */
static void apply_xfb(GLuint p, const char *b)
{
    const char *x = xfbv_part(b), *s;
    char *copy, *at, **names;
    unsigned long mode;
    GLsizei n = 0;
    if (!x)
        return;
    mode = strtoul(x, (char **)&x, 16);
    if (*x == '\n')
        x++;
    for (s = x; *s; s++)
        n += *s == '\n';
    copy = strdup(x);
    names = malloc(sizeof(*names) * (n ? n : 1));
    if (copy && names) {
        GLsizei k = 0;
        for (at = copy; k < n && *at; k++) {
            names[k] = at;
            at = strchr(at, '\n');
            *at++ = 0;
        }
        glTransformFeedbackVaryings(p, k, (const GLchar *const *)names, (GLenum)mode);
    }
    free(names);
    free(copy);
}
#endif

static char *load_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (buf) {
        n = (long)fread(buf, 1, n, f);
        buf[n] = 0;
    }
    fclose(f);
    return buf;
}

/* Appends a and b to the caller's log (NULL: no log), truncating. */
static void log_add(char *log, size_t size, const char *a, const char *b)
{
    size_t n;
    if (!log || !size)
        return;
    n = strlen(log);
    snprintf(log + n, size - n, "%s%s", a, b ? b : "");
}

/* Compiles one shader from memory; `label` names it in the messages. */
static GLuint compile_src(GLenum type, const char *label, const char *src, int *ok_out, char *log,
                          size_t log_size)
{
    const GLchar *srcs[1];
    GLuint s;
    GLint ok = 0;
    char info[2048];

    srcs[0] = src;
    s = glCreateShader(type);
    glShaderSource(s, 1, srcs, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    info[0] = 0;
    glGetShaderInfoLog(s, sizeof(info), NULL, info);
    fprintf(stderr, "v3dglslc: compile %s: %s%s%s\n", label, ok ? "ok" : "FAILED",
            info[0] ? "\n" : "", info);
    if (!ok || info[0]) {
        log_add(log, log_size, type == GL_VERTEX_SHADER ? "vertex shader: " : "fragment shader: ",
                ok ? "" : "compile failed\n");
        log_add(log, log_size, info, NULL);
    }
    *ok_out = ok;
    return s;
}

/* Compile and link one vertex+fragment pair from memory, delete the GL
 * objects again, and with V3DC_WRITER and a `target` write the program
 * there. With V3D_DEBUG=precompile the driver compiles its default variants
 * at link time and prints them. Returns 1 if linked (and written). */
static int link_pair(const char *vs_label, const char *fs_label, const char *vs_src,
                     const char *fs_src, const char *target, char *log, size_t log_size)
{
    GLuint p, vs, fs;
    GLint ok = 0;
    int vs_ok, fs_ok;
    char info[2048];

    fprintf(stderr, "v3dglslc: ==== program %s + %s\n", vs_label, fs_label);
    vs = compile_src(GL_VERTEX_SHADER, vs_label, vs_src, &vs_ok, log, log_size);
    fs = compile_src(GL_FRAGMENT_SHADER, fs_label, fs_src, &fs_ok, log, log_size);
    if (!vs_ok || !fs_ok) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    apply_bindings(p, link_bindings);
#ifdef V3DC_WRITER
    apply_xfb(p, link_bindings);
#endif
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    info[0] = 0;
    glGetProgramInfoLog(p, sizeof(info), NULL, info);
    fprintf(stderr, "v3dglslc: link: %s%s%s\n", ok ? "ok" : "FAILED", info[0] ? "\n" : "", info);
    if (!ok || info[0]) {
        log_add(log, log_size, ok ? "link: " : "link failed: ", info);
        log_add(log, log_size, "\n", NULL);
    }
#ifdef V3DC_WRITER
    if (ok && target) {
        ok = write_program(p, target, vs_src, fs_src);
        if (!ok)
            log_add(log, log_size, "the program binary could not be written: ", target);
    }
#else
    (void)target;
#endif
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return ok;
}

/* One pair of shader files (the command line). */
static int compile_link(const char *vs_path, const char *fs_path)
{
    char *vs_src = load_text(vs_path), *fs_src = load_text(fs_path);
    const char *target = NULL;
    int ok = 0;
#ifdef V3DC_WRITER
    char path[512];
    if (out_dir) {
        program_path(path, sizeof(path), vs_path, fs_path);
        target = path;
    }
#endif
    if (!vs_src || !fs_src)
        fprintf(stderr, "v3dglslc: cannot read %s\n", !vs_src ? vs_path : fs_path);
    else
        ok = link_pair(vs_path, fs_path, vs_src, fs_src, target, NULL, 0);
    free(vs_src);
    free(fs_src);
    return ok;
}

static int run_pairs(int argc, char **argv, int first)
{
    int i, failed = 0;
    for (i = first; i + 1 < argc; i += 2)
        if (!compile_link(argv[i], argv[i + 1]))
            failed++;
    fprintf(stderr, "v3dglslc: done, %d program(s) failed\n", failed);
    return failed ? 10 : 0;
}
