#version 300 es
precision mediump float;
// Boing Ocean for OpenGL ES 3: one direction of a 9-tap Gaussian, in 5
// bilinear reads, over the bloom's floats.
uniform sampler2D u_tex;
uniform vec2 u_dir; // one texel along the blur, times the spread
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    vec4 c = texture(u_tex, v_uv) * 0.2270270;
    c += (texture(u_tex, v_uv + u_dir * 1.3846154) + texture(u_tex, v_uv - u_dir * 1.3846154)) * 0.3162162;
    c += (texture(u_tex, v_uv + u_dir * 3.2307692) + texture(u_tex, v_uv - u_dir * 3.2307692)) * 0.0702703;
    o_col = c;
}
