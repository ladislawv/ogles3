#version 300 es
precision highp float;
// Boing 2026 for OpenGL ES 3: one step of the water in the pool (key 9), a
// float texture of 128 x 128 heights drawn from the last step's (two
// textures, swapped each frame): the wave equation, slowly damped. The
// ball pushes the water down where it touches (u_drop: where, in the
// texture, how wide, how hard). Out: x the height, y the
// height before, z and w the slope (for the floor's normal: floor3.frag).
// All on the GPU: the CPU reads nothing back.
uniform sampler2D u_tex;
uniform vec4 u_drop;
uniform float u_depth;     // the pool's depth in the middle (none at the walls)
uniform vec4 u_drip; // now and then a small drop somewhere: crossing ripples
                     // that give the caustics their web
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    vec2 t = 1.0 / vec2(textureSize(u_tex, 0));
    vec4 c = texture(u_tex, v_uv);
    float l = texture(u_tex, v_uv - vec2(t.x, 0.0)).x, r = texture(u_tex, v_uv + vec2(t.x, 0.0)).x;
    float d = texture(u_tex, v_uv - vec2(0.0, t.y)).x, u = texture(u_tex, v_uv + vec2(0.0, t.y)).x;
    // The wave equation, its speed C (0.5 would be a texel a step). Waves
    // are slower in the shallows (their speed grows with the root of the
    // depth), never quite still, so they still reach the walls.
    vec2 e = min(abs(v_uv - 0.5) * 2.0, 1.0);
    float deep = u_depth * (1.0 - e.x * e.x) * (1.0 - e.y * e.y);
    float C = mix(0.04, 0.16, clamp(sqrt(deep / 1.5), 0.0, 1.0));
    float h = (2.0 * c.x - c.y + C * (l + r + d + u - 4.0 * c.x)) * 0.998;
    // A little of the neighbours' mean mixed in: it smooths the rule's
    // finest ripple (a checkerboard of texels, all curvature and no tilt,
    // which would light caustics everywhere) and leaves the waves.
    h = mix(h, (l + r + d + u) * 0.25, 0.04);
    vec2 q = (v_uv - u_drop.xy) / u_drop.z;
    float k = dot(q, q);
    if (k < 1.0)
        h -= u_drop.w * (1.0 - k) * (1.0 - k);
    q = (v_uv - u_drip.xy) / u_drip.z;
    k = dot(q, q);
    if (k < 1.0)
        h -= u_drip.w * (1.0 - k) * (1.0 - k);
    o_col = vec4(h, c.x, (r - l) * 0.5, (u - d) * 0.5);
}
