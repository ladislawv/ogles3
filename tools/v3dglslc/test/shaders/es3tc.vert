#version 300 es
// m8_draw's rendered textures (GLES3 plan stage B item 10): one texel, at
// the uniform's coordinates, of a texture a framebuffer object drew.
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
