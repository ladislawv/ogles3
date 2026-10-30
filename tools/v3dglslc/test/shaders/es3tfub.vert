#version 300 es
// m8_draw (docs/GLES3_PLAN.md, the remaining gaps, item 2): a uniform block
// read by the vertex shader, bound to a buffer transform feedback wrote
// earlier in the same job: the draw waits for the feedback (draw.c,
// xfb_wait). The colour is the block's second vec4.
in vec2 pos;
uniform Fed { vec4 pad; vec4 c; } fed;
out vec4 v_colour;
void main()
{
    v_colour = fed.c;
    gl_Position = vec4(pos, 0.0, 1.0);
}
