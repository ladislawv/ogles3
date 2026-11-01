#version 300 es
#define POOL
precision highp float;
precision highp int;
// Boing 2026 for OpenGL ES 3: the walls and the ceiling, as room.frag draws
// them, writing two targets at once: their colour, and their part brighter
// than white (floats) for the bloom, which no longer draws the room a second
// time. The floor has a program of its own (floor3.frag), so these carry
// none of its reflections; each wall's normal is flat (one per triangle,
// not interpolated). Both lights' shadow
// maps are the halves of one depth texture read through a shadow sampler:
// the texture unit compares and filters 2x2 depths in one read, so a few
// taps give a smooth penumbra; with contact-hardening it widens with the
// distance from the ball (sharp where the ball touches).
//   u_out: 0 the colour and the bright part, 3 the shadow terms alone (M)
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
uniform highp sampler2DShadow u_shadow;
uniform sampler2D u_water; // the ripples (water3.frag): z, w the slope
// The pool (key 9) or the dry floor: the same source compiled twice, its
// pool form (pool3.frag, poolroom3.frag: made by the Makefile) with POOL
// defined; the dry one then carries none of the water's code.
#ifdef POOL
const bool WATER = true;
#else
const bool WATER = false;
#endif
uniform sampler2D u_rcaus; // the light the water reflects, a quarter a surface (refl3.frag)
in vec3 v_wpos;
flat in vec3 v_n;
in vec4 v_lpos;
in vec4 v_lpos2;
layout(location = 0) out vec4 o_col;
layout(location = 1) out vec4 o_bright;

const vec3 NEON = vec3(1.0, 0.10, 0.85);
const vec3 HAZE = vec3(0.12, 0.35, 1.3);  // the glow round the lines: blue
const int TAPS = 6;         // each 2x2 depths, compared and filtered

// Grid lines: x the sharp line (about 1.5 pixels), y its glow (in cells,
// the same at every resolution), z a faint soft glow round both, coloured
// apart (HAZE): a tenth of a Gaussian over about 3 pixels and of a wide halo.
vec3 grid(vec2 p)
{
    vec2 d = abs(fract(p - 0.5) - 0.5);
    vec2 px = d / max(fwidth(p), 1e-4);
    float m = min(px.x, px.y), dm = min(d.x, d.y);
    float line = clamp(1.3 - m, 0.0, 1.0), glow = exp(-dm * 28.0);
    float haze = 0.125 * exp(-m * m * 0.22) + 0.0135 * exp(-dm * 9.0);
    return vec3(line, glow, haze);
}

// Light k's shadow at this point (lpos: the point in its map's space).
float shadow(int k, vec4 lpos)
{
    if (f_mode.x < 0.5)
        return 1.0;
    vec3 s = lpos.xyz * 0.5 + 0.5; // orthographic: w is 1
    // Only the ball casts a shadow: away from its footprint in the map,
    // nothing to read (most of the screen).
    vec2 fo = s.xy - f_occluder[k].xy;
    if (s.z > 1.0 || dot(fo, fo) > f_occluder[k].z * f_occluder[k].z)
        return 1.0;
    float soft = f_shadow.y;
    if (f_shadow.z > 0.5) {
        // Contact-hardening: the penumbra grows with the distance from the
        // ball's surface (the ball is the only caster: no blocker search).
        float d = max(length(v_wpos - f_ball.xyz) - f_ball.w, 0.0);
        soft = mix(0.0015, 0.03, clamp(d * 0.33, 0.0, 1.0));
    }
    float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2832;
    vec2 o = vec2(cos(rot), sin(rot));
    mat2 golden = mat2(-0.7374, 0.6755, -0.6755, -0.7374); // 2.4 rad
    // A constant count: the compiler unrolls the loop and the texture unit
    // overlaps the reads (a count from a uniform keeps a real loop, each
    // read waited for: 0.7 ms more at 640 x 480).
    float lit = 0.0, z = s.z - 0.003, side = float(k) * 0.5;
    for (int i = 0; i < TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(TAPS)) * soft;
        vec2 t = s.xy + o * r;
        lit += texture(u_shadow, vec3(t.x * 0.5 + side, t.y, z));
        o = golden * o;
    }
    return lit / float(TAPS);
}

