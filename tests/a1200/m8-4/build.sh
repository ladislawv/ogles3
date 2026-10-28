#!/bin/bash
# Fills the ogles2-m8-4 package directory ($1): OpenGL ES 3 contexts through
# SDL2 and SDL3 (docs/GLES3_PLAN.md stage C item 6): each port's gles3test
# with an ES3=1 test build of the library (ES 3.0 given, its program
# drawn; ES 3.2 refused; ES 2.0 as before), SDL2's testgles2 through the
# same backend, then gles3test with a release build (ES 3.0 refused, SDL's
# error). The sync-logging libraries; the on-target compiler beside the
# test build (the programs compile at run time). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
BS=${BS:-$HOME/ogles2-build/m68k-sync}
B3=${B3:-$(dirname "$BS")/m68k-es3-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES3_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B3" EXTRA_CFLAGS=-DOGLES3_LOG_SYNC ES3=1 > /dev/null
make -C "$REPO" -j8 B="$B" > /dev/null
B="$B" bash "$REPO/ports/sdl2/build.sh" > "$PKG.sdl2-build.log" 2>&1 || { tail -20 "$PKG.sdl2-build.log"; exit 1; }
B="$B" bash "$REPO/ports/sdl3/build.sh" > "$PKG.sdl3-build.log" 2>&1 || { tail -20 "$PKG.sdl3-build.log"; exit 1; }
rm -f "$PKG.sdl2-build.log" "$PKG.sdl3-build.log"
mkdir -p "$PKG/libs-es3" "$PKG/libs-release" "$PKG/bin" "$PKG/bin3"
cp "$B3/ogles3.library" "$PKG/libs-es3/"
cp "$V/v3dglslc.stripped" "$PKG/libs-es3/v3dglslc3"
cp "$BS/ogles3.library" "$PKG/libs-release/"
cp "$BS/cmd/OGLES3Log" "$PKG/bin/"
cp "$B/sdl2/tests/gles3test" "$B/sdl2/tests/testgles2" "$PKG/bin/"
cp "$B/sdl3/tests/gles3test" "$PKG/bin3/"
