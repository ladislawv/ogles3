#version 300 es
// The floor: dark checks of 1.5 units with faint violet lines between them
// (the 1984 demo's grid, at night), fading into the dark beyond 9 units,
// made once by the program (make_floor) into u_floor; straight to the
// screen with the picture's gain and vignette.
precision mediump float;
uniform sampler2D u_floor;
uniform vec2 u_res; // 1 / the screen's size
in vec2 v_xz;
out vec4 o_col;

void main()
{
    vec2 d = gl_FragCoord.xy * u_res - 0.5;
    o_col = vec4(texture(u_floor, v_xz / 28.0 + 0.5).rgb * (1.1 * (1.0 - 0.6 * dot(d, d))), 1.0);
}
