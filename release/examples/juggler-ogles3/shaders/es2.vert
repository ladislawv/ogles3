// The ES 2.0 way's full-screen triangle (a vertex buffer: no gl_VertexID).
attribute vec2 a_pos;
varying vec2 v_uv;

void main()
{
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
