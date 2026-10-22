#version 300 es
// GLSL ES 3.00 (docs/GLES3_PLAN.md stage A): an explicit location, an integer
// and a non-square matrix input, two uniform blocks, a flat output,
// gl_VertexID. Written in format 2 (v3dp/FORMAT.md).
layout(location = 3) in vec4 position;
in ivec2 index;
in mat2x3 skew;
uniform Light { vec4 colour; mat4 m; } light;
uniform Transform { mat4 mvp; float scale; };
flat out int vindex;
out vec3 v;
void main()
{
    gl_Position = mvp * position * scale;
    vindex = index.x + gl_VertexID;
    v = skew * vec2(1.0) + light.colour.xyz;
}
