#version 310 es
// m8_draw (docs/GLES3_PLAN.md item 9): the vertices as given, es32gsa.geom passes them on (alpha to coverage, plan item 9).
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
