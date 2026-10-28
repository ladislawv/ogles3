#!/bin/bash
# Fills the ogles2-m8-1 package directory ($1): OpenGL ES 3 stage A
# (docs/GLES3_PLAN.md §4 item 7), m8_es3 with a release build of the library
# (the version tag's 30 refused) and with an ES3=1 test build (ES 3.0
# contexts), its limits for comparison with `v3dglslc -L`, and the smoke
# test (state-only contexts); then m8_draw, GLSL ES 3.00 programs drawn into
# a hidden bitmap. Both libraries log every line at once. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
BS=${BS:-$HOME/ogles2-build/m68k-sync}
B3=${B3:-$(dirname "$BS")/m68k-es3-sync}

make -C "$REPO" -j8 B="$BS" EXTRA_CFLAGS=-DOGLES3_LOG_SYNC > /dev/null
make -C "$REPO" -j8 B="$B3" EXTRA_CFLAGS=-DOGLES3_LOG_SYNC ES3=1 > /dev/null
# The test programs' files (format 2 for the GLSL ES 3.00 ones), named by
# their hash: m8_es3 and m8_draw link them from PROGDIR: without a compiler.
make -C "$REPO" B="$B3" test-v3dp > /dev/null
mkdir -p "$PKG/libs-release" "$PKG/libs-es3" "$PKG/libs-es3n" "$PKG/bin/shaders"
cp "$BS/ogles3.library" "$PKG/libs-release/"
cp "$B3/ogles3.library" "$PKG/libs-es3/"
# The ES3 library with the on-target compiler (m8_draw's draw-time
# variants are compiled into cache/), and without it (they come from there).
cp "${V:-$HOME/ogles2-build/v3dglslc}/v3dglslc.stripped" "$PKG/libs-es3/v3dglslc3"
cp "$B3/ogles3.library" "$PKG/libs-es3n/"
cp "$BS/tests/m8_es3" "$BS/tests/m8_draw" "$BS/tests/smoke" "$BS/cmd/OGLES3Log" "$PKG/bin/"
cp "$B3"/v3dp/*.v3dp "$B3"/v3dp/*.v3dw "$PKG/bin/"
for f in "$REPO"/tools/v3dglslc/test/shaders/*; do
    sed 's/\r$//' "$f" > "$PKG/bin/shaders/$(basename "$f")"
done
# A 1.x compiler's file of an OpenGL ES 3 program (format 1.0), named by its
# hash, which Run-M1 puts into the cache before step 5: the library must
# refuse it and compile a format 2 file in its place (GLES3 plan stage A
# item 7). Kept in oldcache/ (format 1.0 is frozen): made once by ogles2
# 1.3.2's host v3dglslc (`v3dglslc -o . es3old.vert es3old.frag`, then
# v3dp/v3dp_hashname.py) from the sources whose SHA-256 sums follow; its
# name is their program hash, so other sources would leave it unread and
# test_old would test nothing: then make it again the same way.
ES3OLD_SUMS="e2a9ed0bd55e2a5ba63e83e9bf04c5e6d6b230a7c3a080523daadb657fbc0573 5b2a91fe7b56c335aee569f7e99114c13f671b6465c4e97709c9e95ecb5ced63"
[ "$(cat "$PKG/bin/shaders/es3old.vert" | sha256sum | cut -d' ' -f1) $(cat "$PKG/bin/shaders/es3old.frag" | sha256sum | cut -d' ' -f1)" = "$ES3OLD_SUMS" ] ||
    { echo "es3old's sources changed: make oldcache/'s format 1.0 file again (see build.sh)"; exit 1; }
[ "$(od -An -tu2 -j4 -N2 "$HERE"/oldcache/*.v3dp | tr -d ' ')" = 1 ] || { echo "oldcache/'s file is not format 1.0"; exit 1; }
mkdir -p "$PKG/oldcache"
cp "$HERE"/oldcache/*.v3dp "$PKG/oldcache/"
