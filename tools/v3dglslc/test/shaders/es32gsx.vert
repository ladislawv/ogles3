#version 310 es
// m8_draw (docs/GLES3_PLAN.md item 9): transform feedback from a geometry
// shader (es32gsx.link): each point's position to es32gsx.geom.
in vec2 pos;
out vec2 vpos;
void main() { vpos = pos; gl_Position = vec4(pos, 0.0, 1.0); }
