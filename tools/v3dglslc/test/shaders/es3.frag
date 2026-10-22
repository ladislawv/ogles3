#version 300 es
// The fragment shader reads Fog and Light, the vertex shader Light and
// Transform: the program's blocks are Light, Transform, Fog, and this stage's
// map is Fog -> 2, Light -> 0 or Light -> 0, Fog -> 2 (FORMAT.md,
// UNIFORM_BLOCKS). Two outputs with explicit locations, one unsigned.
precision mediump float;
uniform Fog { vec4 fog; };
uniform Light { vec4 colour; mat4 m; } light;
flat in int vindex;
in vec3 v;
layout(location = 0) out vec4 colour;
layout(location = 1) out uvec4 id;
void main()
{
    colour = vec4(v, 1.0) * light.colour + fog;
    id = uvec4(uint(vindex));
}
