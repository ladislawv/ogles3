#version 300 es
// m8_draw's 32-bit samplers and alpha to coverage (GLES3 plan stage A item 5).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
