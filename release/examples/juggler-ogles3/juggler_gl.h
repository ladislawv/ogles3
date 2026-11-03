/*
 * Juggler RT's renderer, portable OpenGL ES 3.1 C: the Amiga program
 * (ogles3.library) and the PC check (Mesa) share it. The includer gives
 * the GL headers, juggler_scene.h, the shaders' strings, and
 *   J_LOG(fmt, ...)  a line of text
 *   J_NOW()          seconds, a double (TIMING)
 *
 * A frame the ES 3.1 way: animate.comp (the juggler only), trace.comp into
 * an image, ham.comp (1987), then the picture drawn over the screen. The
 * ES 2.0 way: the 68k animates and fills uniform arrays, es2trace.frag
 * draws the picture into a texture (every ray against every sphere),
 * es2show.frag onto the screen.
 *
 * MIT License.
 */
#ifndef JUGGLER_GL_H
#define JUGGLER_GL_H

enum { JT_ANIM, JT_TRACE, JT_HAM, JT_SHOW, JT_N };

struct jgl {
    GLuint p_anim, p_shadow, p_trace, p_ham, p_show, p_es2, p_es2show;
    GLint a_rest, a_time, a_ns, a_ng;
    GLint w_lamp, w_lampoff, w_centre, w_ns;
    GLint t_eye, t_fwd, t_right, t_up, t_size, t_ns, t_ng, t_levels, t_bounces, t_lamp, t_lampcol, t_lampoff;
    GLint t_dbg, t_g0, t_g1, t_illum, t_zen, t_hor, t_jitter, t_blend;
    GLint h_size, s_gain;
    GLint e_sp, e_ns, e_levels, e_jitter, e_eye, e_fwd, e_right, e_up, e_size, e_lamp, e_lampcol, e_g0, e_g1, e_illum, e_zen,
        e_hor;
    GLuint b_sph, b_grp, b_chain, b_sgrid, b_cells, vbo, near; /* near: ham.comp's nearest registers */
    GLuint hdr[2], lv, ham, es2tex, es2fbo, lvfbo;
    int w, h, cur; /* the pictures' size; hdr[cur] the newest */
    struct jscene sc;
    struct jsphere sp[J_MAX_SPHERES];
    struct jgroup gr[J_MAX_OBJECTS];
    struct jchain ch[J_MAX_SPHERES];
    float pts[J_MAX_POINTS][4];
    struct jham pal;
    int pal_ready;
    float centre[3]; /* the scene's middle, for the camera's orbit */
    double t[JT_N];
    int timing, dbg;
};

static GLuint jgl_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        J_LOG("shader: %s\n", log);
    }
    return s;
}

