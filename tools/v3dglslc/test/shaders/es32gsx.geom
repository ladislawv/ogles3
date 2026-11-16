#version 310 es
#extension GL_EXT_geometry_shader : require
// m8_draw (docs/GLES3_PLAN.md item 9): each point a small quad, a strip of
// 4 vertices (two triangles, 6 vertices fed back); g_val the vertex's
// number from the point's primitive id * 10, g_pos the point's position.
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in vec2 vpos[];
out float g_val;
out vec2 g_pos;
void main()
{
    for (int k = 0; k < 4; k++) {
        g_val = float(gl_PrimitiveIDIn * 10 + k);
        g_pos = vpos[0];
        gl_Position = vec4(vpos[0] + vec2(float(k & 1), float(k >> 1)) * 0.1, 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
