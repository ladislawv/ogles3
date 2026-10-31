#version 300 es
precision mediump float;
precision highp int;
// Boing 2026 for OpenGL ES 3: a spark's streak, added. In the window its
// light is brought into range (1 - exp); into the reflection (half floats)
// at half (the floor is no mirror); into the bloom (a fifth of them drawn
// there) 0.4, a glow round the streaks.
uniform int u_out;
in float v_across;
flat in vec3 v_col;
layout(location = 0) out vec4 o_col;

void main()
{
    vec3 c = v_col * (1.0 - v_across * v_across);
    o_col = vec4(u_out == 0 ? 1.0 - exp(-c) : c * (u_out == 1 ? 0.4 : 0.5), 0.0);
}
