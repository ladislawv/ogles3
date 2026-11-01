#version 300 es
precision highp float;
// Boing Ocean for OpenGL ES 3: caustics, once a frame into a small tiling
// float texture (R16F, 128 x 128): the light the ripples focus into a moving
// web of thin bright lines, as on a pool's floor. The pattern is the
// well-known tileable water caustic (five rounds of a turbulent warp of the
// point, the light summed where the warp crowds it); the ball's side under
// the water reads it (ocean3.frag).
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
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

const float TAU = 6.2831853;

void main()
{
    float time = f_sun.w * 0.5 + 23.0;
    vec2 p = mod(v_uv * TAU, TAU) - 250.0; // whole turns across the tile: it repeats
    vec2 i = p;
    float c = 1.0, inten = 0.005;
    for (int n = 0; n < 5; n++) {
        float t = time * (1.0 - 3.5 / float(n + 1));
        i = p + vec2(cos(t - i.x) + sin(t + i.y), sin(t - i.y) + cos(t + i.x));
        c += 1.0 / length(vec2(p.x / (sin(i.x + t) / inten), p.y / (cos(i.y + t) / inten)));
    }
    c /= 5.0;
    c = 1.17 - pow(c, 1.4);
    o_col = vec4(pow(abs(c), 8.0), 0.0, 0.0, 1.0);
}
