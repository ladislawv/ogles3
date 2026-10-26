precision mediump float;
/* No colour output: no colour write reaches the tile buffer, whatever the
 * render targets' formats (a 32-bit float or integer target must not get
 * a 16-bit float write: it upsets the GPU until a power cycle). */
void main() {
}
