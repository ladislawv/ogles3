#version 300 es
// The bloom, at quarter size: a 9-tap Gaussian along u_dir (one texel of
// the source, across or down), run twice; the second run adds the sharp
// trails (u_add, u_addw), so the screen reads both in one.
precision mediump float;
uniform sampler2D u_src;
uniform sampler2D u_add;
uniform float u_addw;
uniform vec2 u_dir;
in vec2 v_uv;
out vec4 o_col;

void main()
{
    vec3 c = texture(u_src, v_uv).rgb * 0.2270;
    c += (texture(u_src, v_uv + u_dir * 1.3846).rgb + texture(u_src, v_uv - u_dir * 1.3846).rgb) * 0.3162;
    c += (texture(u_src, v_uv + u_dir * 3.2308).rgb + texture(u_src, v_uv - u_dir * 3.2308).rgb) * 0.0703;
    if (u_addw > 0.0)
        c += texture(u_add, v_uv).rgb * u_addw;
    o_col = vec4(c, 1.0);
}
