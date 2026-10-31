#version 300 es
precision highp float;
precision highp int;
// Boing 2026 for OpenGL ES 3: the ball, as ball.frag, its data from the
// frame's uniform block. u_out: 0 the window (adds the bloom and the
// vignette under itself), 1 its bright part into the bloom chain, 2 linear
// colour for the floor's reflection. The targets are half floats: no
// scaling to keep values above 1.0.
layout(std140) uniform Frame {
    mat4 f_vp;
    mat4 f_vpm;           // the camera mirrored in the floor
    mat4 f_lightvp[2];
    mat4 f_model;         // the ball: world from object
    mat4 f_nmat;          // its normals (the upper 3x3)
    vec4 f_light;
    vec4 f_lightcol;
    vec4 f_light2;
    vec4 f_light2col;
    vec4 f_eye;
    vec4 f_ball;          // centre, radius
    vec4 f_mode;          // x shadow, y reflection, z 1984, w Gouraud
    vec4 f_mode2;         // x checker antialiasing, y bloom, z aspect, w vignette
    vec4 f_shadow;        // x live impacts, y softness, z contact-hardening, w the room's back z
    vec4 f_occluder[2];   // the ball in each shadow map: centre, radius
    vec4 f_post;          // 1 / the window's size, 1 / the room target's
    vec4 f_cam;           // projection x and y scale, time, the ceiling's y
    vec4 f_flare;         // the second light on screen, visibility, aspect
    vec4 f_impact[8];     // sparks: where, w when
    vec4 f_impactn[8];    // the surface's normal there
};
uniform int u_out;
uniform int u_water;      // 1: the floor is water (key 9), the ball dips into it
uniform sampler2D u_bloomtex;
in vec3 v_obj;
in vec3 v_wpos;
in vec3 v_n;
in vec3 v_eye;
in vec2 v_gour;
layout(location = 0) out vec4 o_col;

const float PI = 3.14159265;

// One axis of the checkerboard, -1 or 1, blended over a pixel at the edges.
float square(float a, float fw)
{
    float t = abs(fract(a * 0.5) - 0.5) * 2.0 - 0.5;
    if (f_mode2.x < 0.5)
        return t < 0.0 ? -1.0 : 1.0;
    return clamp(t / max(fw, 1e-4), -0.5, 0.5) * 2.0;
}

