#version 300 es
precision highp float;
precision highp int;
// Boing 2026 for OpenGL ES 3: the floor, as room.frag draws it, writing two
// targets at once: its colour, and its part brighter than white (floats) for
// the bloom, which no longer draws the room a second time. A program of its
// own (the walls and the ceiling are room3.frag): the walls carry none of
// its reflections, and with the water (key 9) off its normal is a constant
// the lighting's dot products are folded with. Both lights' shadow
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
uniform sampler2D u_refl;
uniform sampler2D u_water; // the ripples (water3.frag): z, w the slope
// The pool (key 9) or the dry floor: the same source compiled twice, its
// pool form (pool3.frag, poolroom3.frag: made by the Makefile) with POOL
// defined; the dry one then carries none of the water's code.
#ifdef POOL
const bool WATER = true;
#else
const bool WATER = false;
#endif
uniform sampler2D u_caus;  // the caustics on the bottom (caust3.frag)
uniform float u_depth;    // the water's depth in the middle (keys + and -)
in vec3 v_wpos;
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

// Small waves always running over the pool, four of them, exact: too low
// to see as ripples, they make the caustics shimmer as a pool's do. x, y
// their slope, z its divergence (the curvature that focuses the light).
vec3 fine(vec2 p)
{
    const vec4 W[4] = vec4[4](vec4(0.8, 0.6, 3.1, 1.1), vec4(-0.5, 0.87, 4.3, 1.5),
                              vec4(0.2, -0.98, 5.9, 1.9), vec4(-0.9, -0.44, 7.7, 2.3));
    vec3 s = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float ph = dot(W[i].xy, p) * W[i].z - f_cam.z * W[i].w;
        float a = 0.02; // the slope's size
        s.xy += W[i].xy * (a * cos(ph));
        s.z -= a * W[i].z * sin(ph);
    }
    return s;
}

// The water's caustics at a point of the bottom (world x, z): the light
// the lamp sends through the waves, as caust3.vert and caust3.frag drew it
// this frame (1 as flat water gives; above where the waves focus it).
float caustic(vec2 p)
{
    vec2 uv = p * (1.0 / 11.0) + 0.5;
    if (uv != clamp(uv, 0.0, 1.0))
        return 1.0;
    // Four bilinear reads round the point, averaged: the texture's texels
    // blend into soft lines instead of showing as squares.
    float t = 0.75 / 256.0;
    float c = textureLod(u_caus, uv + vec2(-t, -t), 0.0).r + textureLod(u_caus, uv + vec2(t, -t), 0.0).r +
              textureLod(u_caus, uv + vec2(-t, t), 0.0).r + textureLod(u_caus, uv + vec2(t, t), 0.0).r;
    return min(c * 0.25, 3.0);
}

