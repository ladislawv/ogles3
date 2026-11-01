#version 300 es
precision highp float;
// Boing Ocean for OpenGL ES 3: the scene, as boingocean_fs draws it. u_out 0:
// tone mapped as boingocean_fs ends, with a soft vignette, straight into
// the window (no bloom, at 1/1); 1: linear, as floats, into the scene's
// HDR target (final3.frag tone maps it, the bloom added).
//   - the water: the half-float height texture (textureLod in the march: no
//     implicit level of detail in a loop whose rays diverge), its slope as
//     it is, the ripples from a small tiling texture drawn once a frame
//     (ripple3.frag: two reads for the three sines a pixel), the sun's glint
//     on each ripple kept bright for the bloom (a sparkling road);
//   - the sky: one read of the sky cube (clouds in it) for every
//     reflection;
//   - the ball: its checkerboard from a cube map read with the ball's own
//     direction (mipmapped: no atan/asin per pixel, no crawling edges);
//     under the water, the ripples' caustics playing on it.
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
uniform highp sampler2D u_height;   // the coarse water's height (height3.frag)
uniform highp sampler2D u_slope;    // its slope (xy)
uniform mediump sampler2D u_ripple; // the ripples' slope, tiling (ripple3.frag)
uniform mediump sampler2D u_caust;  // the caustics, tiling (caust3.frag)
uniform highp samplerCube u_sky;    // the sky (sky3.frag)
uniform mediump samplerCube u_checker; // the checkerboard, by the ball's direction
uniform int u_out;
in vec3 v_ray;
layout(location = 0) out vec4 o_col;

#define WATER_DEPTH 1.0
#define WAVE_SLOPE 0.5
#define EXTENT 30.0
#define SAND_Y -2.2   /* the sandbank under the ball: its depth, its reach */
#define SAND_TOP 0.34 /* = boingocean-ogles3.c's: the sand's band, the screen's bottom third */
#define SAND_R 3.2   /* = NEAR_R in boingocean-ogles3.c: the full shader's reach */

float seaHeight(vec2 xz)
{
    return textureLod(u_height, xz * (0.5 / EXTENT) + 0.5, 0.0).r * WATER_DEPTH - WATER_DEPTH;
}

// As boingocean_fs: the slope-bounded march over the height texture.
float raymarchwater(vec3 ray, float t)
{
    float k = 1.0 / (WAVE_SLOPE - ray.y);
    for (int i = 0; i < 8; i++) {
        vec3 pos = f_eye.xyz + ray * t;
        if (abs(pos.x) > EXTENT || abs(pos.z) > EXTENT)
            return t;
        float gap = pos.y - seaHeight(pos.xz);
        if (gap < 0.01 + t * 0.002)
            return t;
        t += gap * k;
    }
    return t;
}

