/*
 * Juggler RT's scenes, portable C (the Amiga program and the PC check share
 * it): Eric Graham's 1987 scene files (.dat, the format of his ray tracer's
 * robot.dat, ele.dat and dragon.dat), the spheres their chains make, the
 * lamp's exposure (rt2.c's lampfac), the juggling (robot.dat's figure
 * animated: three mirror balls in a cascade, two-bone arms and legs), and
 * the HAM palette as rt3.c allocated it.
 *
 * The juggling here and in shaders/animate.comp are the same sums: change
 * both.
 *
 * Eric Graham's ray tracer (1987): "anyone can do what they want with the
 * code, so long as I get a mention". This file: MIT License.
 */
#ifndef JUGGLER_SCENE_H
#define JUGGLER_SCENE_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define J_MAX_OBJECTS 64  /* = groups, the shaders' bounding spheres */
#define J_MAX_POINTS  256 /* control points: a sphere point and its chain's */
#define J_MAX_SPHERES 512
#define J_ES2_SPHERES 320 /* the ES 2.0 way's uniform arrays (es2trace.frag) */
#define J_PI 3.14159265f

enum { J_DULL = 0, J_BRIGHT = 1, J_MIRROR = 2 };

struct jobject {
    float col[3];
    int type;
    int first, npoints; /* its control points */
};

/* Spheres and groups as the shaders' storage buffers hold them (std430):
 * Sphere {vec4 p (centre, radius); vec4 c (colour, type)}, Group {vec4 b
 * (bounding sphere); ivec4 r (first sphere, count, first point, points)}. */
struct jsphere {
    float p[4], c[4];
};
struct jgroup {
    float b[4];
    int r[4];
};
/* What animate.comp reads for each sphere: its two control points, the
 * place between them (t), its type; its colour. */
struct jchain {
    float a[4], c[4];
};

struct jscene {
    float obs[3], alt, az, focal; /* observer, view angles (degrees), focal scale */
    int nobj, npts, nsph;
    struct jobject obj[J_MAX_OBJECTS];
    float pt[J_MAX_POINTS][4]; /* the control points at rest (centre, radius) */
    int seg[J_MAX_POINTS];     /* spheres a segment adds before the point (0: its object's first) */
    float lamp[4], lampcol[3]; /* the first lamp; its colour times lampfac */
    int nlamp;
    float ground[2][3], illum[3], zen[3], hor[3];
    int juggler; /* robot.dat's figure, juggling */
};

/* ---- the .dat parser (datformat-README.md's grammar) ---- */
static const char *j_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return s;
}

static int j_num(const char **s, float *v)
{
    char *e;
    double d;
    *s = j_ws(*s);
    d = strtod(*s, &e);
    if (e == *s)
        return 0;
    *v = (float)d;
    *s = e;
    return 1;
}

static int j_ch(const char **s, char c)
{
    *s = j_ws(*s);
    if (**s != c)
        return 0;
    (*s)++;
    return 1;
}

static int j_vec(const char **s, float *v, char open, char close)
{
    return j_ch(s, open) && j_num(s, v) && j_ch(s, ',') && j_num(s, v + 1) && j_ch(s, ',') && j_num(s, v + 2) &&
           j_ch(s, close);
}

static int j_point(const char **s, float *p)
{
    return j_vec(s, p, '(', ')') && j_ch(s, ':') && j_num(s, p + 3);
}

