#version 300 es
// The billboard's light from its texture (made once by the program): red is
// the white-hot core and its star rays, green the soft glow; tinted by the
// boid's colour (red or white), whiter and brighter the faster it flies (a
// burst flashes white, then cools to red and white; dimmer by u_gain while
// the trails show), added to what is
// behind: on the screen (u_direct: with the picture's gain and vignette;
// the screen's saturating adds clamp the light) or into the glow, the
// lights alone at quarter size, for the trails and the bloom. A point
// sprite: only its inner 70% drawn.
precision mediump float;
uniform sampler2D u_flare;
uniform float u_direct;
uniform float u_gain; // dimmer while the trails show (their light added: the lights burnt white)
uniform vec2 u_res; // 1 / the screen's size
flat in float v_red;
flat in float v_bright;
flat in float v_speed;
out vec4 o_col;

void main()
{
    vec2 t = texture(u_flare, (gl_PointCoord * 2.0 - 1.0) * 0.35 + 0.5).rg * vec2(0.6, 0.06);
    vec3 base = v_red > 0.5 ? vec3(1.0, 0.15, 0.08) : vec3(0.75, 0.85, 1.0);
    base = mix(base, vec3(1.0, 0.95, 0.85), v_speed * 0.5);
    o_col = vec4((base * t.y + mix(base, vec3(1.0), 0.4) * t.x) * (v_bright * (0.8 + 0.8 * v_speed) * u_gain), 0.0);
    if (u_direct > 0.0) {
        vec2 d = gl_FragCoord.xy * u_res - 0.5;
        o_col.rgb *= 1.1 * (1.0 - 0.6 * dot(d, d));
    }
}
