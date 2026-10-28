#version 310 es
// dEQP-GLES31 draw_indirect's fragment shader (glsDrawTest.cpp).
in mediump vec4 v_color;
layout(location = 0) out mediump vec4 dEQP_FragColor;
void main(void)
{
    dEQP_FragColor = v_color;
}
