#version 300 es
// One sampler of each GLSL ES 3.00 kind but those es3tex.vert and the GLSL ES
// 1.00 tests have: 3D, 2D array, the three shadow ones, unsigned cube.
precision mediump float;
uniform mediump sampler3D volume;
uniform mediump sampler2DArray layers;
uniform mediump sampler2DShadow shadow;
uniform mediump samplerCubeShadow cube_shadow;
uniform mediump sampler2DArrayShadow layer_shadow;
uniform highp usamplerCube ids;
in vec3 tc;
out vec4 colour;
void main()
{
    float s = texture(shadow, tc) + texture(cube_shadow, vec4(tc, 0.5)) + texture(layer_shadow, vec4(tc, 0.5));
    colour = texture(volume, tc) + texture(layers, tc) * s + vec4(texture(ids, tc));
}
