#version 310 es
in vec4 position;
out vec4 vcolour;
void main() { vcolour = position * 0.5 + 0.5; gl_Position = position; }
