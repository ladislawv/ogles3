// The ES 2.0 way: a boid's light from the same lens-flare texture (red the
// core and its star, green the glow), tinted, added straight to the screen
// (no float targets, no bloom).
precision mediump float;
uniform sampler2D u_flare;
varying vec2 v_uv;
varying vec4 v_col;

void main()
{
    vec2 t = texture2D(u_flare, v_uv).rg * vec2(0.6, 0.06);
    gl_FragColor = vec4((v_col.rgb * t.y + mix(v_col.rgb, vec3(1.0), 0.4) * t.x) * v_col.a, 0.0);
}
