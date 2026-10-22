#version 300 es
// GLSL ES 3.00 uniforms (docs/GLES3_PLAN.md stage A item 3): non-square
// matrices and unsigned values in the default block, for glUniform*ui,
// glUniformMatrix2x3fv and friends, and transpose.
in vec4 p;
uniform mat2x3 m23;
uniform mat3x2 m32;
uniform uvec3 u3;
uniform uint u1;
out vec3 v;
void main()
{
    v = m23 * vec2(1.0) + vec3(m32 * vec3(1.0), 1.0) + vec3(u3) + float(u1);
    gl_Position = p;
}