// The pool's depth at x, z: none at the walls, u_depth in the middle (as
// caust3.vert has it).
float depth(vec2 p)
{
    vec2 q = min(abs(p) * (1.0 / 5.5), 1.0);
    return u_depth * (1.0 - q.x * q.x) * (1.0 - q.y * q.y);
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
    // Water (key 9): a pool over the floor, its ripples simulated on the
    // GPU over the 11 x 11 units round the room's middle. Their slope tilts
    // the normal, the grid on the bottom is refracted through the depth,
    // and the light reaching the bottom is gathered into caustics.
    // The floor's surface is the water's; what is seen through it lies
    // the depth below, where the view ray, bent by the water (about 3/4), meets
    // the bottom: the grid and the caustics are worked out there.
    vec3 vd = normalize(f_eye.xyz - v_wpos);
    vec3 n = vec3(0.0, 1.0, 0.0);
    vec2 refr = vec2(0.0), bottom = v_wpos.xz;
    float deep = 0.0; // the water's depth where the view ray meets the bottom
    if (WATER) {
        // The view ray, bent by the water (about 3/4), to the curved
        // bottom: three steps closer to where it meets it.
        vec2 dir = vd.xz / max(vd.y, 0.25) * 0.75;
        for (int i = 0; i < 3; i++)
            bottom = v_wpos.xz - dir * depth(bottom);
        deep = depth(bottom);
        vec2 wuv = v_wpos.xz * (1.0 / 11.0) + 0.5;
        if (wuv == clamp(wuv, 0.0, 1.0)) {
            vec2 sl = textureLod(u_water, wuv, 0.0).zw * 6.0 + fine(v_wpos.xz).xy;
            n = normalize(vec3(-sl.x, 1.0, -sl.y));
            refr = -sl * deep;
        }
        bottom += refr;
    }
    vec3 g = grid(bottom);

    if (f_mode.z > 0.5) {
        // 1984: grey floor, purple lines, a flat shadow.
        vec3 c = mix(vec3(0.66), vec3(0.62, 0.0, 0.62), g.x);
        o_col = vec4(c * mix(0.45, 1.0, shadow(0, v_lpos)), 1.0);
        return;
    }

    vec3 tol = f_light.xyz - v_wpos;
    float dist2 = dot(tol, tol);
    vec3 l = tol * inversesqrt(dist2);
    float nl = max(l.y, 0.0); // the bottom is flat, whatever the waves do
    // A spotlight from the ceiling panel, straight down.
    float atten = 30.0 / (dist2 + 1.0) * smoothstep(0.3, 0.8, l.y);
    // Under water the second light (flying round the ball) reaches the
    // bottom through the waves, gathered and spread: the caustics there.
    float ca = 1.0;
    if (WATER)
        ca = caustic(bottom);
    float sh = shadow(0, v_lpos);
    float sh2 = shadow(1, v_lpos2);

    // The ball's occlusion of the sky above this point (analytic sphere).
    vec3 tb = f_ball.xyz - v_wpos;
    float db2 = dot(tb, tb);
    float occ = clamp(tb.y * inversesqrt(db2) * f_ball.w * f_ball.w / db2, 0.0, 1.0);
    float ao = 1.0 - occ * 0.9;

    // A pool of light that the shadow cuts. Under water the bottom is a
    // lighter tile, so the caustics show as they focus and spread the light;
    // the surface reflects less (sur).
    vec3 albedo = WATER ? vec3(0.07, 0.08, 0.085) : vec3(0.06, 0.055, 0.07);
    float sur = WATER ? 0.3 : 1.0;
    vec3 col = albedo * (f_lightcol.rgb * nl * atten * sh + vec3(0.05, 0.02, 0.10) * ao);
    col += vec3(1.0, 0.2, 0.2) * occ * 0.10 * sh; // red light bounced off the ball

    // The second light: a pool of its colour where it passes.
    vec3 t2 = f_light2.xyz - v_wpos;
    float d2 = dot(t2, t2);
    vec3 l2 = t2 * inversesqrt(d2);
    float nl2 = max(l2.y, 0.0), at2 = 14.0 / (d2 + 1.0);
    col += albedo * f_light2col.rgb * nl2 * at2 * ao * sh2 * ca;
    col += (NEON * g.x * 1.25 + HAZE * (g.y * 0.30 + g.z)) * mix(0.12, 1.0, sh) * mix(0.35, 1.0, sh2) * ao;
    if (WATER) // seen through the water: the deep tinted, the caustics on the bottom
        col = col * mix(vec3(1.0), vec3(0.45, 0.75, 1.0), min(deep * 0.8, 1.0)) + f_light2col.rgb * at2 * sh2 * max(ca - 1.0, 0.0) * 0.06;

    // Polished: a sharp highlight and the reflections.
    vec3 h = normalize(l + vd);
    // Water is smoother than the lacquer: its highlight a sharp glint.
    float rough = WATER ? 0.02 : 0.08;
    float a = rough * rough, nh = max(dot(n, h), 0.0);
    float d = nh * nh * (a - 1.0) + 1.0;
    col += f_lightcol.rgb * a / (3.14159 * d * d) * 0.06 * max(dot(n, l), 0.0) * atten * sh * sur;
    vec3 h2 = normalize(l2 + vd);
    float nh2 = max(dot(n, h2), 0.0), dh2 = nh2 * nh2 * (a - 1.0) + 1.0;
    // On water its glint (the light flying round the ball) is bright: sharp
    // points of light dancing on the ripples.
    col += f_light2col.rgb * a / (3.14159 * dh2 * dh2) * 0.06 * nl2 * at2 * sh2 * (WATER ? 5.0 : 1.0);

    float nv = max(dot(n, vd), 0.0);
    float fres = mix(0.30, 1.0, pow(1.0 - nv, 5.0)) * 0.85;
    // Water reflects less than the lacquer, but the ball keeps most of its
    // mirror image clearly (fball: at least 75%, more at glancing angles).
    float fball = WATER ? 0.75 + 0.25 * pow(1.0 - nv, 5.0) : fres;
    fres *= WATER ? 0.35 : 1.0;
    // The back wall's grid and the ceiling's light panel, by tracing the
    // mirrored view ray to them.
    vec3 r = reflect(-vd, n);
    r.y = max(r.y, 0.02);
    {
        vec3 c = v_wpos + r * ((f_cam.w - v_wpos.y) / r.y);
        vec2 q = abs(c.xz - f_light.xz);
        float e = WATER ? 0.02 : 0.15; // the panel's edge: sharp on water
        float panel = (1.0 - smoothstep(1.45 - e, 1.45 + e, q.x)) * (1.0 - smoothstep(0.75 - e, 0.75 + e, q.y));
        col += f_lightcol.rgb * panel * fres * 1.2 * sh * sur;
    }
    if (r.z < -0.01) {
        float t = (f_shadow.w - v_wpos.z) / r.z;
        vec3 hit = v_wpos + r * t;
        if (hit.y < 7.0) {
            vec3 gw = grid(hit.xy);
            col += (NEON * gw.x * 1.1 + HAZE * (gw.y * 0.25 + gw.z)) * fres * exp(-t * 0.08);
        }
    }
    if (f_mode.y > 0.5) {
        // One bilinear read of the half-size, half-float texture.
        vec4 rc = textureLod(u_refl, gl_FragCoord.xy * f_post.zw + n.xz * 0.08, 0.0);
        col = col * (1.0 - rc.a * fball) + rc.rgb * fball;
    }

    // Light fades into the depth of the room.
    col *= exp(-max(length(v_wpos - f_eye.xyz) - 8.0, 0.0) * 0.07);

    // Held as the ball's (ball3.frag); the water's much less, so its glints
    // and caustics stay sharp instead of blooming into a haze.
    o_bright = vec4(min(max(col - 1.0, 0.0), 6.0) * (WATER ? 0.15 : 1.0), 1.0);
    o_col = vec4(pow(aces(col), vec3(1.0 / 2.2)), 1.0);
}
