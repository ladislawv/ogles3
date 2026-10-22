#version 300 es
// m8_draw (docs/GLES3_PLAN.md stage A item 5): a program whose precompiled
// fragment variants were drawn into a float and an unsigned target (its
// outputs' types), drawn into the RGBA8 default framebuffer: the draw needs
// a variant of another key, compiled at the draw or found in its .v3dx.
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
