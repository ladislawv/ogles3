#version 300 es
precision highp float;
precision highp int;
// Boing 2026 for OpenGL ES 3: the room, drawn at the render scale, brought up
// to the window with the bloom (half floats: its real values) and the
// vignette, in one opaque pass. The ball, drawn after it at the window's
// size, adds its own bloom and vignette (ball3.frag).
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
uniform sampler2D u_scene;
uniform sampler2D u_tex;  // the bloom
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    vec3 s = texture(u_scene, v_uv).rgb;
    vec3 b = texture(u_tex, v_uv).rgb * f_mode2.y;
    vec2 q = (v_uv - 0.5) * vec2(f_mode2.z, 1.0);
    float vig = 1.0 - dot(q, q) * 0.55 * f_mode2.w;
    o_col = vec4(1.0 - exp(-b) + s * vig, 1.0);
}
