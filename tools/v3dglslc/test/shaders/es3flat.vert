#version 300 es
// m8_draw (docs/GLES3_PLAN.md stage A items 3 and 4): a flat varying takes
// the provoking (last) vertex's colour over the whole triangle.
in vec2 pos;
in vec4 col;
flat out vec4 fcol;
void main()
{
    fcol = col;
    gl_Position = vec4(pos, 0.0, 1.0);
}
