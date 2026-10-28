#version 310 es
// ES 3.1 multisample textures: samples 0 and 3 of a 4x texture's texel
// (2, 2), averaged; textureSize's width in alpha (16: 1.0).
precision mediump float;
uniform mediump sampler2DMS ms;
out vec4 colour;
void main()
{
    vec4 a = texelFetch(ms, ivec2(2, 2), 0), b = texelFetch(ms, ivec2(2, 2), 3);
    colour = vec4(((a + b) * 0.5).rgb, float(textureSize(ms).x) / 16.0);
}
