#version 300 es
precision mediump float;
// Boing Ocean for OpenGL ES 3: the far water, shaded at half the size,
// blown up into the scene, bilinear: soft, as out of focus behind the ball.
uniform sampler2D u_tex;
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    o_col = vec4(texture(u_tex, v_uv).rgb, 1.0);
}
