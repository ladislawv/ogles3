#version 300 es
precision mediump float;
// Boing Ocean for OpenGL ES 3: the scene's part brighter than white (the
// sun, its glints on the water, the ball's highlight), brought down to a
// quarter: four bilinear reads, one texel either side of the centre, each
// a 2x2 average. The texel size from textureSize: no uniform.
uniform sampler2D u_tex;
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    vec2 t = 1.0 / vec2(textureSize(u_tex, 0));
    vec3 c = texture(u_tex, v_uv + vec2(-t.x, -t.y)).rgb + texture(u_tex, v_uv + vec2(t.x, -t.y)).rgb +
             texture(u_tex, v_uv + vec2(-t.x, t.y)).rgb + texture(u_tex, v_uv + vec2(t.x, t.y)).rgb;
    o_col = vec4(min(max(c * 0.25 - 1.0, 0.0), 8.0), 1.0);
}
