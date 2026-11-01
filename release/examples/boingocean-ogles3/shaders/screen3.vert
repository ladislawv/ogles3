#version 300 es
// Boing Ocean for OpenGL ES 3: one triangle over the whole target, made from
// gl_VertexID: no vertex buffer.
out vec2 v_uv;

void main()
{
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
