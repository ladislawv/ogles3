#version 300 es
// The floor: one square of 28 x 28 units at y = 0 (a 4-vertex strip).
uniform mat4 u_vp;
out vec2 v_xz;

void main()
{
    v_xz = (vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0) * 14.0;
    gl_Position = u_vp * vec4(v_xz.x, 0.0, v_xz.y, 1.0);
}
