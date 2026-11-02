// The ES 2.0 way's floor: floor.frag's checks and violet lines, the lines
// of a fixed width (no fwidth without an extension in ES 2.0).
precision mediump float;
uniform vec3 u_clear;
varying vec2 v_xz;

void main()
{
    vec2 q = v_xz / 1.5;
    vec2 f = abs(fract(q) - 0.5);
    float check = mod(floor(q.x) + floor(q.y), 2.0);
    float line = 1.0 - smoothstep(0.0, 0.05, 0.5 - max(f.x, f.y));
    vec3 c = mix(vec3(0.045, 0.045, 0.06), vec3(0.075, 0.07, 0.095), check) + vec3(0.25, 0.12, 0.4) * line * 0.35;
    gl_FragColor = vec4(mix(c, u_clear, smoothstep(9.0, 14.0, length(v_xz))), 1.0);
}
