#version 300 es
precision mediump float;
in float v_twice;
in vec4 v_pos;
flat in int v_id;
out vec4 colour;
void main()
{
    colour = vec4(v_twice, 0.0, float(v_id & 1), 1.0);
}