static GLuint jgl_link(GLuint a, GLuint b)
{
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, a);
    if (b)
        glAttachShader(p, b);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(a);
    if (b)
        glDeleteShader(b);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        J_LOG("link: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint jgl_compute(const char *src)
{
    return jgl_link(jgl_compile(GL_COMPUTE_SHADER, src), 0);
}

static void jgl_lap(struct jgl *j, int part, double *a)
{
    double n;
    if (!j->timing)
        return;
    glFinish();
    n = J_NOW();
    j->t[part] += n - *a;
    *a = n;
}

/* The programs and buffers; 0 if a program is missing. */
static int jgl_init(struct jgl *j)
{
    static const GLfloat tri[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    GLuint p;
    memset(j, 0, sizeof(*j));
    if (!(j->p_anim = jgl_compute(animate_comp)) || !(j->p_trace = jgl_compute(trace_comp)) ||
        !(j->p_shadow = jgl_compute(shadow_comp)) ||
        !(j->p_ham = jgl_compute(ham_comp)) ||
        !(j->p_show = jgl_link(jgl_compile(GL_VERTEX_SHADER, screen_vert), jgl_compile(GL_FRAGMENT_SHADER, show_frag))) ||
        !(j->p_es2 = jgl_link(jgl_compile(GL_VERTEX_SHADER, es2_vert), jgl_compile(GL_FRAGMENT_SHADER, es2trace_frag))) ||
        !(j->p_es2show = jgl_link(jgl_compile(GL_VERTEX_SHADER, es2_vert), jgl_compile(GL_FRAGMENT_SHADER, es2show_frag))))
        return 0;
    p = j->p_anim;
    j->a_rest = glGetUniformLocation(p, "u_rest");
    j->a_time = glGetUniformLocation(p, "u_time");
    j->a_ns = glGetUniformLocation(p, "u_ns");
    j->a_ng = glGetUniformLocation(p, "u_ng");
    p = j->p_shadow;
    j->w_lamp = glGetUniformLocation(p, "u_lamp");
    j->w_lampoff = glGetUniformLocation(p, "u_lampoff");
    j->w_centre = glGetUniformLocation(p, "u_centre");
    j->w_ns = glGetUniformLocation(p, "u_ns");
    p = j->p_trace;
    j->t_eye = glGetUniformLocation(p, "u_eye");
    j->t_fwd = glGetUniformLocation(p, "u_fwd");
    j->t_right = glGetUniformLocation(p, "u_right");
    j->t_up = glGetUniformLocation(p, "u_up");
    j->t_size = glGetUniformLocation(p, "u_size");
    j->t_ns = glGetUniformLocation(p, "u_ns");
    j->t_ng = glGetUniformLocation(p, "u_ng");
    j->t_levels = glGetUniformLocation(p, "u_levels");
    j->t_bounces = glGetUniformLocation(p, "u_bounces");
    j->t_lamp = glGetUniformLocation(p, "u_lamp");
    j->t_lampcol = glGetUniformLocation(p, "u_lampcol");
    j->t_lampoff = glGetUniformLocation(p, "u_lampoff");
    j->t_g0 = glGetUniformLocation(p, "u_g0");
    j->t_g1 = glGetUniformLocation(p, "u_g1");
    j->t_illum = glGetUniformLocation(p, "u_illum");
    j->t_zen = glGetUniformLocation(p, "u_zen");
    j->t_hor = glGetUniformLocation(p, "u_hor");
    j->t_jitter = glGetUniformLocation(p, "u_jitter");
    j->t_blend = glGetUniformLocation(p, "u_blend");
    j->t_dbg = glGetUniformLocation(p, "u_dbg");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_prev"), 1);
    p = j->p_ham;
    j->h_size = glGetUniformLocation(p, "u_size");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_lvtex"), 2);
    glUniform1i(glGetUniformLocation(p, "u_near"), 3);
    j->s_gain = glGetUniformLocation(j->p_show, "u_gain");
    glUseProgram(j->p_show);
    glUniform1i(glGetUniformLocation(j->p_show, "u_img"), 0);
    glUseProgram(j->p_es2show);
    glUniform1i(glGetUniformLocation(j->p_es2show, "u_img"), 0);
    p = j->p_es2;
    j->e_sp = glGetUniformLocation(p, "u_sp");
    j->e_ns = glGetUniformLocation(p, "u_ns");
    j->e_levels = glGetUniformLocation(p, "u_levels");
    j->e_jitter = glGetUniformLocation(p, "u_jitter");
    j->e_eye = glGetUniformLocation(p, "u_eye");
    j->e_fwd = glGetUniformLocation(p, "u_fwd");
    j->e_right = glGetUniformLocation(p, "u_right");
    j->e_up = glGetUniformLocation(p, "u_up");
    j->e_size = glGetUniformLocation(p, "u_size");
    j->e_lamp = glGetUniformLocation(p, "u_lamp");
    j->e_lampcol = glGetUniformLocation(p, "u_lampcol");
    j->e_g0 = glGetUniformLocation(p, "u_g0");
    j->e_g1 = glGetUniformLocation(p, "u_g1");
    j->e_illum = glGetUniformLocation(p, "u_illum");
    j->e_zen = glGetUniformLocation(p, "u_zen");
    j->e_hor = glGetUniformLocation(p, "u_hor");
    glUseProgram(0);

    glGenBuffers(1, &j->b_sph);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_sph);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(j->sp), NULL, GL_DYNAMIC_COPY);
    glGenBuffers(1, &j->b_grp);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_grp);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(j->gr), NULL, GL_DYNAMIC_COPY);
    glGenBuffers(1, &j->b_chain);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_chain);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(j->ch), NULL, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, j->b_sph);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, j->b_grp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, j->b_chain);
    /* shadow.comp's grid: 5 vec4 and 16 x 16 counts; 48 spheres a cell */
    glGenBuffers(1, &j->b_sgrid);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_sgrid);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 5 * 16 + 16 * 16 * 4, NULL, GL_DYNAMIC_COPY);
    glGenBuffers(1, &j->b_cells);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_cells);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 16 * 16 * 48 * 16, NULL, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, j->b_sgrid);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, j->b_cells);
    glGenBuffers(1, &j->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, j->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 1;
}

