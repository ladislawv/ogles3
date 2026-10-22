#version 300 es
precision mediump float;
flat in vec4 fcol;
out vec4 colour;
void main()
{
    colour = fcol;
}
