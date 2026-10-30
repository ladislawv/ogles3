#version 300 es
precision mediump float;
in vec4 v_colour;
out vec4 colour;
void main()
{
    colour = clamp(v_colour, 0.0, 1.0);
}
