#version 300 es
precision mediump float;
// Boing 2026 for OpenGL ES 3: the room's bright part (half floats, the room
// target's size) brought down to a quarter: four bilinear reads, one texel
// either side of the centre, average a 4x4 block. The texel size comes from
// textureSize: no uniform.
uniform sampler2D u_tex;
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    vec2 t = 1.0 / vec2(textureSize(u_tex, 0));
    vec4 c = texture(u_tex, v_uv + vec2(-t.x, -t.y)) + texture(u_tex, v_uv + vec2(t.x, -t.y));
    c += texture(u_tex, v_uv + vec2(-t.x, t.y)) + texture(u_tex, v_uv + vec2(t.x, t.y));
    o_col = c * 0.25;
}
