#version 310 es
// ES 3.1 explicit uniform locations: 7 and 2 used, 20 not active (its
// glUniform* ignored), the others none.
precision mediump float;
layout(location = 7) uniform vec4 tint;
layout(location = 2) uniform float scale;
layout(location = 20) uniform vec4 unused;
out vec4 colour;
void main()
{
    colour = tint * scale;
}
