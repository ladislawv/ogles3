#version 300 es
// Boing 2026 for OpenGL ES 3: the second light's lens flare in one instanced
// draw: gl_InstanceID picks the piece from a constant array, gl_VertexID
// the quad's corner (a strip of 4); no vertex data, no uniform per piece.
layout(std140) uniform Frame {
    mat4 f_vp;
    mat4 f_vpm;           // the camera mirrored in the floor
    mat4 f_lightvp[2];
    mat4 f_model;         // the ball: world from object
    mat4 f_nmat;          // its normals (the upper 3x3)
    vec4 f_light;
    vec4 f_lightcol;
    vec4 f_light2;
    vec4 f_light2col;
    vec4 f_eye;
    vec4 f_ball;          // centre, radius
    vec4 f_mode;          // x shadow, y reflection, z 1984, w Gouraud
    vec4 f_mode2;         // x checker antialiasing, y bloom, z aspect, w vignette
    vec4 f_shadow;        // x live impacts, y softness, z contact-hardening, w the room's back z
    vec4 f_occluder[2];   // the ball in each shadow map: centre, radius
    vec4 f_post;          // 1 / the window's size, 1 / the room target's
    vec4 f_cam;           // projection x and y scale, time, the ceiling's y
    vec4 f_flare;         // the second light on screen, visibility, aspect
    vec4 f_impact[8];     // sparks: where, w when
    vec4 f_impactn[8];    // the surface's normal there
};
out vec2 v_uv;
flat out vec3 v_col;
flat out int v_kind;

// Each piece: where along the line from the light through the screen's
// centre (1 the light), its size, its kind (0 the light, 1 a halo ring, 2 a
// ghost), and how far its colour is turned towards the next channel's.
const vec4 PIECE[8] = vec4[8](
    vec4(1.0, 0.42, 0.0, 0.0), vec4(1.0, 0.30, 1.0, 0.0),
    vec4(0.45, 0.07, 2.0, 0.15), vec4(0.15, 0.12, 2.0, 0.35), vec4(-0.25, 0.05, 2.0, 0.5),
    vec4(-0.55, 0.16, 2.0, 0.7), vec4(-0.9, 0.09, 2.0, 0.85), vec4(-1.3, 0.22, 1.0, 0.6));

void main()
{
    vec4 piece = PIECE[gl_InstanceID];
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vec3 c = f_light2col.rgb;
    float k = piece.w;
    c = vec3(c.y * k + c.x * (1.0 - k), c.z * k + c.y * (1.0 - k), c.x * k + c.z * (1.0 - k));
    v_col = c * 0.4 * f_flare.z;
    v_kind = int(piece.z);
    v_uv = corner;
    gl_Position = vec4(f_flare.xy * piece.x + corner * vec2(piece.y / f_flare.w, piece.y), 0.0, 1.0);
}
