#version 300 es
precision highp float;
// Boing Ocean for OpenGL ES 3: the coarse water (8 octaves of afl_ext's
// waves), once a frame over the sea round the ball, into two half-float
// targets at once: o_height the height alone (0..1: value * depth - depth;
// 16 bits a texel, which the march reads 8 times a pixel), o_slope its
// slope as it is (no packing into 8 bits: no steps in the normal).
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
layout(location = 0) out vec4 o_height;
layout(location = 1) out vec4 o_slope;

#define DRAG_MULT 0.38
const float EXTENT = 30.0, TEXEL = 60.0 / 160.0;

void main()
{
    vec2 position = gl_FragCoord.xy * TEXEL - EXTENT;
    float len = length(position);
    float phase = len * 0.1;
    vec2 dphase = position * (0.1 / max(len, 0.0001));
    vec2 p = vec2(0.0, 1.0); // (sin, cos) of the octave's angle
    float frequency = 1.0, timeMul = 2.0, weight = 1.0;
    float sum = 0.0, weights = 0.0;
    vec2 grad = vec2(0.0);
    mat2 J = mat2(1.0);
    for (int i = 0; i < 8; i++) {
        float x = dot(p, position) * frequency + f_sun.w * timeMul + phase;
        float sx = sin(x), cx = cos(x);
        float wave = exp(sx - 1.0);
        vec2 dx = (p * J) * frequency + dphase;
        grad += dx * (wave * cx * weight);
        float k = wave * (sx - cx * cx) * weight * DRAG_MULT;
        position -= p * (wave * cx * weight * DRAG_MULT);
        J += mat2(p * (dx.x * k), p * (dx.y * k));
        sum += wave * weight;
        weights += weight;
        weight *= 0.8;
        frequency *= 1.18;
        timeMul *= 1.07;
        p = vec2(p.x * 0.625017174 + p.y * 0.780610999, p.y * 0.625017174 - p.x * 0.780610999);
    }
    o_height = vec4(sum / weights, 0.0, 0.0, 1.0);
    o_slope = vec4(grad / weights, 0.0, 1.0);
}
