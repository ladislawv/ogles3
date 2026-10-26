#extension GL_EXT_frag_depth : require
// m8_draw's EXT_frag_depth program: colour `c`, depth `z` written.
precision mediump float;
uniform vec4 c;
uniform float z;
void main()
{
    gl_FragColor = c;
    gl_FragDepthEXT = z;
}
