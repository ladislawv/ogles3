// m8_draw's EXT_frag_depth program (GLSL ES 1.00): a triangle covering
// the picture at depth 0.5.
attribute vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
