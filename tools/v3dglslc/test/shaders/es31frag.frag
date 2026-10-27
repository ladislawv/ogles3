#version 310 es
// ES 3.1: each fragment counts itself with an atomic counter and stores the
// count in its pixel's word of a storage buffer (64 pixels a row).
precision highp float;
layout(binding = 1) uniform atomic_uint frags;
layout(std430, binding = 2) buffer Px { uint px[]; };
out vec4 colour;
void main()
{
    uint n = atomicCounterIncrement(frags);
    px[uint(gl_FragCoord.y) * 64u + uint(gl_FragCoord.x)] = n + 1u;
    colour = vec4(0.0, 1.0, 0.0, 1.0);
}