/* A scene to draw: its spheres at rest, the lamp exposed by them, the
 * buffers filled; the HAM palette made again at the next 1987 picture. */
static void jgl_scene(struct jgl *j, const struct jscene *sc)
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    int i, k;
    j->sc = *sc;
    memcpy(j->pts, sc->pt, sizeof(j->pts));
    j_expand(&j->sc, (const float(*)[4])j->pts, j->sp, j->gr);
    j_expose(&j->sc, j->sp);
    j_chains(&j->sc, j->ch);
    for (i = 0; i < sc->nsph; i++)
        for (k = 0; k < 3; k++) {
            if (j->sp[i].p[k] - j->sp[i].p[3] < lo[k])
                lo[k] = j->sp[i].p[k] - j->sp[i].p[3];
            if (j->sp[i].p[k] + j->sp[i].p[3] > hi[k])
                hi[k] = j->sp[i].p[k] + j->sp[i].p[3];
        }
    for (k = 0; k < 3; k++)
        j->centre[k] = 0.5f * (lo[k] + hi[k]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_sph);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sc->nsph * sizeof(struct jsphere), j->sp);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_grp);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sc->nobj * sizeof(struct jgroup), j->gr);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, j->b_chain);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sc->nsph * sizeof(struct jchain), j->ch);
    j->pal_ready = 0;
}

