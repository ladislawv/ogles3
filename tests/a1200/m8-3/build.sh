#!/bin/bash
# Fills the ogles2-m8-3 package directory ($1): dEQP-GLES31 (ports/deqp,
# MODULE=gles31) with its data, an ES31=1 test build of the library that
# logs every line at once, and the on-target shader compiler (every test's
# shaders compile on the Amiga and are kept in cache/; ports/deqp/run_pc.sh
# --package m8-3 compiles them on the PC). OpenGL ES 3.1, stage D
# (docs/GLES3_PLAN.md §7). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B31=${B31:-$(dirname "${B:-$HOME/ogles2-build/m68k}")/m68k-es31}
B31S=${B31S:-$B31-sync}
V=${V:-$HOME/ogles2-build/v3dglslc}

make -C "$REPO" -j8 B="$B31S" EXTRA_CFLAGS=-DOGLES2_LOG_SYNC ES31=1 > /dev/null
make -C "$REPO" -j8 B="$B31" ES31=1 > /dev/null
MODULE=gles31 B="$B31" bash "$REPO/ports/deqp/build.sh" > /dev/null 2>&1 ||
    { echo "MODULE=gles31 ports/deqp/build.sh failed"; exit 1; }
mkdir -p "$PKG/libs" "$PKG/bin" "$PKG/cache"
cp "$B31S/ogles2.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc"
cp "$B31/deqp-gles31/deqp-gles31" "$B31S/cmd/OGLES2Log" "$PKG/bin/"
cp -r "$B31/deqp-gles31/gles31" "$PKG/bin/gles31"
# The must-pass list, a file per group (Run-M1 LIST=<group>).
MODULE=gles31 bash "$REPO/ports/deqp/caselists.sh" "$PKG/caselists" > /dev/null