// The room as the ball sees it: a dark purple box, its neon horizon and the
// ceiling's light panel.
vec3 env(vec3 r)
{
    vec3 l = normalize(f_light.xyz - v_wpos);
    vec3 c = mix(vec3(0.012, 0.004, 0.03), vec3(0.30, 0.03, 0.42), exp(-abs(r.y) * 4.0));
    if (r.y < 0.0) {
        vec2 g = r.xz / max(-r.y, 0.05) * 2.0;
        vec2 d = abs(fract(g) - 0.5);
        c += vec3(0.9, 0.08, 0.8) * exp(-min(0.5 - d.x, 0.5 - d.y) * 10.0) * exp(r.y * 3.0) * 0.3;
    }
    float p = dot(r, l);
    c += f_lightcol.rgb * (smoothstep(0.97, 0.985, p) * 9.0 + pow(max(p, 0.0), 24.0) * 0.15);
    return c;
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    // In the water the mirror shows only what is above it.
    if (u_water == 1 && u_out == 2 && v_wpos.y < 0.0)
        discard;
    vec3 p = normalize(v_obj);
    float u = atan(p.z, p.x) * (8.0 / PI);
    float v = asin(clamp(p.y, -1.0, 1.0)) * (8.0 / PI);
    float u2 = fract(u / 16.0) * 16.0; // the same, its seam at u = 0, not +-8
    float fu = min(fwidth(u), fwidth(u2)), fv = fwidth(v);
    float k = 0.5 + 0.5 * square(u, fu) * square(v + 4.0, fv);

    vec3 red = vec3(0.80, 0.015, 0.01), white = vec3(0.92, 0.90, 0.88);
    if (f_mode.z > 0.5) {
        o_col = vec4(mix(white, vec3(0.95, 0.0, 0.0), k), 1.0);
        return;
    }
    vec3 base = mix(white, red, k);

    vec3 n = normalize(v_n);
    vec3 vd = normalize(v_eye - v_wpos);
    vec3 l = normalize(f_light.xyz - v_wpos);
    vec3 h = normalize(l + vd);
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, vd), 1e-3);
    float fres = 0.04 + 0.96 * pow(1.0 - nv, 5.0);

    float ao = mix(0.35, 1.0, clamp(v_wpos.y * 1.4, 0.0, 1.0)) * mix(1.0, 0.8, max(-n.y, 0.0));
    vec3 hemi = mix(vec3(0.10, 0.02, 0.14), vec3(0.06, 0.05, 0.12), n.y * 0.5 + 0.5);
    vec3 col;
    if (f_mode.w > 0.5) {
        // Per vertex: the highlight smears between the vertices.
        col = base * (f_lightcol.rgb * v_gour.x * 0.30 + hemi * ao) + f_lightcol.rgb * v_gour.y * 0.8;
    } else {
        float a = 0.16 * 0.16, nh = max(dot(n, h), 0.0), lh = max(dot(l, h), 0.0);
        float d = nh * nh * (a - 1.0) + 1.0;
        float spec = a / (PI * d * d) * (0.04 + 0.96 * pow(1.0 - lh, 5.0)) / (4.0 * lh * lh + 1e-3);
        col = base * (f_lightcol.rgb * nl * 0.30 + hemi * ao);
        col += f_lightcol.rgb * spec * nl * 3.0;
        col += env(reflect(-vd, n)) * fres * ao;
        col += vec3(0.7, 0.1, 1.0) * pow(1.0 - nv, 4.0) * 0.35; // neon rim

        // The second light: diffuse and its own highlight, fading with
        // distance.
        vec3 t2 = f_light2.xyz - v_wpos;
        float d2 = dot(t2, t2);
        vec3 l2 = t2 * inversesqrt(d2);
        vec3 h2 = normalize(l2 + vd);
        float nl2 = max(dot(n, l2), 0.0), nh2 = max(dot(n, h2), 0.0), lh2 = max(dot(l2, h2), 0.0);
        float dd2 = nh2 * nh2 * (a - 1.0) + 1.0;
        float spec2 = a / (PI * dd2 * dd2) * (0.04 + 0.96 * pow(1.0 - lh2, 5.0)) / (4.0 * lh2 * lh2 + 1e-3);
        col += f_light2col.rgb * (12.0 / (d2 + 2.0)) * nl2 * (base * 0.30 + spec2 * 3.0);
    }

    // Below the surface the ball is seen through the water: dimmed and
    // tinted the more the deeper, as the bottom is.
    if (u_water == 1 && v_wpos.y < 0.0)
        col *= mix(vec3(0.55, 0.8, 1.0), vec3(0.15, 0.3, 0.4), clamp(-v_wpos.y * 1.5, 0.0, 1.0));

    // The reflection and the bright part are floats, but held to what the
    // highlight should give (4 times white, 6 above it): a sharper, steadier
    // glow than the ES 2.0 way's 8 bits, without the clear-coat's peaks
    // blooming into blots.
    if (u_out == 2)
        o_col = vec4(min(col, 4.0), 1.0);
    else if (u_out == 1)
        o_col = vec4(min(max(col - 1.0, 0.0), 6.0), 1.0);
    else {
        // On screen: the bloom and the vignette, as composite3.frag gives
        // the room around it.
        vec2 suv = gl_FragCoord.xy * f_post.xy;
        vec3 b = texture(u_bloomtex, suv).rgb * f_mode2.y;
        vec2 q = (suv - 0.5) * vec2(f_mode2.z, 1.0);
        float vig = 1.0 - dot(q, q) * 0.55 * f_mode2.w;
        o_col = vec4(1.0 - exp(-b) + pow(aces(col), vec3(1.0 / 2.2)) * vig, 1.0);
    }
}