vec3 seaNormal(vec2 xz, vec2 coarse, float dist)
{
    vec2 grad = coarse;
    float fade = clamp(1.0 - dist * 0.06, 0.0, 1.0);
    if (fade > 0.0 && f_mode.x > 0.5) {
        // Two reads of the ripples' tile at two scales, the second turned
        // and drifting the other way: finer, many more ripples than the
        // three sines, for less.
        grad += texture(u_ripple, xz * 0.25).rg * fade;
        // The second read at a scale and angle unrelated to the first's
        // (the golden ratio, 37 degrees) and drifting another way, so the
        // two tiles hardly ever line up alike: no grid of repeats.
        grad += mat2(0.8, -0.6, 0.6, 0.8) * texture(u_ripple, mat2(0.8, 0.6, -0.6, 0.8) * xz * 0.4045 + vec2(f_sun.w * 0.031, -f_sun.w * 0.047)).rg * (0.6 * fade);
        // A third, finer still (about 4 times the first's scale, at
        // another angle), fading out sooner: the small chop near the eye.
        grad += mat2(0.28, -0.96, 0.96, 0.28) * texture(u_ripple, mat2(0.28, 0.96, -0.96, 0.28) * xz * 0.97 + vec2(-f_sun.w * 0.06, f_sun.w * 0.02)).rg * (0.35 * fade * fade);
    } else if (fade > 0.0) {
        vec2 p = vec2(0.6, 0.8);
        float frequency = 3.8, amp = 0.03 * fade;
        for (int i = 0; i < 3; i++) {
            float x = dot(p, xz) * frequency + f_sun.w * (frequency * 0.9 + 2.0);
            grad += p * (cos(x) * frequency * amp);
            frequency *= 1.5;
            amp *= 0.7;
            p = vec2(p.x * 0.625017174 + p.y * 0.780610999, p.y * 0.625017174 - p.x * 0.780610999);
        }
    }
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

vec3 sky(vec3 dir)
{
    return texture(u_sky, dir).rgb;
}

// The sun's sharp disc, where the eye sees it (the cube holds its glow).
float sunDisc(vec3 dir)
{
    float d = dot(dir, f_sun.xyz);
    return d > 0.9995 ? 60.0 * smoothstep(0.9995, 0.99985, d) : 0.0;
}

// The caustics at a point under the water (world x, z): how much of the
// sunlight reaches it, the ripples having focused or spread it: two crossing
// reads of the web (caust3.frag), finer than the tile, the brighter of the
// two, sharpened. A tenth where the light is spread, up to 4 on the lines.
float caustics(vec2 p)
{
    float a = texture(u_caust, p * 0.7).r;
    float b = texture(u_caust, mat2(0.8, 0.6, -0.6, 0.8) * p * 0.53 + 0.37).r;
    return 0.1 + 3.9 * pow(max(a, b), 2.0);
}

float hitBall(vec3 ro, vec3 rd)
{
    vec3 oc = ro - f_ball.xyz;
    float b = dot(oc, rd);
    float disc = b * b - (dot(oc, oc) - f_ball.w * f_ball.w);
    if (disc < 0.0)
        return -1.0;
    float t = -b - sqrt(disc);
    return t > 0.0 ? t : -1.0;
}

vec3 shadeBall(vec3 p, vec3 rd)
{
    vec3 n = (p - f_ball.xyz) * (1.0 / f_ball.w);
    vec3 q = vec3(dot(n, f_px.xyz), dot(n, f_py.xyz), dot(n, f_pz.xyz));
    // The texture holds the distance to the squares' edges: cut at 0.5,
    // smoothed over a pixel (fwidth), sharp edges at any size.
    float e = texture(u_checker, q).r - 0.5;
    float w = max(fwidth(e), 0.002);
    vec3 albedo = mix(vec3(0.85), vec3(0.80, 0.03, 0.02), smoothstep(-w, w, e));
    float diff = max(dot(n, f_sun.xyz), 0.0);
    vec3 col = albedo * (diff * vec3(0.9, 0.855, 0.765) + f_ambient.rgb + vec3(0.02, 0.05, 0.06) * max(-n.y, 0.0));
    vec3 h = normalize(f_sun.xyz - rd);
    col += pow(max(dot(n, h), 0.0), 120.0) * 1.5;
    vec3 env = sky(reflect(rd, n));
    float f1 = 1.0 - max(dot(n, -rd), 0.0), f2 = f1 * f1;
    float fres = 0.08 + 0.92 * f2 * f2 * f1;
    return mix(col * 1.44, env, fres); // 44% brighter than boingocean_fs
}

vec3 shadeWater(vec3 origin, vec3 ray, float dist)
{
    vec3 hitPos = origin + ray * dist;
    vec4 w = textureLod(u_slope, hitPos.xz * (0.5 / EXTENT) + 0.5, 0.0);
    vec3 N = seaNormal(hitPos.xz, w.xy * WATER_DEPTH, dist);
    float flat_ = 0.8 * min(1.0, sqrt(dist * 0.01) * 1.1);
    N = mix(N, vec3(0.0, 1.0, 0.0), flat_);
    float f1 = 1.0 - max(0.0, dot(-N, ray)), f2 = f1 * f1;
    float fresnel = 0.04 + 0.96 * f2 * f2 * f1;
    vec3 R = normalize(reflect(ray, N));
    R.y = abs(R.y);
    vec3 reflection = sky(R);
    // The sun's glint on every ripple, kept above white for the bloom.
    if (f_mode.y > 0.5)
    {
        // The sun in the water as boingocean_fs has it (afl_ext's sun:
        // its disc and glare in the reflected ray), kept above white for
        // the bloom: the road of sparkle towards the sun.
        float d = dot(R, f_sun.xyz);
        if (d > 0.985)
            reflection += vec3(pow(d, 720.0) * 210.0);
    }
    vec3 body = vec3(0.0293, 0.0698, 0.1717) * 0.1 * (0.2 + (hitPos.y + WATER_DEPTH) / WATER_DEPTH);
    float r = length(hitPos.xz - f_ball.xz);
#ifndef FAR // (oceanfar3.frag: this file with FAR defined, for the water away from
            // the ball: none of the ball's and the sand's work, fewer registers)
    // A sandbank of white sand under the ball, SAND_Y below, fading into the
    // deep round it: seen through the water along the refracted ray, lit by
    // the sun and the sky, the caustics' web (caust3.frag) playing on it,
    // dimmed and tinted by the water the light crosses.
    if (r < SAND_R) {
        vec3 rs = refract(ray, N, 0.75);
        float ts = (SAND_Y - hitPos.y) / min(rs.y, -0.05);
        vec3 sp = hitPos + rs * ts;
        float patch_ = smoothstep(SAND_R, SAND_R * 0.45, length(sp.xz - f_ball.xz));
        // Gone by the top of the bottom third (SAND_TOP): above, outside
        // the ball's rectangle, the far shader draws, without sand.
        patch_ *= smoothstep(SAND_TOP, SAND_TOP - 0.1, gl_FragCoord.y * f_up.w);
        if (patch_ > 0.0) {
            float sun = max(f_sun.y, 0.0) * 1.1 * (f_mode.w > 0.5 ? caustics(sp.xz) : 1.0);
            vec3 sand = vec3(0.92, 0.88, 0.78) * (sun * vec3(1.0, 0.98, 0.94) + f_ambient.rgb * 1.2);
            // The water between: absorbing red first, then green.
            vec3 through = exp(-vec3(0.45, 0.18, 0.11) * (ts + (hitPos.y - SAND_Y)));
            body = mix(body, sand * through, patch_);
        }
    }
    float ring = 0.0;
    if (r < f_ball.w * 4.0) {
        float tr = hitBall(hitPos, R);
        if (tr > 0.0)
            reflection = shadeBall(hitPos + R * tr, R);
        vec3 rf = refract(ray, N, 0.75);
        float tu = hitBall(hitPos, rf);
        if (tu > 0.0) {
            vec3 bp = hitPos + rf * tu;
            vec3 under = shadeBall(bp, rf) * vec3(0.3, 0.5, 0.58);
            // The ripples' caustics playing on the ball under the water.
            if (f_mode.w > 0.5) {
                // The caustics: its light focused and spread by the ripples
                // above, most on the side facing up.
                float up = max(dot((bp - f_ball.xyz) / f_ball.w, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5, 0.0);
                under *= mix(1.0, caustics(bp.xz) * 1.92, up * 0.85);
            }
            body = mix(body, under, exp(-tu * 1.2) * 0.85);
            fresnel *= 0.6;
        }
        float dy = hitPos.y - f_ball.y;
        float edge = r - sqrt(max(f_ball.w * f_ball.w - dy * dy, 0.0));
        ring = smoothstep(0.03, 0.0, edge) * step(0.0, edge) * step(abs(dy), f_ball.w);
    }
#endif
    vec3 C = fresnel * reflection + (1.0 - fresnel) * body;
#ifndef FAR
    C = mix(C, C * 1.6 + vec3(0.01, 0.015, 0.02), ring * 0.5);
#endif
    vec2 far = abs(hitPos.xz) * (1.0 / EXTENT);
    float haze = smoothstep(0.55, 0.95, max(far.x, far.y));
    if (haze > 0.0)
        C = mix(C, sky(normalize(vec3(ray.x, 0.03, ray.z))), haze);
    return C;
}

void main()
{
    vec3 ray = normalize(v_ray);
#ifdef FAR
    float tb = -1.0; // the ball is drawn by the full shader, over its rectangle
#else
    float tb = hitBall(f_eye.xyz, ray);
#endif
    vec3 C;
    if (ray.y >= 0.0) {
        C = tb > 0.0 ? shadeBall(f_eye.xyz + ray * tb, ray) : sky(ray) + vec3(sunDisc(ray));
    } else {
        float dist = raymarchwater(ray, -f_eye.y / ray.y);
        if (tb > 0.0) {
            vec3 bp = f_eye.xyz + ray * tb;
            if (bp.y < seaHeight(bp.xz)) {
                dist = min(dist, tb - 0.02);
                tb = -1.0;
            }
        }
        if (tb > 0.0 && tb < dist)
            C = shadeBall(f_eye.xyz + ray * tb, ray);
        else
            C = shadeWater(f_eye.xyz, ray, dist);
    }
    C *= 2.0; // exposed as boingocean_fs does
    if (u_out == 1) {
        o_col = vec4(C, 1.0);
        return;
    }
    C = clamp(C * (2.51 * C + 0.03) / (C * (2.43 * C + 0.59) + 0.14), 0.0, 1.0);
    vec2 q = (gl_FragCoord.xy * vec2(f_rt.w, f_up.w) - 0.5) * vec2(f_eye.w, 1.0);
    o_col = vec4(sqrt(C) * (1.0 - dot(q, q) * 0.35), 1.0);
}
