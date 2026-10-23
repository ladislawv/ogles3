#version 300 es
// m8-1's refused file (GLES3 plan stage A item 7): a 1.x compiler's format
// 1.0 file of this program is put into the cache; the library must refuse it
// and compile a format 2 one in its place.
in vec2 pos;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
}
