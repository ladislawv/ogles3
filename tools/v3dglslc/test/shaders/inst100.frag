// m8_draw's EXT_instanced_arrays program: the instance's colour.
precision mediump float;
varying vec4 c;
void main()
{
    gl_FragColor = c;
}
