#version 300 es
// Layer `layer` of a 2D array, at the middle of its texels.
precision mediump float;
uniform mediump sampler2DArray tex;
uniform float layer;
out vec4 colour;
void main()
{
    colour = texture(tex, vec3(0.5, 0.5, layer));
}
