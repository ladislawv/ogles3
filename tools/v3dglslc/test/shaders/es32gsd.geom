#version 310 es
#extension GL_EXT_geometry_shader : require
// m8_draw (docs/GLES3_PLAN.md item 9): each triangle moved right by half the
// screen, green: what is drawn comes from the geometry stage.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
out vec4 gcol;
void main()
{
    for (int i = 0; i < 3; i++) {
        gcol = vec4(0.0, 1.0, 0.0, 1.0);
        gl_Position = gl_in[i].gl_Position + vec4(1.0, 0.0, 0.0, 0.0);
        EmitVertex();
    }
    EndPrimitive();
}
