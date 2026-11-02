#version 300 es
// Each vertex one boid, a point sprite (a square facing the screen: one
// vertex a light, where a 4-vertex instanced billboard cost 3.5 ms a frame
// in vertices alone), its position from the simulation's image
// (texelFetch). Only the light's inner 70% is drawn (its faint outer glow
// cut: the render is fill, and that rim was half of it); the light's size
// kept: the point 1.4 u_size across in the world (u_pscale: pixels a unit
// at distance 1). u_step: every boid (1) or every u_step-th, u_step times as
// bright (2: the reflection, soft; 4: the glow, blurred: less work). With
// u_mirror the boid's reflection in the floor: mirrored below it (y = 0), dimmer, and
// fainter the higher the boid flies.
uniform highp sampler2D u_boids;
uniform mat4 u_vp;
uniform float u_size;
uniform float u_pscale;
uniform vec3 u_eye;
uniform float u_near; // the eye's distance to the ball's centre
uniform float u_mirror; // 0: the boids; 1: their reflections
uniform int u_step;     // every u_step-th boid, u_step times as bright
flat out float v_red;
flat out float v_bright;
flat out float v_speed; // 0..1: the boid's speed, of 12 units a second

void main()
{
    int id = gl_VertexID * u_step;
    vec4 b = texelFetch(u_boids, ivec2(id & 127, id >> 7), 0);
    v_red = floor(b.w);
    v_speed = fract(b.w);
    // Nearer the camera brighter: from 0.2 three units beyond the ball's
    // centre to 3 three units before it (its near side).
    v_bright = mix(0.2, 3.0, clamp((u_near + 3.0 - distance(b.xyz, u_eye)) / 6.0, 0.0, 1.0)) * float(u_step);
    if (u_mirror > 0.0) {
        v_bright *= 0.25 * exp(-b.y * 0.25);
        b.y = -b.y;
    }
    gl_Position = u_vp * vec4(b.xyz, 1.0);
    gl_PointSize = 1.4 * u_size * u_pscale / gl_Position.w;
}
