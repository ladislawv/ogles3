#version 300 es
// A signed integer texel's magnitudes, each / 255.
precision mediump float;
uniform highp isampler2D tex;
out vec4 colour;
void main()
{
    colour = vec4(abs(texture(tex, vec2(0.5)))) / 255.0;
}
