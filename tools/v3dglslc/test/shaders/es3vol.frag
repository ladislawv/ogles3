#version 300 es
// A 3D texture at depth `z`, at the middle of its texels.
precision mediump float;
uniform mediump sampler3D tex;
uniform float z;
out vec4 colour;
void main()
{
    colour = texture(tex, vec3(0.5, 0.5, z));
}
