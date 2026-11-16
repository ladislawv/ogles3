#version 310 es
precision mediump float;
in vec4 gcol;
out vec4 colour;
void main() { colour = gcol; }
