#version 300 es
// m8_draw (docs/GLES3_PLAN.md stage C item 1): transform feedback, interleaved
// (es3xfb.link): each vertex's v_twice, v_pos and v_id fed back, 6 words a
// vertex; the colour from v_twice too, so a draw that feeds back also draws.
in vec2 pos;
in float a;
out float v_twice;
out vec4 v_pos;
flat out int v_id;
void main()
{
    v_twice = a * 2.0;
    v_pos = vec4(pos, a, 1.0) * 2.0;
    v_id = gl_VertexID;
    gl_Position = vec4(pos, 0.0, 1.0);
    gl_PointSize = 1.0;
}
