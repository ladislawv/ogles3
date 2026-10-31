#version 300 es
precision mediump float;
// Boing 2026 for OpenGL ES 3: a triangle of the water's surface where the
// light it reflects lands on a wall or the ceiling (refl3.vert): its light
// over its new area, as a share of what flat water would give there, worked
// out for the whole triangle there. Added.
flat in float v_light;
layout(location = 0) out vec4 o_col;

void main()
{
    o_col = vec4(vec3(v_light), 1.0);
}
