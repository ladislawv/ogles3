#version 300 es
// Boing 2026 for OpenGL ES 3: the ball seen from light u_k, into that
// light's half of the shadow maps' depth texture (the viewport says which).
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
uniform int u_k;
layout(location = 0) in vec3 a_pos;

void main()
{
    gl_Position = f_lightvp[u_k] * (f_model * vec4(a_pos, 1.0));
}
