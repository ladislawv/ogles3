#version 310 es
precision mediump float;
in vec4 gcolour;
uniform vec4 tint;
out vec4 colour;
void main() { colour = gcolour * tint; }