/* Parses text into *sc; 0 and *err the reason when it is not a scene. */
static int j_parse(struct jscene *sc, const char *s, const char **err)
{
    float v[2], f;
    int i;
    memset(sc, 0, sizeof(*sc));
    *err = "the observer, view angles or focal scale";
    if (!j_vec(&s, sc->obs, '(', ')') || !j_ch(&s, '[') || !j_num(&s, v) || !j_ch(&s, ',') || !j_num(&s, v + 1) ||
        !j_ch(&s, ']') || !j_num(&s, &sc->focal))
        return 0;
    sc->alt = v[0], sc->az = v[1];
    for (;;) {
        struct jobject *o;
        if (j_ch(&s, ';'))
            break; /* the list's end */
        *err = "too many objects or points";
        if (sc->nobj == J_MAX_OBJECTS || sc->npts == J_MAX_POINTS)
            return 0;
        o = &sc->obj[sc->nobj];
        *err = "an object (<colour> material (x,y,z):r ...;)";
        if (!j_vec(&s, o->col, '<', '>') || !j_num(&s, &f) || !j_point(&s, sc->pt[sc->npts]))
            return 0;
        o->type = (int)f;
        o->first = sc->npts;
        sc->seg[sc->npts++] = 0;
        sc->nsph++;
        while (!j_ch(&s, ';')) {
            *err = "too many points";
            if (sc->npts == J_MAX_POINTS)
                return 0;
            *err = "a chain (count (x,y,z):r)";
            if (!j_num(&s, &f) || f < 0 || !j_point(&s, sc->pt[sc->npts]))
                return 0;
            sc->seg[sc->npts++] = (int)f + 1;
            sc->nsph += (int)f + 1;
        }
        o->npoints = sc->npts - o->first;
        sc->nobj++;
    }
    *err = "too many spheres";
    if (sc->nsph > J_MAX_SPHERES)
        return 0;
    *err = "the lamps";
    if (!j_num(&s, &f) || f < 1)
        return 0;
    sc->nlamp = (int)f;
    for (i = 0; i < sc->nlamp; i++) {
        float p[4], c[3];
        if (!j_point(&s, p) || !j_vec(&s, c, '<', '>'))
            return 0;
        if (i == 0) /* the shaders light with one: the scenes have one */
            memcpy(sc->lamp, p, sizeof(p)), memcpy(sc->lampcol, c, sizeof(c));
    }
    *err = "the environment (five colours)";
    if (!j_vec(&s, sc->ground[0], '<', '>') || !j_vec(&s, sc->ground[1], '<', '>') || !j_vec(&s, sc->illum, '<', '>') ||
        !j_vec(&s, sc->zen, '<', '>') || !j_vec(&s, sc->hor, '<', '>'))
        return 0;
    return 1;
}

/* ---- spheres from control points ---- */
/* The spheres the points pts make (an object's first point is a sphere; a
 * segment of count n adds n + 1 between the point before and its own, the
 * last one the point itself), and each object's bounding sphere. */
static void j_expand(const struct jscene *sc, const float (*pts)[4], struct jsphere *sp, struct jgroup *gr)
{
    int o, k, d, n = 0;
    for (o = 0; o < sc->nobj; o++) {
        const struct jobject *ob = &sc->obj[o];
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, c[3], r = 0.0f;
        gr[o].r[0] = n;
        for (k = ob->first; k < ob->first + ob->npoints; k++) {
            int i, m = sc->seg[k] ? sc->seg[k] : 1;
            for (i = 1; i <= m; i++) {
                float t = sc->seg[k] ? (float)i / m : 1.0f;
                const float *a = pts[sc->seg[k] ? k - 1 : k], *b = pts[k];
                for (d = 0; d < 4; d++)
                    sp[n].p[d] = a[d] + (b[d] - a[d]) * t;
                sp[n].c[0] = ob->col[0], sp[n].c[1] = ob->col[1], sp[n].c[2] = ob->col[2];
                sp[n].c[3] = (float)ob->type;
                n++;
            }
            for (d = 0; d < 3; d++) {
                if (pts[k][d] - pts[k][3] < lo[d])
                    lo[d] = pts[k][d] - pts[k][3];
                if (pts[k][d] + pts[k][3] > hi[d])
                    hi[d] = pts[k][d] + pts[k][3];
            }
        }
        /* round the box of the control spheres, which holds every sphere
         * between them (animate.comp's sums) */
        for (d = 0; d < 3; d++)
            c[d] = 0.5f * (lo[d] + hi[d]);
        for (k = ob->first; k < ob->first + ob->npoints; k++) {
            float dx = pts[k][0] - c[0], dy = pts[k][1] - c[1], dz = pts[k][2] - c[2];
            float e = (float)sqrt(dx * dx + dy * dy + dz * dz) + pts[k][3];
            if (e > r)
                r = e;
        }
        gr[o].b[0] = c[0], gr[o].b[1] = c[1], gr[o].b[2] = c[2], gr[o].b[3] = r;
        gr[o].r[1] = n - gr[o].r[0];
        gr[o].r[2] = ob->first;
        gr[o].r[3] = ob->npoints;
    }
}

