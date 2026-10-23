#!/bin/bash
# Fills the ogles2-m8-2 package directory ($1): dEQP-GLES3 (ports/deqp,
# MODULE=gles3) with its data, an ES3=1 test build of the library that logs
# every line at once, and the on-target shader compiler (every test's
# shaders compile on the Amiga and are kept in cache/). OpenGL ES 3 stage A
# item 6 (docs/GLES3_PLAN.md): the ES 3.0 context and dEQP-GLES3.info; the
# rest of the list from stage B on. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B3=${B3:-$(dirname "${B:-$HOME/ogles2-build/m68k}")/m68k-es3}
B3S=${B3S:-$B3-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$B3S" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC ES3=1 > /dev/null
make -C "$REPO" -j8 B="$B3" ES3=1 > /dev/null
MODULE=gles3 B="$B3" bash "$REPO/ports/deqp/build.sh" > /dev/null 2>&1 ||
    { echo "MODULE=gles3 ports/deqp/build.sh failed"; exit 1; }
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/cache"
cp "$B3S/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B3/deqp-gles3/deqp-gles3" "$B3S/cmd/OGLES2Log" "$PKG/bin/"
cp -r "$B3/deqp-gles3/gles3" "$PKG/bin/gles3"
# The must-pass list, a file per group (Run-M1 LIST=<group>).
MODULE=gles3 bash "$REPO/ports/deqp/caselists.sh" "$PKG/caselists" > /dev/null
