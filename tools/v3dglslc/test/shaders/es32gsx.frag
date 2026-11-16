#version 310 es
precision mediump float;
in float g_val;
out vec4 colour;
void main() { colour = vec4(g_val / 100.0, 0.0, 0.0, 1.0); }