static GLuint jgl_tex(int w, int h, GLenum fmt, GLenum filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void jgl_free_size(struct jgl *j)
{
    GLuint t[5];
    t[0] = j->hdr[0], t[1] = j->hdr[1], t[2] = j->lv, t[3] = j->ham, t[4] = j->es2tex;
    if (j->hdr[0])
        glDeleteTextures(5, t);
    if (j->es2fbo) {
        glDeleteFramebuffers(1, &j->es2fbo);
        glDeleteFramebuffers(1, &j->lvfbo);
    }
    j->hdr[0] = j->hdr[1] = j->lv = j->ham = j->es2tex = j->es2fbo = j->lvfbo = 0;
    j->w = j->h = 0;
}

/* The pictures w x h: 0 if a framebuffer is not complete. */
static int jgl_size(struct jgl *j, int w, int h)
{
    static const GLfloat black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int i, ok;
    if (w == j->w && h == j->h)
        return 1;
    jgl_free_size(j);
    j->w = w, j->h = h;
    for (i = 0; i < 2; i++)
        j->hdr[i] = jgl_tex(w, h, GL_RGBA16F, GL_LINEAR);
    j->lv = jgl_tex(w, h, GL_RGBA8, GL_NEAREST);
    j->ham = jgl_tex(w, h, GL_RGBA8, GL_NEAREST);
    j->es2tex = jgl_tex(w, h, GL_RGBA8, GL_NEAREST);
    glGenFramebuffers(1, &j->es2fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, j->es2fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->es2tex, 0);
    ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenFramebuffers(1, &j->lvfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, j->lvfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->lv, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    for (i = 0; ok && i < 2; i++) { /* the first still picture adds to black */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->hdr[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            glClearBufferfv(GL_COLOR, 0, black);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->lv, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok)
        J_LOG("a %dx%d picture's framebuffer is not complete\n", w, h);
    glBindImageTexture(1, j->lv, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, j->ham, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, j->lv);
    glActiveTexture(GL_TEXTURE0);
    j->cur = 0;
    return ok;
}

/* The HAM palette from the levels picture just traced (rt3.c's allocation,
 * rows top first: GL's last row first). */
static void jgl_palette(struct jgl *j)
{
    unsigned char *px = (unsigned char *)malloc((size_t)j->w * j->h * 4);
    int *row = (int *)malloc((size_t)j->w * 6 * sizeof(int)), x, y;
    if (!px || !row) {
        free(px);
        free(row);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, j->lvfbo);
    glReadPixels(0, 0, j->w, j->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    j_ham_init(&j->pal);
    for (y = j->h - 1; y >= 0; y--) {
        for (x = 0; x < j->w; x++) {
            const unsigned char *p = px + ((size_t)y * j->w + x) * 4;
            row[3 * x] = (p[0] + 8) / 17, row[3 * x + 1] = (p[1] + 8) / 17, row[3 * x + 2] = (p[2] + 8) / 17;
        }
        j_ham_row(&j->pal, row, j->w, row + 3 * j->w, 1);
    }
    free(px);
    free(row);
    j->pal_ready = 1;
}

/* ham.comp's table: for each 4-bit colour (r * 256 + g * 16 + b, 64 a
 * row) its nearest register (rgb, levels * 17) and the distance (a). */
static void jgl_near(struct jgl *j)
{
    static unsigned char t[4096 * 4];
    int i, k, c[3], d, pen;
    for (i = 0; i < 4096; i++) {
        c[0] = i >> 8, c[1] = (i >> 4) & 15, c[2] = i & 15;
        pen = j_nearest(&j->pal, c, &d, 0);
        for (k = 0; k < 3; k++)
            t[4 * i + k] = (unsigned char)(j->pal.reg[pen][k] * 17);
        t[4 * i + 3] = (unsigned char)d;
    }
    if (!j->near) {
        glGenTextures(1, &j->near);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, j->near);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, j->near);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, t);
    glActiveTexture(GL_TEXTURE0);
}

struct jframe {
    struct jcamera cam;
    float time;        /* the juggling's (0 .. 6 J_H) */
    int levels;        /* 1987: 4-bit levels and HAM */
    int es2;           /* the ES 2.0 way */
    int bounces;       /* reflections at most (the ES 2.0 way: 4) */
    float blend;       /* 1, or 1/n for the n-th still picture added up */
    float jitter[2];   /* within the pixel (1987: -0.5, 0.5, rt1.c's pixel corner) */
    float lampoff[3];  /* a place on the lamp */
};

/* A picture of f: the texture to show and its gain. */
static GLuint jgl_frame(struct jgl *j, const struct jframe *f, float *gain)
{
    const struct jscene *sc = &j->sc;
    double a = j->timing ? J_NOW() : 0.0;
    if (f->es2) {
        static GLfloat u[4 * 2 * J_ES2_SPHERES];
        int i, n = sc->nsph < J_ES2_SPHERES ? sc->nsph : J_ES2_SPHERES;
        if (sc->juggler) { /* the 68k animates */
            j_juggle((const float(*)[4])sc->pt, f->time, j->pts);
            j_expand(sc, (const float(*)[4])j->pts, j->sp, j->gr);
        }
        for (i = 0; i < n; i++)
            memcpy(u + 8 * i, j->sp[i].p, 8 * sizeof(GLfloat));
        jgl_lap(j, JT_ANIM, &a);
        glBindFramebuffer(GL_FRAMEBUFFER, j->es2fbo);
        glViewport(0, 0, j->w, j->h);
        glUseProgram(j->p_es2);
        glUniform4fv(j->e_sp, 2 * n, u);
        glUniform1i(j->e_ns, n);
        glUniform1i(j->e_levels, f->levels);
        glUniform2fv(j->e_jitter, 1, f->jitter);
        glUniform3fv(j->e_eye, 1, f->cam.eye);
        glUniform3fv(j->e_fwd, 1, f->cam.fwd);
        glUniform3fv(j->e_right, 1, f->cam.right);
        glUniform3fv(j->e_up, 1, f->cam.up);
        glUniform2f(j->e_size, (GLfloat)j->w, (GLfloat)j->h);
        glUniform4fv(j->e_lamp, 1, sc->lamp);
        glUniform3fv(j->e_lampcol, 1, sc->lampcol);
        glUniform3fv(j->e_g0, 1, sc->ground[0]);
        glUniform3fv(j->e_g1, 1, sc->ground[1]);
        glUniform3fv(j->e_illum, 1, sc->illum);
        glUniform3fv(j->e_zen, 1, sc->zen);
        glUniform3fv(j->e_hor, 1, sc->hor);
        glBindBuffer(GL_ARRAY_BUFFER, j->vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        jgl_lap(j, JT_TRACE, &a);
        *gain = 1.0f;
        return j->es2tex;
    }
    if (sc->juggler) {
        glUseProgram(j->p_anim);
        glUniform4fv(j->a_rest, J_POINTS, &sc->pt[0][0]);
        glUniform1f(j->a_time, f->time);
        glUniform1i(j->a_ns, sc->nsph);
        glUniform1i(j->a_ng, sc->nobj);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    glUseProgram(j->p_shadow);
    glUniform4fv(j->w_lamp, 1, sc->lamp);
    glUniform3fv(j->w_lampoff, 1, f->lampoff);
    glUniform3fv(j->w_centre, 1, j->centre);
    glUniform1i(j->w_ns, sc->nsph);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    jgl_lap(j, JT_ANIM, &a);
    j->cur ^= 1;
    glBindImageTexture(0, j->hdr[j->cur], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, j->hdr[j->cur ^ 1]);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(j->p_trace);
    glUniform3fv(j->t_eye, 1, f->cam.eye);
    glUniform3fv(j->t_fwd, 1, f->cam.fwd);
    glUniform3fv(j->t_right, 1, f->cam.right);
    glUniform3fv(j->t_up, 1, f->cam.up);
    glUniform2i(j->t_size, j->w, j->h);
    glUniform1i(j->t_ns, sc->nsph);
    glUniform1i(j->t_ng, sc->nobj);
    glUniform1i(j->t_levels, f->levels);
    glUniform1i(j->t_bounces, j->dbg & 2 ? 0 : f->bounces);
    glUniform4fv(j->t_lamp, 1, sc->lamp);
    glUniform3fv(j->t_lampcol, 1, sc->lampcol);
    glUniform3fv(j->t_lampoff, 1, f->lampoff);
    glUniform3fv(j->t_g0, 1, sc->ground[0]);
    glUniform3fv(j->t_g1, 1, sc->ground[1]);
    glUniform3fv(j->t_illum, 1, sc->illum);
    glUniform3fv(j->t_zen, 1, sc->zen);
    glUniform3fv(j->t_hor, 1, sc->hor);
    glUniform2fv(j->t_jitter, 1, f->jitter);
    glUniform1f(j->t_blend, f->blend);
    glUniform1i(j->t_dbg, j->dbg);
    glDispatchCompute((j->w + 15) / 16, (j->h + 15) / 16, 1); /* trace.comp's TILE * SUB */
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                    GL_FRAMEBUFFER_BARRIER_BIT);
    jgl_lap(j, JT_TRACE, &a);
    if (!f->levels) {
        *gain = 16.0f / 15.0f;
        return j->hdr[j->cur];
    }
    if (!j->pal_ready) {
        jgl_palette(j);
        jgl_near(j);
    }
    glUseProgram(j->p_ham);
    glUniform2i(j->h_size, j->w, j->h);
    glDispatchCompute((j->h + 255) / 256, 1, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    jgl_lap(j, JT_HAM, &a);
    *gain = 1.0f;
    return j->ham;
}

#ifndef JGL_NO_SHOW
/* The picture t over the viewport the caller set. */
static void jgl_show(struct jgl *j, GLuint t, float gain, int es2)
{
    double a = j->timing ? J_NOW() : 0.0;
    glBindTexture(GL_TEXTURE_2D, t);
    if (es2) {
        glUseProgram(j->p_es2show);
        glBindBuffer(GL_ARRAY_BUFFER, j->vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        glUseProgram(j->p_show);
        glUniform1f(j->s_gain, gain);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    jgl_lap(j, JT_SHOW, &a);
}
#endif

#endif
