#version 300 es
precision mediump float;
uniform sampler2D tex;
out vec4 colour;
void main()
{
    colour = vec4(vec2(textureSize(tex, 0)) / 255.0, 0.0, 1.0);
}
