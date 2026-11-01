#version 300 es
precision highp float;
precision highp int;
// Boing Ocean for OpenGL ES 3: the HDR scene (floats, at the render scale)
// brought to the window with the bloom added (u_bloom, a quarter of the
// size, blurred): Narkowicz's ACES fit and gamma 2 as boingocean_fs ends,
// and a soft vignette.
layout(std140) uniform Frame {
    vec4 f_sun;           // xyz the sun's direction, w the time
    vec4 f_eye;           // xyz, w the render target's aspect
    vec4 f_fw;            // the camera: forward, right, up
    vec4 f_rt;            // w: 1 / the target's width (up: height)
    vec4 f_up;
    vec4 f_ball;          // centre, radius
    vec4 f_px;            // the ball's checkerboard axes
    vec4 f_py;
    vec4 f_pz;
    vec4 f_ambient;       // the sky's light from above
    vec4 f_mode;          // x ripples from the texture, y glints, z clouds, w caustics on the ball
    vec4 f_post;          // xy 1 / the window's size, z bloom, w the window's aspect
};
uniform sampler2D u_scene;
uniform sampler2D u_bloom;
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    vec3 c = texture(u_scene, v_uv).rgb + texture(u_bloom, v_uv).rgb * f_post.z;
    c = clamp(c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    vec2 q = (v_uv - 0.5) * vec2(f_post.w, 1.0);
    o_col = vec4(sqrt(c) * (1.0 - dot(q, q) * 0.35), 1.0);
}
