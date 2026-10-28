#version 310 es
// ES 3.1 explicit uniform locations: 7 and 2 used, 20 not active (its
// glUniform* ignored), the others none; a sampler at unit 3 by
// layout(binding).
precision mediump float;
layout(location = 7) uniform vec4 tint;
layout(location = 2) uniform float scale;
layout(location = 20) uniform vec4 unused;
layout(binding = 3) uniform mediump sampler2D tex;
out vec4 colour;
void main()
{
    colour = tint * scale + vec4(texture(tex, vec2(0.5)).rgb, 0.0); /* unbound: (0, 0, 0, 1) */
}
