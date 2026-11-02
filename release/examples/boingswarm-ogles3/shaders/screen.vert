#version 300 es
// One triangle over the whole target (gl_VertexID 0..2); v_uv 0..1 on it.
out vec2 v_uv;

void main()
{
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 4.0 - 1.0;
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
