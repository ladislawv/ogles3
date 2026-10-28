#version 310 es
// ES 3.1 multisample textures (m8_draw's test_ms_texture).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
