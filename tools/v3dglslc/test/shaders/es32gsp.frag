#version 310 es
precision mediump float;
layout(location = 0) in highp vec4 gcol;
out vec4 colour;
void main() { colour = gcol; }
