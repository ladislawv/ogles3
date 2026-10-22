#version 300 es
precision mediump float;
in vec3 v;
uniform bool flag;
out vec4 colour;
void main()
{
    colour = vec4(v, flag ? 1.0 : 0.0);
}
