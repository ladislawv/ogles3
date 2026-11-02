#version 300 es
// The trails and their bloom (blur.frag's second run: the trails blurred,
// the sharp trails added; quarter size, half floats) added onto the screen
// over the scene (as much as u_fx says), with the scene's gain and
// vignette; the screen's saturating adds clamp the light.
precision mediump float;
uniform sampler2D u_bloom;
uniform float u_fx; // 0 on the formed ball, 1 while the swarm changes shape
uniform vec2 u_res; // 1 / the screen's size
in vec2 v_uv;
out vec4 o_col;

void main()
{
    vec2 d = gl_FragCoord.xy * u_res - 0.5;
    o_col = vec4(texture(u_bloom, v_uv).rgb * (0.09 * u_fx * 1.1 * (1.0 - 0.6 * dot(d, d))), 0.0);
}
