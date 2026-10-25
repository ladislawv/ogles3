#version 300 es
// m8_draw's vertex formats (GLES3 plan stage B item 5): an integer array
// (its w an integer 1 when the array has fewer), an unsigned current value,
// and a float input from ES 3.0's new types.
in vec2 pos;
in ivec4 ia;
in uvec4 ub;
in vec4 hf;
flat out vec4 c;
void main()
{
    c = vec4(float(ia.x) / 255.0, float(ub.y) / 255.0, hf.x, float(ia.w));
    gl_Position = vec4(pos, 0.0, 1.0);
}
