#version 310 es
// dEQP-GLES31 draw_indirect's vertex shader (glsDrawTest.cpp), two vec2
// attributes: m8_indirect (GLES3 plan stage D item 3).
in highp vec2 a_0;
in highp vec2 a_1;
uniform highp float u_coordScale;
uniform highp float u_colorScale;
out mediump vec4 v_color;
void main(void)
{
    gl_PointSize = 1.0;
    highp vec2 coord = vec2(0.0, 0.0);
    highp vec3 color = vec3(1.0, 1.0, 1.0);
    coord += vec2(a_0.xy);
    color.rg = color.rg * vec2(a_1.xy);
    v_color = vec4(u_colorScale * color, 1.0) * 0.5 + vec4(0.5, 0.5, 0.5, 0.5);
    gl_Position = vec4(u_coordScale * coord, 1.0, 1.0);
}
