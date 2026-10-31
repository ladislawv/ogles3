#version 300 es
// Boing 2026 for OpenGL ES 3: the room, a box seen from inside (no depth
// buffer needed), for both its programs (floor3.frag, room3.frag). Each
// wall's normal is the same at its three vertices: flat, not interpolated.
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
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
out vec3 v_wpos;
flat out vec3 v_n;
out vec4 v_lpos;
out vec4 v_lpos2;

void main()
{
    v_wpos = a_pos;
    v_n = a_nrm;
    v_lpos = f_lightvp[0] * vec4(a_pos, 1.0);
    v_lpos2 = f_lightvp[1] * vec4(a_pos, 1.0);
    gl_Position = f_vp * vec4(a_pos, 1.0);
}
