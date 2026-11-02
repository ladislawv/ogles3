// The stats' text: squares in pixels (from a client array), each a letter
// of the program's 5x7 font texture.
attribute vec2 a_pos;
attribute vec2 a_uv;
uniform vec2 u_screen; // 2 / width, 2 / height
varying vec2 v_uv;

void main()
{
    v_uv = a_uv;
    gl_Position = vec4(a_pos * u_screen - 1.0, 0.0, 1.0);
}
