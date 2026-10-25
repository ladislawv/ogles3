#version 300 es
// m8_draw's 2D array textures (GLES3 plan stage B item 8).
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
