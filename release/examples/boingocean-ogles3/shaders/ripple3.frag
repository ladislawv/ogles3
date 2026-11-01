#version 300 es
precision highp float;
// Boing Ocean for OpenGL ES 3: the ripples, once a frame into a small tiling
// half-float texture (RG16F, 128 x 128 over 4 x 4 units): the slope of 16
// short waves, each with a whole number of wavelengths across the tile (so
// it repeats without a seam), each running at its own speed. The water reads
// it twice a pixel (ocean3.frag) where boingocean_fs works out three sines.
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
in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main()
{
    // Whole wave counts across the tile in x and y, so the tile repeats;
    // w: the amplitude of the slope.
    const vec3 W[16] = vec3[16](vec3(3.0, 1.0, 0.030), vec3(-2.0, 3.0, 0.026), vec3(4.0, -3.0, 0.020),
                                vec3(1.0, 5.0, 0.018), vec3(-5.0, -2.0, 0.016), vec3(6.0, 4.0, 0.012),
                                vec3(-3.0, 7.0, 0.010), vec3(8.0, -1.0, 0.009), vec3(-7.0, -6.0, 0.007),
                                vec3(2.0, -9.0, 0.006),
                                vec3(10.0, 3.0, 0.005), vec3(-4.0, 11.0, 0.004), vec3(11.0, -7.0, 0.0035),
                                vec3(-12.0, -2.0, 0.003), vec3(5.0, 13.0, 0.0025), vec3(-9.0, -10.0, 0.002));
    vec2 slope = vec2(0.0);
    for (int i = 0; i < 16; i++) {
        vec2 k = W[i].xy * 6.2831853; // a whole number of cycles across 0..1
        float len = length(W[i].xy);
        // Deep-water ripples: the shorter, the slower in phase per cycle.
        float x = dot(k, v_uv) - f_sun.w * sqrt(len) * 2.2;
        slope += W[i].xy / len * (cos(x) * W[i].z * len);
    }
    o_col = vec4(slope, 0.0, 1.0);
}
