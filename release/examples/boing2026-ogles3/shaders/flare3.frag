#version 300 es
precision mediump float;
// Boing 2026 for OpenGL ES 3: the lens flare's pieces, added to the window.
// v_kind (flat, an integer): 0 the light itself (a hot core, a glow and a
// four-point star), 1 a halo ring, 2 a soft ghost disc.
in vec2 v_uv;
flat in vec3 v_col;
flat in int v_kind;
layout(location = 0) out vec4 o_col;

void main()
{
    float r = length(v_uv);
    vec3 c;
    if (v_kind == 0) {
        float core = exp(-r * r * 60.0) * 3.0;
        float glow = exp(-r * 4.5) * 0.7;
        float star = (exp(-abs(v_uv.x) * 70.0) + exp(-abs(v_uv.y) * 70.0)) * exp(-r * 2.5) * 0.9;
        c = vec3(core) + v_col * (glow + star);
    } else if (v_kind == 1) {
        c = v_col * exp(-abs(r - 0.8) * 30.0) * 0.35;
    } else {
        c = v_col * smoothstep(1.0, 0.7, r) * (0.10 + 0.12 * r);
    }
    o_col = vec4(c * (1.0 - smoothstep(0.85, 1.0, r * 0.95)), 1.0);
}
