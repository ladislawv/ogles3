#version 300 es
// Each instance one boid, a billboard facing the camera (a 4-vertex strip):
// its position from the simulation's image (texelFetch), or with u_vbo from
// the storage buffer bound as an instanced attribute.
uniform highp sampler2D u_boids;
uniform mat4 u_vp;
uniform float u_size;
uniform vec3 u_right; // the camera's right and up in the world: the
uniform vec3 u_up;    // billboard faces it
uniform int u_vbo;
in vec4 a_boid;
out vec2 v_uv;
flat out float v_red;
flat out float v_bright;

void main()
{
    int id = gl_InstanceID;
    vec4 b = u_vbo != 0 ? a_boid : texelFetch(u_boids, ivec2(id & 127, id >> 7), 0);
    // Only the light's inner 70% drawn (its faint outer glow cut: the
    // render is fill, and that rim was half of it); the light's size kept.
    v_uv = (vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0) * 0.7;
    v_red = b.w;
    // Nearer the camera (at z = 9) brighter: from 0.2 at the ball's back
    // (z = -3) to 3 at its front (z = 3).
    v_bright = mix(0.2, 3.0, clamp((b.z + 3.0) / 6.0, 0.0, 1.0));
    gl_Position = u_vp * vec4(b.xyz + (u_right * v_uv.x + u_up * v_uv.y) * u_size, 1.0);
}
