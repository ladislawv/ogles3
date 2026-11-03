// The ES 2.0 way's picture onto the screen.
precision mediump float;
uniform sampler2D u_img;
varying vec2 v_uv;

void main()
{
    gl_FragColor = vec4(texture2D(u_img, v_uv).rgb, 1.0);
}