// The light the water reflects onto this wall or the ceiling (n: its
// normal), as refl3.vert and refl3.frag traced it this frame: 1 as flat water
// would give, more where the waves focus it, 0 where none reaches. Four
// bilinear reads, averaged, so its texels blend into soft lines.
float reflected(vec3 n)
{
    vec2 c, quad;
    if (n.z > 0.5) {            // the back wall
        c = vec2(v_wpos.x / 5.5, v_wpos.y / 3.5 - 1.0);
        quad = vec2(0.0, 0.0);
    } else if (n.y < -0.5) {    // the ceiling
        c = vec2(v_wpos.x / 5.5, (v_wpos.z - 4.0) / 9.0);
        quad = vec2(1.0, 1.0);
    } else {                    // the left wall (normal +x) or the right
        c = vec2((v_wpos.z - 4.0) / 9.0, v_wpos.y / 3.5 - 1.0);
        quad = n.x > 0.0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    }
    vec2 uv = (quad + clamp(c * 0.5 + 0.5, 0.004, 0.996)) * 0.5;
    float t = 0.75 / 512.0;
    float s = textureLod(u_rcaus, uv + vec2(-t, -t), 0.0).r + textureLod(u_rcaus, uv + vec2(t, -t), 0.0).r +
              textureLod(u_rcaus, uv + vec2(-t, t), 0.0).r + textureLod(u_rcaus, uv + vec2(t, t), 0.0).r;
    return min(s * 0.25, 4.0);
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    o_bright = vec4(0.0, 0.0, 0.0, 1.0);
    if (u_out == 3) {
        // M: the first light's shadow in red and blue, the second's in
        // green: each shows as the colour the other leaves.
        float a1 = shadow(0, v_lpos), a2 = shadow(1, v_lpos2);
        o_col = vec4(a1, a2, a1, 1.0);
        return;
    }
    vec3 n = v_n;
    vec2 uv = abs(n.y) > 0.5 ? v_wpos.xz : (abs(n.z) > 0.5 ? v_wpos.xy : v_wpos.zy);
    vec3 g = grid(uv);

    if (f_mode.z > 0.5) {
        // 1984: grey room, purple lines, a flat shadow.
        vec3 c = mix(vec3(0.66), vec3(0.62, 0.0, 0.62), g.x);
        c *= mix(0.45, 1.0, shadow(0, v_lpos));
        if (n.y < -0.5)
            c = vec3(0.66);
        o_col = vec4(c, 1.0);
        return;
    }

    vec3 tol = f_light.xyz - v_wpos;
    float dist2 = dot(tol, tol);
    vec3 l = tol * inversesqrt(dist2);
    float nl = max(dot(n, l), 0.0);
    // The ceiling panel's spotlight points down: a little of it washes the
    // walls.
    float atten = 30.0 / (dist2 + 1.0) * 0.3;
    float sh = shadow(0, v_lpos);
    float sh2 = shadow(1, v_lpos2);

    // The ball's occlusion of the sky above this point (analytic sphere).
    vec3 tb = f_ball.xyz - v_wpos;
    float db2 = dot(tb, tb);
    float occ = clamp(dot(n, tb) * inversesqrt(db2) * f_ball.w * f_ball.w / db2, 0.0, 1.0);
    float ao = 1.0 - occ * 0.9;

    const vec3 albedo = vec3(0.05, 0.035, 0.08); // dark lacquer
    vec3 col = albedo * (f_lightcol.rgb * nl * atten * sh + vec3(0.05, 0.02, 0.10) * ao);
    col += vec3(1.0, 0.2, 0.2) * occ * 0.10 * sh; // red light bounced off the ball

    // The second light: a pool of its colour where it passes.
    vec3 t2 = f_light2.xyz - v_wpos;
    float d2 = dot(t2, t2);
    vec3 l2 = t2 * inversesqrt(d2);
    float nl2 = max(dot(n, l2), 0.0), at2 = 14.0 / (d2 + 1.0);
    col += albedo * f_light2col.rgb * nl2 * at2 * ao * sh2;
    col += (NEON * g.x * 1.25 + HAZE * (g.y * 0.30 + g.z)) * mix(0.12, 1.0, sh) * mix(0.35, 1.0, sh2) * ao;

    // Water (key 9): the second light (flying round the ball) reflected off
    // the pool onto the walls and the ceiling, traced (refl3.vert): its
    // strength as from the light's mirror image under the water, its lines
    // where the waves focus the reflection; dark where none reaches.
    if (WATER) {
        vec3 lm = f_light2.xyz * vec3(1.0, -1.0, 1.0);
        vec3 tm = lm - v_wpos;
        float face = max(dot(n, tm) * inversesqrt(dot(tm, tm)), 0.0);
        float bounce = 14.0 / (dot(v_wpos - lm, v_wpos - lm) + 1.0) * face;
        col += f_light2col.rgb * bounce * reflected(n) * 0.2 * ao;
    }

    // Ceiling: the light panel.
    if (n.y < -0.5) {
        vec2 q = abs(v_wpos.xz - f_light.xz);
        float panel = (1.0 - smoothstep(1.4, 1.5, q.x)) * (1.0 - smoothstep(0.7, 0.8, q.y));
        col = mix(col, f_lightcol.rgb * 1.4, panel);
    }

    // Light fades into the depth of the room.
    col *= exp(-max(length(v_wpos - f_eye.xyz) - 8.0, 0.0) * 0.07);

    o_bright = vec4(min(max(col - 1.0, 0.0), 6.0), 1.0); // held as the ball's (ball3.frag)
    o_col = vec4(pow(aces(col), vec3(1.0 / 2.2)), 1.0);
}
