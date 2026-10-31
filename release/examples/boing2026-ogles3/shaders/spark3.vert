#version 300 es
// Boing 2026 for OpenGL ES 3: sparks where the ball hits the floor or a
// wall. One instanced draw, 640 streaks for each impact still throwing
// sparks (the frame's block: where, when, the surface's normal; the CPU
// puts those first, their count in f_shadow.x, and draws only theirs; the
// reflection and the bloom take each impact's first 128): each
// spark's direction, speed and life come from integer hashes of
// gl_InstanceID, its path (thrown, falling, bouncing once on the floor) is
// worked out here, the quad's corner is gl_VertexID. No vertex data and no
// CPU work per spark. A spark behind the ball is hidden by a test against
// the sphere (there is no depth buffer).
//   u_out: 0 the window, 1 the bloom, 2 the floor's reflection (mirrored)
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
out float v_across;
flat out vec3 v_col;

const float ROOM_X = 5.45;

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rnd(inout uint s)
{
    s = hash(s);
    return float(s >> 8) * (1.0 / 16777216.0);
}

// Where a spark thrown from p0 at v0 is at time t: it falls, bounces once
// on the floor at time tb, losing most of its speed, and slides to rest.
vec3 path(vec3 p0, vec3 v0, float tb, float t)
{
    vec3 p;
    if (t < tb) {
        p = p0 + v0 * t;
        p.y -= 4.9 * t * t;
    } else {
        vec3 pb = p0 + v0 * tb, vb = vec3(v0.x * 0.5, -(v0.y - 9.8 * tb) * 0.35, v0.z * 0.5);
        float u = t - tb;
        pb.y = 0.0;
        p = pb + vb * u;
        p.y = max(p.y - 4.9 * u * u, 0.0);
    }
    p.x = clamp(p.x, -ROOM_X, ROOM_X);
    return p;
}

void hide()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped
    v_across = 0.0;
    v_col = vec3(0.0);
}

void main()
{
    // Instance i: impact i % live, its spark i / live, so the first
    // 128 x live instances are each impact's first 128 sparks.
    int live = int(f_shadow.x);
    int k = gl_InstanceID % live, j = gl_InstanceID / live;
    vec4 imp = f_impact[k];
    float age = f_cam.z - imp.w;
    uint s = uint(j) * 747796405u + uint(imp.w * 997.0);
    float life = 0.45 + 0.9 * rnd(s);
    if (age < 0.0 || age > life) {
        hide();
        return;
    }
    vec3 n = f_impactn[k].xyz;
    vec3 d = normalize(vec3(rnd(s), rnd(s), rnd(s)) * 2.0 - 1.0 + n * 1.25);
    float speed = 1.5 + 6.0 * rnd(s) * rnd(s);
    // On water (the impact's w 1, key 9) the hit throws a crown of drops
    // instead: a fifth as many, from a ring round where the ball went in,
    // up and out, each gone where it falls back in.
    bool water = f_impactn[k].w > 0.5;
    vec3 p0 = imp.xyz + n * 0.05;
    if (water) {
        if (j >= 128) {
            hide();
            return;
        }
        float a = rnd(s) * 6.2832;
        vec3 rim = vec3(cos(a), 0.0, sin(a));
        p0 += rim * (0.55 + 0.2 * rnd(s));
        d = normalize(rim * (0.35 + 0.3 * rnd(s)) + n + (vec3(rnd(s), rnd(s), rnd(s)) - 0.5) * 0.25);
        speed = 1.6 + 1.8 * rnd(s);
    }
    vec3 v0 = d * speed;
    float tb = (v0.y + sqrt(v0.y * v0.y + 19.6 * p0.y)) / 9.8;
    if (water && age > tb) {
        hide();
        return;
    }
    vec3 head = path(p0, v0, tb, age), tail = path(p0, v0, tb, max(age - (water ? 0.006 : 0.02), 0.0));

    // Behind the ball, seen from this pass's eye?
    vec3 eye = u_out == 2 ? f_eye.xyz * vec3(1.0, -1.0, 1.0) : f_eye.xyz;
    vec3 dv = head - eye;
    float len = length(dv);
    vec3 oc = f_ball.xyz - eye;
    float tc = dot(oc, dv) / len, h2 = dot(oc, oc) - tc * tc, r2 = f_ball.w * f_ball.w;
    if (h2 < r2 && tc - sqrt(r2 - h2) < len) {
        hide();
        return;
    }

    mat4 vp = u_out == 2 ? f_vpm : f_vp;
    vec4 c1 = vp * vec4(head, 1.0), c0 = vp * vec4(tail, 1.0);
    if (c1.w < 0.2 || c0.w < 0.2) {
        hide();
        return;
    }
    // A thin streak from the tail to the head, about 1.5 pixels wide.
    vec2 asp = vec2(f_flare.w, 1.0);
    vec2 s1 = c1.xy / c1.w, s0 = c0.xy / c0.w, dir = (s1 - s0) * asp;
    float sl = length(dir);
    dir = sl > 1e-5 ? dir / sl : vec2(1.0, 0.0);
    vec2 side = vec2(-dir.y, dir.x) / asp;
    float w = (water ? 2.2 : 1.5) * f_post.y; // a drop: short and round
    float along = float(gl_VertexID >> 1), across = float(gl_VertexID & 1) * 2.0 - 1.0;
    vec2 p = mix(s0 - dir / asp * w, s1 + dir / asp * w, along) + side * across * w;
    gl_Position = vec4(p, mix(c0.z / c0.w, c1.z / c1.w, along), 1.0);

    // White-hot, cooling to orange; tinted by the two lights' colours.
    float fade = 1.0 - age / life, heat = fade * fade;
    vec3 tint = mix(vec3(1.0), f_lightcol.rgb * 0.12 + f_light2col.rgb * 0.25, 0.4);
    v_col = mix(vec3(1.0, 0.30, 0.06), vec3(1.0, 0.8, 0.55), heat) * (1.5 + 9.0 * heat) * tint;
    if (water) // clear drops, only the lights caught in them: dim
        v_col = vec3(0.05, 0.07, 0.09) + f_lightcol.rgb * 0.03 + f_light2col.rgb * 0.12;
    v_across = across;
}