/* animate.comp's table: each sphere's two points and the place between. */
static void j_chains(const struct jscene *sc, struct jchain *ch)
{
    int o, k, n = 0;
    for (o = 0; o < sc->nobj; o++) {
        const struct jobject *ob = &sc->obj[o];
        for (k = ob->first; k < ob->first + ob->npoints; k++) {
            int i, m = sc->seg[k] ? sc->seg[k] : 1;
            for (i = 1; i <= m; i++, n++) {
                ch[n].a[0] = (float)(sc->seg[k] ? k - 1 : k);
                ch[n].a[1] = (float)k;
                ch[n].a[2] = sc->seg[k] ? (float)i / m : 1.0f;
                ch[n].a[3] = (float)ob->type;
                ch[n].c[0] = ob->col[0], ch[n].c[1] = ob->col[1], ch[n].c[2] = ob->col[2], ch[n].c[3] = 0.0f;
            }
        }
    }
}

/* rt2.c's exposure: the lamp made as bright as lets no sphere's lit side
 * pass full white (the spheres at rest). */
static void j_expose(struct jscene *sc, const struct jsphere *sp)
{
    float f = 1e10f, t, r;
    int i, k;
    for (i = 0; i < sc->nsph; i++) {
        float dx = sp[i].p[0] - sc->lamp[0], dy = sp[i].p[1] - sc->lamp[1], dz = sp[i].p[2] - sc->lamp[2];
        r = (float)sqrt(dx * dx + dy * dy + dz * dz) - sp[i].p[3];
        for (k = 0; k < 3; k++) {
            t = sp[i].c[k] * sc->lampcol[k] / (r * r);
            if (t == 0.0f)
                continue;
            t = (1.0f - sp[i].c[k] * sc->illum[k]) / t;
            if (t < f)
                f = t;
        }
    }
    for (k = 0; k < 3; k++)
        sc->lampcol[k] *= f;
}

/* ---- the juggling (robot.dat's 22 points; animate.comp's twin) ----
 * Points: 0-2 the balls, 3 head, 4 hair, 5-6 eyes, 7 neck, 8-9 the body,
 * 10-12 left hip, knee, foot, 13-15 right's, 16-18 right shoulder, elbow,
 * hand, 19-21 left's. A three-ball cascade: a throw every J_H seconds,
 * hands taking turns (right on even throws), each ball J_D in a hand and
 * J_F in the air; each hand goes round a circle (caught outside, carried
 * down and in, thrown inside, back over the top). */
#define J_POINTS 22
#define J_H 0.4f            /* seconds between throws */
#define J_D (0.75f * J_H)   /* a ball's time in a hand */
#define J_F (3.0f * J_H - J_D) /* its time in the air */
#define J_RISE 1.8f         /* a throw's height over a straight line (the top in the picture) */
/* The pattern's place (chosen by a search over these: the balls 0.2 clear
 * of each other and 0.3 of the head at every moment, tools/pccheck measures
 * it; the top in the picture; the outer catch in the arm's reach): the
 * hands' circles at x J_HX, y +-J_CY,
 * J_RY round; throws from the right bowed J_XF towards the camera, from the
 * left J_XB away, so crossing balls pass. */
#define J_HX (-1.4f)
#define J_CY 1.9f
#define J_RY 0.3f
#define J_XF 1.8f
#define J_XB 0.3f

static float j_mod(float a, float m)
{
    return a - m * (float)floor(a / m);
}

/* Hand h's place at the angle a of its circle (0 the catch, outside; pi
 * the throw, inside; the ball carried through the bottom half). */
static void j_hand_at(int h, float a, float *p)
{
    float s = h ? 1.0f : -1.0f;
    p[0] = J_HX - 0.12f * (float)sin(a);
    p[1] = s * (J_CY + J_RY * (float)cos(a));
    p[2] = 4.5f - 0.3f * (float)sin(a);
}

/* Hand h (0 right, at y < 0; 1 left) at time t: it throws at h * J_H and
 * every 2 J_H after, and caught J_D before. */
