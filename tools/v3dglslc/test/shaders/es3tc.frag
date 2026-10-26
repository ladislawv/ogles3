#version 300 es
precision mediump float;
uniform highp sampler2D tex;
uniform highp vec2 at;
out vec4 color;
void main()
{
    color = texture(tex, at);
}
