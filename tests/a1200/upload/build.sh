#!/bin/bash
# Fills the ogles2-upload package directory ($1): upload_test, the 16-bit
# texture upload report's test (tests/perf/upload_test.c): 64 tiles of
# 256x256 into a 1024x1024x4 2D array, RGBA8 against RGB565, RGB5_A1 and
# RGBA4 (16-bit and byte data). The library in libs/. Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
TC=${TC:-$HOME/amiga-gcc-install}
make -C "$REPO" -j8 B="$B" "$B/ogles3.library" "$B/libogles3.a" > /dev/null
mkdir -p "$PKG/bin" "$PKG/libs"
cp "$B/ogles3.library" "$PKG/libs/"
"$TC/bin/m68k-amigaos-gcc" -std=gnu99 -O2 -mcpu=68020 -m68881 -mcrt=clib2 -Wall -I"$REPO/include" \
    -I"$B/gen/include" "$REPO/tests/perf/upload_test.c" -L"$B" -logles3 -o "$PKG/bin/upload_test"
