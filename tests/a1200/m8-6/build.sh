#!/bin/bash
# Fills the ogles2-m8-6 package directory ($1): dEQP-GLES31 draw_indirect's
# restarts (docs/GLES3_PLAN.md §7, item 3, "Parked"), m8_indirect drawing
# the cases' draws over and over in a 256x256 window, with the ES31=1 test
# build of the library that logs every line at once (as m8-3's). The
# on-target compiler in libs/ (draw-time variants compiled into cache/).
# Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B31S=${B31S:-$(dirname "${BS:-$HOME/ogles2-build/m68k-sync}")/m68k-es31-sync}

make -C "$REPO" -j8 B="$B31S" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC ES31=1 > /dev/null
make -C "$REPO" B="$B31S" test-v3dp > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin/shaders"
cp "$B31S/ogles2.library" "$PKG/libs/"
cp "${V:-$HOME/ogles2-build/v3dglslc}/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B31S/tests/m8_indirect" "$B31S/cmd/OGLES2Log" "$PKG/bin/"
cp "$B31S"/v3dp/*.v3dp "$B31S"/v3dp/*.v3dw "$PKG/bin/"
for f in "$REPO"/tools/v3dglslc/test/shaders/es31di.*; do
    sed 's/\r$//' "$f" > "$PKG/bin/shaders/$(basename "$f")"
done
