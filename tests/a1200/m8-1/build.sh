#!/bin/bash
# Fills the ogles2-m8-1 package directory ($1): OpenGL ES 3 stage A
# (docs/GLES3_PLAN.md §4 item 7), m8_es3 with a release build of the library
# (the version tag's 30 refused) and with an ES3=1 test build (ES 3.0
# contexts), its limits for comparison with `v3dglslc -L`, and the smoke
# test. Both libraries log every line at once. Nothing draws: state-only
# contexts. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
BS=${BS:-$HOME/ogles2-build/m68k-sync}
B3=${B3:-$(dirname "$BS")/m68k-es3-sync}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B3" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC ES3=1 > /dev/null
mkdir -p "$PKG/libs-release" "$PKG/libs-es3" "$PKG/bin"
cp "$BS/ogles2.library" "$PKG/libs-release/"
cp "$B3/ogles2.library" "$PKG/libs-es3/"
cp "$BS/tests/m8_es3" "$BS/tests/smoke" "$BS/cmd/OGLES2Log" "$PKG/bin/"
