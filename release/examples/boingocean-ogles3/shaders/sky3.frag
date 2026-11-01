#version 300 es
precision highp float;
// Boing Ocean for OpenGL ES 3: one face (u_face) of the sky as a half-float
// cube map, drawn when the sun has moved (every 16th frame): afl_ext's
// atmosphere towards each texel's direction, the sun's glow, and clouds
// drifting through a 3D noise texture (sampler3D), lit by the sun. Every
// reflection then reads it once: no table to decode, no sun to work out.
// Below the horizon, the far sea's colour.
layout(std140) uniform Frame {
    vec4 f_sun;           // xyz the sun's direction, w the time
    vec4 f_eye;           // xyz, w the render target's aspect
    vec4 f_fw;            // the camera: forward, right, up
    vec4 f_rt;            // w: 1 / the target's width (up: height)
    vec4 f_up;
    vec4 f_ball;          // centre, radius
    vec4 f_px;            // the ball's checkerboard axes
    vec4 f_py;
    vec4 f_pz;
    vec4 f_ambient;       // the sky's light from above
    vec4 f_mode;          // x ripples from the texture, y glints, z clouds, w caustics on the ball
    vec4 f_post;          // xy 1 / the window's size, z bloom, w the window's aspect
};
uniform int u_face;
uniform highp sampler3D u_noise;
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

// The direction through this texel of face u_face (as GL lays a cube out).
vec3 face_dir(vec2 uv)
{
    vec2 c = uv * 2.0 - 1.0;
    if (u_face == 0) return vec3(1.0, -c.y, -c.x);
    if (u_face == 1) return vec3(-1.0, -c.y, c.x);
    if (u_face == 2) return vec3(c.x, 1.0, c.y);
    if (u_face == 3) return vec3(c.x, -1.0, -c.y);
    if (u_face == 4) return vec3(c.x, -c.y, 1.0);
    return vec3(-c.x, -c.y, -1.0);
}

vec3 atmosphere(vec3 dir)
{
    vec3 sun = f_sun.xyz;
    float y = max(dir.y, 0.0), d = dot(dir, sun);
    float st = 1.0 / (y + 0.1);
    float st2 = 1.0 / (sun.y * 11.0 + 1.0);
    float raysundt = d * d;
    float sundt = pow(max(0.0, d), 8.0);
    float mymie = sundt * st * 0.2;
    vec3 suncolor = mix(vec3(1.0), max(vec3(0.0), vec3(1.0) - vec3(5.5, 13.0, 22.4) / 22.4), st2);
    vec3 bluesky = vec3(5.5, 13.0, 22.4) / 22.4 * suncolor;
    vec3 bluesky2 = max(vec3(0.0), bluesky - vec3(5.5, 13.0, 22.4) * 0.002 * (st - 6.0 * sun.y * sun.y));
    bluesky2 *= st * (0.24 + raysundt * 0.24);
    return (bluesky2 * (1.0 + pow(1.0 - y, 3.0)) + mymie * suncolor) * 0.5;
}

void main()
{
    vec3 dir = normalize(face_dir(v_uv));
    vec3 c;
    if (dir.y < 0.0) {
        // The far sea, as the ball sees it below the horizon.
        c = mix(vec3(0.004, 0.012, 0.025), atmosphere(normalize(vec3(dir.x, 0.02, dir.z))) * 0.6, exp(dir.y * 8.0));
    } else {
        c = atmosphere(dir);
        // Clouds on a layer far above, large, their place on it scrolled by
        // the wind: three octaves of the noise; dense where they pass a
        // threshold, lit by the sun from above and through their edges.
        if (f_mode.z > 0.5 && dir.y > 0.02) {
            vec2 p = dir.xz / dir.y * 0.04 + vec2(f_sun.w * 0.0005, f_sun.w * 0.0002);
            float t = f_sun.w * 0.003;
            float n = texture(u_noise, vec3(p, t)).r * 0.55 + texture(u_noise, vec3(p * 2.1, t * 1.7)).r * 0.3 +
                      texture(u_noise, vec3(p * 4.3, t * 2.9)).r * 0.15;
            float cover = smoothstep(0.48, 0.72, n) * smoothstep(0.02, 0.2, dir.y);
            float lit = 0.55 + 0.45 * clamp(f_sun.y * 1.5, 0.0, 1.0);
            vec3 cloud = mix(vec3(0.35, 0.37, 0.42), vec3(1.05, 1.0, 0.92), lit) * (0.6 + 0.6 * f_sun.y) +
                         vec3(1.2, 0.9, 0.6) * pow(max(dot(dir, f_sun.xyz), 0.0), 6.0) * (1.0 - cover) * 0.8;
            c = mix(c, cloud, cover * 0.9);
        }
        // The sun's glow, soft enough for the cube's texels; its sharp disc
        // is drawn where the eye sees it (ocean3.frag).
        c += vec3(1.0, 0.95, 0.85) * exp((dot(dir, f_sun.xyz) - 1.0) * 700.0) * 40.0;
    }
    o_col = vec4(c, 1.0);
}
