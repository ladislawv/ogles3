#version 300 es
precision mediump float;
// Boing 2026 for OpenGL ES 3: the shadow maps' depths as grey (M). The
// texture compares when read through a shadow sampler; here a sampler object
// bound to its unit turns the comparison off.
uniform highp sampler2D u_map;
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    o_col = vec4(vec3(texture(u_map, v_uv).r), 1.0);
}
