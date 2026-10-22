#version 300 es
precision mediump float;
layout(location = 0) out vec4 colour;
layout(location = 1) out uvec4 id;
void main()
{
    colour = vec4(0.0, 1.0, 0.0, 1.0);
    id = uvec4(7u);
}
