#version 310 es
#extension GL_EXT_geometry_shader : require
// m8_draw (docs/GLES3_PLAN.md item 9): each triangle moved right by half the
// screen, its colour a texel the geometry stage reads: with a float texture
// a draw-time variant of 32-bit samplers in the geometry stage.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
uniform highp sampler2D tex;
out vec4 gcol;
void main()
{
    vec4 c = texelFetch(tex, ivec2(0, 0), 0);
    for (int i = 0; i < 3; i++) {
        gcol = c;
        gl_Position = gl_in[i].gl_Position + vec4(1.0, 0.0, 0.0, 0.0);
        EmitVertex();
    }
    EndPrimitive();
}
