// The ES 2.0 way: trace.comp's ray tracer as a fragment shader (GLSL ES
// 1.00), a pixel each, nothing shared: every ray tests every sphere, which
// the 68k puts in uniform arrays each frame (it animates the juggler).
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define MAXS 320
#define SMALL 1.0e-3
uniform vec4 u_sp[2 * MAXS]; // per sphere: centre and radius; colour and type
uniform int u_ns, u_levels;
uniform vec3 u_eye, u_fwd, u_right, u_up;
uniform vec2 u_size, u_jitter;
uniform vec4 u_lamp;
uniform vec3 u_lampcol;
uniform vec3 u_g0, u_g1, u_illum, u_zen, u_hor;
varying vec2 v_uv;

float hit(vec3 o, vec3 d, vec4 s, float eps)
{
    vec3 oc = o - s.xyz;
    float b = dot(oc, d), q = b * b - dot(oc, oc) + s.w * s.w;
    if (q <= 0.0)
        return -1.0;
    q = sqrt(q);
    float t = -b - q;
    if (t < eps)
        t = -b + q;
    return t > eps ? t : -1.0;
}

bool blocked(vec3 o, vec3 d, float eps, int skip)
{
    for (int i = 0; i < MAXS; i++) {
        if (i >= u_ns)
            break;
        if (i != skip && hit(o, d, u_sp[2 * i], eps) > 0.0)
            return true;
    }
    return false;
}

vec3 pixbrite(vec3 p, vec3 n, vec3 col, int skip)
{
    vec3 c = (n.z + 1.5) * 0.4 * u_illum * col;
    vec3 lp = u_lamp.xyz - p;
    float cosi = dot(lp, n);
    if (cosi > 0.0) {
        float r = length(lp);
        if (!blocked(p, lp / r, SMALL * r, skip))
            c += cosi / (r * r * r) * col * u_lampcol;
    }
    return c;
}

bool glint(vec3 p, vec3 n, vec3 d, int skip)
{
    vec3 lp = u_lamp.xyz - p;
    if (dot(lp, n) <= 0.0)
        return false;
    float r = length(lp);
    if (blocked(p, lp / r, SMALL * r, skip))
        return false;
    vec3 rv = reflect(d, n);
    float t = dot(lp, rv);
    return t * t / (dot(lp, lp) * dot(rv, rv)) > 0.95;
}

vec3 ground(vec3 p)
{
    float kx = floor(abs(p.x)) + (p.x < 0.0 ? 1.0 : 0.0), ky = floor(abs(p.y)) + (p.y < 0.0 ? 1.0 : 0.0);
    return mod(floor(kx / 3.0) + floor(ky / 3.0), 2.0) < 0.5 ? u_g0 : u_g1;
}

void main()
{
    vec2 f = floor(v_uv * u_size) + 0.5 + u_jitter;
    vec3 d = u_fwd + u_right * (f.x / u_size.x - 0.5) + u_up * (f.y / u_size.y - 0.5), o = u_eye;
    vec3 thr = vec3(1.0), col = vec3(0.0);
    float eps = SMALL * length(d);
    d = normalize(d);
    for (int b = 0; b < 5; b++) { // up to 4 reflections
        float tmin = 1e10;
        int idx = -1;
        vec4 hs = vec4(0.0), hc = vec4(0.0);
        for (int i = 0; i < MAXS; i++) {
            if (i >= u_ns)
                break;
            vec4 s = u_sp[2 * i];
            float t = hit(o, d, s, eps);
            if (t > 0.0 && t < tmin) {
                tmin = t;
                idx = i;
                hs = s;
                hc = u_sp[2 * i + 1];
            }
        }
        float tl = hit(o, d, u_lamp, eps);
        if (tl > 0.0 && tl < tmin) {
            col = thr * u_lampcol / (u_lamp.w * u_lamp.w);
            break;
        }
        if (d.z != 0.0) {
            float tg = -o.z / d.z;
            if (tg > eps && tg < tmin) {
                vec3 p = o + d * tg;
                col = thr * pixbrite(p, vec3(0.0, 0.0, 1.0), ground(p), -1);
                break;
            }
        }
        if (idx >= 0) {
            vec3 p = o + d * tmin, nr = (p - hs.xyz) / hs.w;
            if (hc.w > 1.5) {
                if (dot(nr, d) >= 0.0)
                    break;
                thr *= hc.rgb;
                d = reflect(d, nr);
                o = p;
                continue;
            }
            if (hc.w > 0.5 && glint(p, nr, d, idx)) {
                col = thr;
                break;
            }
            col = thr * pixbrite(p, nr, hc.rgb, idx);
            break;
        }
        float s2 = d.z * d.z;
        col = thr * ((1.0 - s2) * u_hor + s2 * u_zen);
        break;
    }
    if (u_levels != 0)
        gl_FragColor = vec4(clamp(floor(col * 16.0 + 0.5), 0.0, 15.0) / 15.0, 1.0);
    else
        gl_FragColor = vec4(min(col * (16.0 / 15.0), vec3(1.0)), 1.0);
}
