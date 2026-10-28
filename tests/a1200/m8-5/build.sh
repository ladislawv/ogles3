#!/bin/bash
# Fills the ogles2-m8-5 package directory ($1): OpenGL ES 3.1, stage D
# (docs/GLES3_PLAN.md §7), with an ES31=1 test build of the library that
# logs every line at once: m8_es31 (ES 3.1's state, with the GPU: the parts
# with texture storage too), its limits, and m8_draw in an ES 3.1 context
# (its ES 3.1 draws run there: test_vab). The on-target compiler in libs/
# (draw-time variants compiled into cache/). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B31S=${B31S:-$(dirname "${BS:-$HOME/ogles2-build/m68k-sync}")/m68k-es31-sync}

make -C "$REPO" -j8 B="$B31S" EXTRA_CFLAGS=-DOGLES3_LOG_SYNC ES31=1 > /dev/null
make -C "$REPO" B="$B31S" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin/shaders"
cp "$B31S/ogles3.library" "$PKG/libs/"
cp "${V:-$HOME/ogles2-build/v3dglslc}/v3dglslc.stripped" "$PKG/libs/v3dglslc3"
cp "$B31S/tests/m8_es31" "$B31S/tests/m8_draw" "$B31S/cmd/OGLES3Log" "$PKG/bin/"
cp "$B31S"/v3dp/*.v3dp "$B31S"/v3dp/*.v3dw "$PKG/bin/"
for f in "$REPO"/tools/v3dglslc/test/shaders/*; do
    sed 's/\r$//' "$f" > "$PKG/bin/shaders/$(basename "$f")"
done
