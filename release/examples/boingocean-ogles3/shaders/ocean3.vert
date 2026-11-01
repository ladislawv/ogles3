#version 300 es
// Boing Ocean for OpenGL ES 3: the view ray for each corner of one triangle
// over the target, from the frame's uniform block and gl_VertexID.
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
out vec3 v_ray;

void main()
{
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    v_ray = f_fw.xyz * 1.5 + f_rt.xyz * (p.x * 0.5 * f_eye.w) + f_up.xyz * (p.y * 0.5);
    gl_Position = vec4(p, 0.0, 1.0);
}