static void j_hand(int h, float t, float *p)
{
    float u = j_mod(t - h * J_H + J_D, 2.0f * J_H);
    j_hand_at(h, u < J_D ? J_PI * u / J_D : J_PI + J_PI * (u - J_D) / (2.0f * J_H - J_D), p);
}

/* Ball i at time t: thrown by hand i % 2 at i * J_H, caught by the other. */
static void j_ball(int i, float t, float *p)
{
    float tau = j_mod(t - i * J_H, 6.0f * J_H), a[3], b[3], f, g = 8.0f * J_RISE / (J_F * J_F);
    int h = i & 1, k;
    if (tau >= 3.0f * J_H)
        tau -= 3.0f * J_H, h = 1 - h; /* the second half: thrown back */
    if (tau < J_F) {
        j_hand_at(h, J_PI, a);
        j_hand_at(1 - h, 0.0f, b);
        f = tau / J_F;
        for (k = 0; k < 3; k++)
            p[k] = a[k] + (b[k] - a[k]) * f;
        p[0] += (h ? J_XB : -J_XF) * (float)sin(J_PI * f);
        p[2] += 0.5f * g * tau * (J_F - tau);
    } else
        j_hand(1 - h, t, p);
    p[2] += 0.7f; /* on the hand */
}

/* The bend at a joint: a root r, an end e, the bones' lengths la and lb,
 * bent towards pole (two-bone IK). */
static void j_ik(const float *r, const float *e, float la, float lb, const float *pole, float *j)
{
    float d[3], n[3], l, x, h, pv[3], pn, pl;
    int k;
    for (k = 0; k < 3; k++)
        d[k] = e[k] - r[k];
    l = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (k = 0; k < 3; k++)
        n[k] = d[k] / l;
    if (l > la + lb - 1e-3f)
        l = la + lb - 1e-3f;
    if (l < (float)fabs(la - lb) + 1e-3f)
        l = (float)fabs(la - lb) + 1e-3f;
    x = (la * la - lb * lb + l * l) / (2.0f * l);
    h = la * la - x * x;
    h = h > 0.0f ? (float)sqrt(h) : 0.0f;
    pn = pole[0] * n[0] + pole[1] * n[1] + pole[2] * n[2];
    for (k = 0; k < 3; k++)
        pv[k] = pole[k] - n[k] * pn;
    pl = (float)sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]) + 1e-6f;
    for (k = 0; k < 3; k++)
        j[k] = r[k] + n[k] * x + pv[k] / pl * h;
}

static float j_dist(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return (float)sqrt(x * x + y * y + z * z);
}

/* The figure at time t: pts from rest (each point's radius kept). */
static void j_juggle(const float (*rest)[4], float t, float (*pts)[4])
{
    float bob = 0.12f * (0.5f - 0.5f * (float)cos(2.0f * J_PI * t / J_H));
    float sway = 0.05f * (float)sin(J_PI * t / J_H);
    float pole[3];
    int k, l;
    for (k = 0; k < J_POINTS; k++)
        memcpy(pts[k], rest[k], sizeof(pts[k]));
    for (k = 0; k < 3; k++)
        j_ball(k, t, pts[k]);
    /* the body sinks a little at each throw and sways to the throwing hand */
    for (k = 3; k < J_POINTS; k++)
        if (k != 11 && k != 12 && k != 14 && k != 15 && k != 17 && k != 18 && k != 20 && k != 21) {
            pts[k][1] += k >= 10 && k <= 15 ? 0.3f * sway : sway;
            pts[k][2] -= bob;
        }
    for (l = 0; l < 2; l++) {
        int hip = 10 + 3 * l, sh = 16 + 3 * l;
        /* legs: the feet stay; the knees bend forwards (-x: the left leg is
           all but straight at rest, which gives no way to bend) */
        pole[0] = -1.0f, pole[1] = pole[2] = 0.0f;
        j_ik(pts[hip], pts[hip + 2], j_dist(rest[hip], rest[hip + 1]), j_dist(rest[hip + 1], rest[hip + 2]), pole,
             pts[hip + 1]);
        /* arms: the hands juggle (16-18 the right, hand 0) */
        j_hand(l, t, pts[sh + 2]);
        for (k = 0; k < 3; k++)
            pole[k] = rest[sh + 1][k] - 0.5f * (rest[sh][k] + rest[sh + 2][k]);
        j_ik(pts[sh], pts[sh + 2], j_dist(rest[sh], rest[sh + 1]), j_dist(rest[sh + 1], rest[sh + 2]), pole,
             pts[sh + 1]);
    }
}

