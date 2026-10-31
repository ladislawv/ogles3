#version 300 es
// Boing 2026 for OpenGL ES 3: the ball. A unit sphere, moved, squashed and
// spun by the frame's block; u_out 2 draws it mirrored about the floor (the
// reflection pass), the shading staying in the real world. Also lights each
// vertex for the Gouraud mode.
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
uniform int u_out;        // 0 window, 1 bloom, 2 reflection
layout(location = 0) in vec3 a_pos;
out vec3 v_obj;
out vec3 v_wpos;
out vec3 v_n;
out vec3 v_eye;
out vec2 v_gour;

void main()
{
    vec4 w = f_model * vec4(a_pos, 1.0);
    vec3 n = normalize(mat3(f_nmat) * a_pos);
    vec3 eye = u_out == 2 ? f_eye.xyz * vec3(1.0, -1.0, 1.0) : f_eye.xyz;
    vec3 l = normalize(f_light.xyz - w.xyz);
    vec3 v = normalize(eye - w.xyz);
    vec3 h = normalize(l + v);
    v_gour = vec2(max(dot(n, l), 0.0), pow(max(dot(n, h), 0.0), 60.0));
    v_obj = a_pos;
    v_wpos = w.xyz;
    v_n = n;
    v_eye = eye;
    gl_Position = (u_out == 2 ? f_vpm : f_vp) * w;
}
