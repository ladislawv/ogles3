#version 310 es
// m8_draw (docs/GLES3_PLAN.md item 7): KHR_blend_equation_advanced's multiply.
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