/* ---- the camera (rt2.c's observer) ---- */
struct jcamera {
    float eye[3], fwd[3], right[3], up[3]; /* fwd: the view direction times the focal length */
};

/* alt, az in degrees; the view plane 0.75 high and 0.75 * aspect wide. */
static void j_camera(const float *obs, float alt, float az, float focal, float aspect, struct jcamera *c)
{
    float d = 0.0174533f, fl = 0.028f * focal;
    alt *= d, az *= d;
    memcpy(c->eye, obs, 3 * sizeof(float));
    c->fwd[0] = (float)(cos(az) * cos(alt)) * fl;
    c->fwd[1] = (float)(sin(az) * cos(alt)) * fl;
    c->fwd[2] = (float)sin(alt) * fl;
    c->right[0] = (float)sin(az) * 0.75f * aspect;
    c->right[1] = -(float)cos(az) * 0.75f * aspect;
    c->right[2] = 0.0f;
    c->up[0] = -(float)(cos(az) * sin(alt)) * 0.75f;
    c->up[1] = -(float)(sin(az) * sin(alt)) * 0.75f;
    c->up[2] = (float)cos(alt) * 0.75f;
}

/* ---- HAM (rt3.c) ----
 * The palette: rt3.c's allocation run over one picture of 4-bit levels
 * (rows top first, lv rgb triples): black and white first, then a colour
 * more than 4 from every register gets the next of 16. */
struct jham {
    int reg[16][3], n;
};

static int j_cd(const int *a, const int *b) /* rt3.c's coldist: the worst channel left out */
{
    int k, r = 0, m = 0, d;
    for (k = 0; k < 3; k++) {
        d = a[k] - b[k];
        if (d < 0)
            d = -d;
        r += d;
        if (d > m)
            m = d;
    }
    return r - m;
}

static int j_cd2(const int *a, const int *b)
{
    int k, r = 0, d;
    for (k = 0; k < 3; k++) {
        d = a[k] - b[k];
        r += d < 0 ? -d : d;
    }
    return r;
}

static int j_nearest(struct jham *h, const int *c, int *dist, int alloc)
{
    int i, m = 32000, best = 0, d;
    for (i = 0; i < h->n; i++) {
        d = j_cd2(c, h->reg[i]);
        if (d < m)
            m = d, best = i;
    }
    if (alloc && m > 4 && h->n < 16) {
        memcpy(h->reg[h->n], c, 3 * sizeof(int));
        best = h->n++;
        m = 0;
    }
    *dist = m;
    return best;
}

/* One row by rt3.c's ham2: px the levels in, out the colours shown. */
static void j_ham_row(struct jham *h, const int *px, int w, int *out, int alloc)
{
    int i, k, prev[3] = {0, 0, 0}, dif, dif2, pen;
    for (i = 0; i < w; i++) {
        const int *p = px + 3 * i;
        int reg = 0;
        if (!i) {
            pen = j_nearest(h, p, &dif2, alloc);
            reg = 1;
        } else {
            dif = j_cd(p, prev);
            if (dif) {
                pen = j_nearest(h, p, &dif2, alloc);
                if (dif2 < dif)
                    reg = 1;
            }
        }
        if (reg)
            memcpy(prev, h->reg[pen], sizeof(prev));
        else {
            int id = 0, m = 0;
            for (k = 0; k < 3; k++) {
                dif = p[k] - prev[k];
                if (dif < 0)
                    dif = -dif;
                if (dif > m)
                    m = dif, id = k;
            }
            prev[id] = p[id];
        }
        memcpy(out + 3 * i, prev, sizeof(prev));
    }
}

static void j_ham_init(struct jham *h)
{
    memset(h, 0, sizeof(*h));
    h->reg[1][0] = h->reg[1][1] = h->reg[1][2] = 15;
    h->n = 2;
}

#endif
