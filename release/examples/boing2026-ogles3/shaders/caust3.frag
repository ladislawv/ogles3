#version 300 es
precision highp float;
// Boing 2026 for OpenGL ES 3: a triangle of the water's surface where the
// lamp's light it lets through lands on the bottom (caust3.vert): its light
// over its new area, as a share of what flat water would give. Added: where
// triangles crowd together (the light focused) the sum rises above 1, where
// they spread it falls below. dFdx and dFdy give both areas.
in vec2 v_flat;
in vec2 v_bent;
layout(location = 0) out vec4 o_col;

void main()
{
    float before = length(dFdx(v_flat)) * length(dFdy(v_flat));
    float after = length(dFdx(v_bent)) * length(dFdy(v_bent));
    o_col = vec4(vec3(min(before / max(after, 1e-7), 6.0)), 1.0);
}
