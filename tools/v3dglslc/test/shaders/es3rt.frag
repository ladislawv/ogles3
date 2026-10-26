#version 300 es
precision highp float;
uniform vec4 f;
uniform ivec4 i;
uniform uvec4 u;
layout(location = 0) out vec4 of;
layout(location = 1) out ivec4 oi;
layout(location = 2) out uvec4 ou;
void main()
{
    of = f;
    oi = i;
    ou = u;
}
