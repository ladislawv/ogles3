#version 310 es
// m8_es31 and m8_draw (docs/GLES3_PLAN.md item 9): a program pipeline of
// three separable programs, this vertex shader, es32gsp.geom and
// es32gsp.frag (the varyings between programs by location, of one
// precision, as ES 3.1 matches them).
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
