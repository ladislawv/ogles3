#version 300 es
// An unsigned integer texel, each component / 255.
precision mediump float;
uniform highp usampler2D tex;
out vec4 colour;
void main()
{
    colour = vec4(texture(tex, vec2(0.5))) / 255.0;
}
