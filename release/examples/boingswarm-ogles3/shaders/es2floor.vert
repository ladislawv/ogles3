// The ES 2.0 way's floor: 28 x 28 units at y = 0, from a client array.
attribute vec2 a_xz;
uniform mat4 u_vp;
varying vec2 v_xz;

void main()
{
    v_xz = a_xz;
    gl_Position = u_vp * vec4(a_xz.x, 0.0, a_xz.y, 1.0);
}
