#version 310 es
#extension GL_EXT_geometry_shader : require
layout(triangles, invocations = 2) in;
layout(triangle_strip, max_vertices = 3) out;
in vec4 vcolour[];
out vec4 gcolour;
uniform float gscale;
void main()
{
    for (int i = 0; i < 3; i++) {
        gcolour = vcolour[i] * gscale;
        gl_Layer = gl_InvocationID;
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
