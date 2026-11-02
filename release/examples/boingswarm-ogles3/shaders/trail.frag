#version 300 es
// The trails, at half size: this frame's glow (the lights alone) added to
// the trails a frame before, faded by u_decay: fast boids leave streaks.
precision mediump float;
uniform sampler2D u_glow;
uniform sampler2D u_prev;
uniform float u_decay;
in vec2 v_uv;
out vec4 o_col;

void main()
{
    o_col = vec4(texture(u_glow, v_uv).rgb + texture(u_prev, v_uv).rgb * u_decay, 1.0);
}
