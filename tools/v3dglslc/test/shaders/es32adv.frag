#version 310 es
#extension GL_KHR_blend_equation_advanced : require
// m8_draw (docs/GLES3_PLAN.md item 7): only multiply declared, so a draw
// with another advanced equation is GL_INVALID_OPERATION.
precision mediump float;
layout(blend_support_multiply) out;
layout(location = 0) out vec4 colour;
void main() { colour = vec4(0.5, 0.5, 0.5, 1.0); }
