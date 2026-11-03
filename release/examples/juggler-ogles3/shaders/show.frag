#version 300 es
// The picture onto the screen: the HAM picture (nearest, its pixels kept)
// or the light added up (its 15/16 is full white, as rt2.c's ham() had it).
precision mediump float;
uniform sampler2D u_img;
uniform float u_gain;
in vec2 v_uv;
out vec4 o_col;

void main()
{
    o_col = vec4(min(texture(u_img, v_uv).rgb * u_gain, vec3(1.0)), 1.0);
}
