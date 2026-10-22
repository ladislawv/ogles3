#version 300 es
// m8_draw (docs/GLES3_PLAN.md stage A item 4): gl_VertexID as a flat colour,
// red its low byte, green its high byte, so the pixels show the provoking
// (last) vertex's number.
in vec2 pos;
flat out vec4 c;
void main()
{
    c = vec4(float(gl_VertexID & 255) / 255.0, float(gl_VertexID >> 8) / 255.0, 0.0, 1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
