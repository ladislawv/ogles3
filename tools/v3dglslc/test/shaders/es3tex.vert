#version 300 es
// GLSL ES 3.00 samplers in the key-state draws (docs/GLES3_PLAN.md stage A
// item 2): every kind gets a texture of its own, so the variants' key
// (sampler_is_32b) is what real draws give it.
in vec4 position;
out vec3 tc;
uniform highp isampler2D vtable;
void main()
{
    tc = position.xyz + vec3(texelFetch(vtable, ivec2(0), 0).xyz);
    gl_Position = position;
}
