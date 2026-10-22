#version 300 es
// m8_draw: a uniform block member in the fragment shader (it reads zeros
// until stage B binds buffers to blocks).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
