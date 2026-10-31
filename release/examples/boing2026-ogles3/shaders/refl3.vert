#version 300 es
// Boing 2026 for OpenGL ES 3: the light the water reflects onto the walls
// (key 9), traced. The water's surface as a mesh of 64 x 64 cells made from
// gl_VertexID: each vertex reflects the second light's ray (the coloured one
// flying round the ball) off the waves and follows it to the surface u_surf
// (0 the back wall, 1 the left, 2 the right, 3 the ceiling). The triangles
// are drawn there, into that surface's quarter of a float texture, added:
// refl3.frag gives each the light it carries over the area it covers, so
// where the waves focus the reflection the wall gets bright lines. A
// triangle whose rays miss the surface (or go the other way) is dropped.
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
uniform sampler2D u_wave;  // the simulation (water3.frag): z, w the slope
uniform int u_surf;
uniform float u_tilt;      // how much of the waves' tilt the reflection sees
flat out float v_light;    // the triangle's light over its new area

const int N = 64;          // cells a side, over the 11 x 11 units of the pool

// The small waves always running over the pool (as floor3.frag): the slope.
vec2 fine(vec2 p)
{
    const vec4 W[4] = vec4[4](vec4(0.8, 0.6, 3.1, 1.1), vec4(-0.5, 0.87, 4.3, 1.5),
                              vec4(0.2, -0.98, 5.9, 1.9), vec4(-0.9, -0.44, 7.7, 2.3));
    vec2 s = vec2(0.0);
    for (int i = 0; i < 4; i++)
        s += W[i].xy * (0.02 * cos(dot(W[i].xy, p) * W[i].z - f_cam.z * W[i].w));
    return s;
}

// Where the light's ray reflected off the water at p (normal n) meets the
// surface u_surf, in that surface's own coordinates, -1..1 over it; w: the
// distance along the ray, set negative where the ray goes the other way,
// grazes the surface (under about 17 degrees) or lands far off: such rays
// stretch their triangles over the whole surface, flashing as the waves
// tilt them.
vec3 land(vec3 p, vec3 n, out float glance)
{
    vec3 r = reflect(normalize(p - f_light2.xyz), n);
    float t;
    vec2 c;
    float facing;
    if (u_surf == 0) {          // the back wall, z = back
        facing = -r.z;
        t = (f_shadow.w - p.z) / r.z;
        vec3 h = p + r * t;
        c = vec2(h.x / 5.5, h.y / 3.5 - 1.0);
    } else if (u_surf == 3) {   // the ceiling
        facing = r.y;
        t = (f_cam.w - p.y) / r.y;
        vec3 h = p + r * t;
        c = vec2(h.x / 5.5, (h.z - 4.0) / 9.0);
    } else {                    // the left (x = -5.5) or right wall
        facing = u_surf == 1 ? -r.x : r.x;
        t = ((u_surf == 1 ? -5.5 : 5.5) - p.x) / r.x;
        vec3 h = p + r * t;
        c = vec2((h.z - 4.0) / 9.0, h.y / 3.5 - 1.0);
    }
    // Fading out as the ray grazes the surface, so a triangle near the
    // limit dims away instead of popping.
    glance = smoothstep(0.2, 0.4, facing);
    return vec3(c, facing > 0.2 && t < 20.0 ? t : -1.0);
}

// Corner k (0..5 round a cell) of the mesh: where its ray lands off flat
// water (xy) and off the waves (zw); ok false where it misses.
vec4 corner(int cell, int k, out bool ok, out float fade)
{
    const ivec2 C[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));
    vec2 g = vec2(ivec2(cell % N, cell / N) + C[k]) / float(N);
    vec3 p = vec3((g.x - 0.5) * 11.0, 0.0, (g.y - 0.5) * 11.0);
    // The waves at u_tilt of their tilt (keys + and -): less spreads the
    // reflection less, so its bands on the walls stay smooth.
    vec2 sl = (textureLod(u_wave, g, 0.0).zw * 6.0 + fine(p.xz)) * u_tilt;
    float gf, gb;
    vec3 f = land(p, vec3(0.0, 1.0, 0.0), gf), b = land(p, normalize(vec3(-sl.x, 1.0, -sl.y)), gb);
    // Well past the surface's edges still counts (the viewport clips the
    // overhang): the light runs into the corners and up to the top of the
    // walls instead of stopping short. The water behind the back wall reflects nothing.
    ok = b.z > 0.0 && f.z > 0.0 && abs(b.x) <= 4.0 && abs(b.y) <= 4.0 && p.z > f_shadow.w;
    // The traced patch of water fades out over its last 1.5 units, so its
    // edge (in the middle of the floor in front) leaves no line on the walls.
    vec2 e = smoothstep(5.5, 4.0, abs(p.xz));
    fade = e.x * e.y * gb;
    return vec4(f.xy, b.xy);
}

float area(vec2 a, vec2 b, vec2 c)
{
    return abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

void main()
{
    // The whole triangle, worked out by each of its three vertices alike:
    // if any corner misses the surface, or the waves stretch it more than
    // 40 times (by then it has dimmed to nothing), all three collapse and
    // it is never drawn (one stretched over the wall would cost its whole
    // area in pixels).
    int cell = gl_VertexID / 6, first = (gl_VertexID - cell * 6) / 3 * 3;
    bool o0, o1, o2;
    float f0, f1, f2;
    vec4 a = corner(cell, first, o0, f0), b = corner(cell, first + 1, o1, f1), c = corner(cell, first + 2, o2, f2);
    float before = area(a.xy, b.xy, c.xy), after = area(a.zw, b.zw, c.zw);
    int k = gl_VertexID - cell * 6 - first;
    if (!(o0 && o1 && o2) || after > before * 40.0 || before <= 0.0) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped
        v_light = 0.0;
        return;
    }
    // Stretched triangles dim away (from 10 to 40 times) rather than pop.
    float stretch = after / before;
    v_light = min(before / max(after, 1e-9), 6.0) * (f0 + f1 + f2) * (1.0 / 3.0) * smoothstep(40.0, 10.0, stretch);
    gl_Position = vec4(k == 0 ? a.zw : k == 1 ? b.zw : c.zw, 0.0, 1.0);
}
