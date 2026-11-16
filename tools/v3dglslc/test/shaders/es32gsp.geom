#version 310 es
#extension GL_EXT_geometry_shader : require
// The pipeline's geometry program: each triangle moved right by half the
// screen, its colour a uniform of this program's (green in m8_draw).
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
uniform highp vec4 gtint;
layout(location = 0) out highp vec4 gcol;
void main()
{
    for (int i = 0; i < 3; i++) {
        gcol = gtint;
        gl_Position = gl_in[i].gl_Position + vec4(1.0, 0.0, 0.0, 0.0);
        EmitVertex();
    }
    EndPrimitive();
}
