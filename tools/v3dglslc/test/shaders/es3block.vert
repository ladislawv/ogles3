#version 300 es
// m8_draw: a uniform block member in the fragment shader (zeros without a
// buffer bound, else the buffer range bound to its binding).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
