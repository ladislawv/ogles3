#version 300 es
// m8_draw's instanced draws (GLES3 plan stage B item 4): instance i moves
// right by i, its colour an instanced attribute's.
in vec2 pos;
in vec4 col;
flat out vec4 c;
void main()
{
    c = col;
    gl_Position = vec4(pos.x + float(gl_InstanceID), pos.y, 0.0, 1.0);
}
