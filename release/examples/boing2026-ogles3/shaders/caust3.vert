#version 300 es
// Boing 2026 for OpenGL ES 3: the water's caustics (key 9). The water's
// surface as a mesh of 96 x 96 cells made from gl_VertexID (no vertex data):
// each vertex reads the waves (the simulation's slope and the small running
// waves), refracts the second light's ray (the coloured one flying round the
// ball) through the surface into the water and
// goes where that ray meets the bottom, a bowl u_depth deep in the middle. The triangles are
// drawn there, into a float texture over the bottom, added: caust3.frag
// gives each the light it carries over the area it now covers, so where the
// waves focus the light the bottom gets bright lines, where they spread it
// it dims.
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
uniform float u_depth;
out vec2 v_flat;           // where the ray would land through flat water
out vec2 v_bent;           // where it lands through the waves

const int N = 96;          // cells a side, over the 11 x 11 units of the pool

// The small waves always running over the pool (as floor3.frag): x, y the
// slope.
vec2 fine(vec2 p)
{
    const vec4 W[4] = vec4[4](vec4(0.8, 0.6, 3.1, 1.1), vec4(-0.5, 0.87, 4.3, 1.5),
                              vec4(0.2, -0.98, 5.9, 1.9), vec4(-0.9, -0.44, 7.7, 2.3));
    vec2 s = vec2(0.0);
    for (int i = 0; i < 4; i++)
        s += W[i].xy * (0.02 * cos(dot(W[i].xy, p) * W[i].z - f_cam.z * W[i].w));
    return s;
}

// The pool's depth at x, z: none at the walls, u_depth in the middle.
float depth(vec2 p)
{
    vec2 q = min(abs(p) * (1.0 / 5.5), 1.0);
    return u_depth * (1.0 - q.x * q.x) * (1.0 - q.y * q.y);
}

// Where the second light's ray (the one flying round the ball) through the surface at p (normal n) meets the bottom.
vec2 land(vec3 p, vec3 n)
{
    vec3 r = refract(normalize(p - f_light2.xyz), n, 1.0 / 1.33);
    // The bottom is curved: from where the ray would meet it at the depth
    // under p, three steps closer to where it does.
    vec2 q = p.xz;
    for (int i = 0; i < 3; i++)
        q = p.xz + r.xz * (depth(q) / max(-r.y, 0.05));
    return q;
}

void main()
{
    // Two triangles a cell, their corners from the vertex's number.
    const ivec2 C[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));
    int cell = gl_VertexID / 6;
    vec2 g = vec2(ivec2(cell % N, cell / N) + C[gl_VertexID - cell * 6]) / float(N);
    vec3 p = vec3((g.x - 0.5) * 11.0, 0.0, (g.y - 0.5) * 11.0);
    vec2 sl = textureLod(u_wave, g, 0.0).zw * 6.0 + fine(p.xz);
    v_flat = land(p, vec3(0.0, 1.0, 0.0));
    v_bent = land(p, normalize(vec3(-sl.x, 1.0, -sl.y)));
    gl_Position = vec4(v_bent * (2.0 / 11.0), 0.0, 1.0);
}
