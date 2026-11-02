#version 300 es
// The billboard's light from its texture (made once by the program): red is
// the white-hot core and its star rays, green the soft glow; tinted by the
// boid's colour (red or white), added to what is behind.
precision mediump float;
uniform sampler2D u_flare;
in vec2 v_uv;
flat in float v_red;
flat in float v_bright;
out vec4 o_col;

void main()
{
    vec2 t = texture(u_flare, v_uv * 0.5 + 0.5).rg * vec2(0.6, 0.06);
    vec3 base = v_red > 0.5 ? vec3(1.0, 0.15, 0.08) : vec3(0.75, 0.85, 1.0);
    o_col = vec4((base * t.y + mix(base, vec3(1.0), 0.4) * t.x) * v_bright * 0.8, 0.0);
}
