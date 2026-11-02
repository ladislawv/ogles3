// The stats' text: the font's letter, in u_col, over the picture.
precision mediump float;
uniform sampler2D u_font;
uniform vec4 u_col;
varying vec2 v_uv;

void main()
{
    gl_FragColor = u_col * texture2D(u_font, v_uv).a;
}
