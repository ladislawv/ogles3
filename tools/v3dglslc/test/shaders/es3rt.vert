#version 300 es
// m8_draw's render targets beyond 8 bits (GLES3 plan stage B item 10): its
// uniforms' values written to a float, a signed and an unsigned integer
// output, at depth z.
in vec2 pos;
uniform float z;
void main()
{
    gl_Position = vec4(pos, z, 1.0);
}
