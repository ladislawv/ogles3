#version 300 es
precision mediump float;
uniform Extra { vec4 add; } extra;
out vec4 colour;
void main()
{
    colour = vec4(0.25, 0.5, 0.75, 1.0) + extra.add;
}
