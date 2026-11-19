#!/bin/bash
# Fills the ogles2-m8-7 package directory ($1): m8_queue, job.c's render
# chain (a framebuffer object's job queued behind the running render, from
# ogles2pi3's m8_queue), with the cases its binning must not run beside
# (H1: a vertex shader reading the texture just rendered; H2: feedback into
# a buffer the object drew from). The library and the on-target compiler
# in libs/ (H1's and H2's programs are compiled there). Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
make -C "$REPO" -j8 B="$B" "$B/ogles3.library" "$B/tests/m8_queue" > /dev/null
make -C "$REPO" B="$B" test-v3dp > /dev/null
mkdir -p "$PKG/bin" "$PKG/libs"
cp "$B/ogles3.library" "$PKG/libs/"
cp "${V:-$HOME/ogles2-build/v3dglslc}/v3dglslc.stripped" "$PKG/libs/v3dglslc3"
cp "$B/tests/m8_queue" "$PKG/bin/"
cp "$B"/v3dp/*.v3dp "$PKG/bin/"
