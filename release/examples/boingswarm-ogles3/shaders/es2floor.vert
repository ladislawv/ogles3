// The ES 2.0 way's floor: 28 x 28 units at y = 0, from a client array.
attribute vec2 a_pos;
uniform mat4 u_vp;
varying vec2 v_xz;

void main()
{
    v_xz = a_pos;
    gl_Position = u_vp * vec4(a_pos.x, 0.0, a_pos.y, 1.0);
}
