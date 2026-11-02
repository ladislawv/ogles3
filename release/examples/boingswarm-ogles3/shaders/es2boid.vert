// The ES 2.0 way (key E): the boids' squares built by the 68k each frame,
// six vertices a boid in client arrays (no instancing in ES 2.0), each with
// its corner and its colour (brightness already in it). With u_mirror the
// reflections in the floor, as boid.vert's: mirrored below y = 0, dimmer,
// and fainter the higher.
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec4 a_col;
uniform mat4 u_vp;
uniform float u_mirror; // 0: the boids; 1: their reflections
varying vec2 v_uv;
varying vec4 v_col;

void main()
{
    vec3 p = a_pos;
    v_uv = a_uv;
    v_col = a_col;
    if (u_mirror > 0.0) {
        v_col.a *= 0.25 * exp(-max(p.y, 0.0) * 0.25);
        p.y = -p.y;
    }
    gl_Position = u_vp * vec4(p, 1.0);
}
