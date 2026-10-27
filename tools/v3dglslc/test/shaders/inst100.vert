// m8_draw's EXT_instanced_arrays program (GLSL ES 1.00): the colour an
// instanced attribute's, the triangle the same for every instance.
attribute vec2 pos;
attribute vec4 col;
varying vec4 c;
void main()
{
    c = col;
    gl_Position = vec4(pos, 0.0, 1.0);
}
