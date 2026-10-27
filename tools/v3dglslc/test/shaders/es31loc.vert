#version 310 es
// ES 3.1 explicit uniform locations (m8_draw's test_uniform_locations).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
