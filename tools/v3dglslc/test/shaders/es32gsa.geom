#version 310 es
#extension GL_EXT_geometry_shader : require
// m8_draw (docs/GLES3_PLAN.md item 9): each triangle as it is, its colour
// (0.25, 0.5, 0.75) with alpha 0.5 from the geometry stage: alpha to
// coverage in a 4x MSAA context covers half the samples.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
out vec4 gcol;
void main()
{
    for (int i = 0; i < 3; i++) {
        gcol = vec4(0.25, 0.5, 0.75, 0.5);
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
