#version 300 es
// One texel of a float or RGBA8 texture, its alpha too: with a float texture
// the draw's key needs the sampler's 32-bit bit (sampler_is_32b), and with
// GL_SAMPLE_ALPHA_TO_COVERAGE in a 4x MSAA context its alpha sets the samples
// covered (GLES3 plan stage A item 5).
precision mediump float;
uniform highp sampler2D tex;
out vec4 color;
void main()
{
    color = texture(tex, vec2(0.5));
}
