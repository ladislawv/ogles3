#version 300 es
// m8_draw: textureSize (QUNIFORM_TEXTURE_WIDTH and _HEIGHT), written as a
// colour: red the width, green the height, in units of 1/255.
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
