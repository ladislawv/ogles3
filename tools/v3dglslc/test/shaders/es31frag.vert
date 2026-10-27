#version 310 es
// ES 3.1: storage buffers and atomic counters in a fragment shader
// (m8_draw's test_fragment_storage).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
