#!/bin/bash
# Fills the ogles2-m8-2c package directory ($1): m8-2 (../m8-2/build.sh:
# dEQP-GLES3, its library, compiler and lists) with the C3=1 dEQP
# (ports/deqp/c3/README.md): the built-in functions' precision checks also on
# the Pi's free ARM cores, bin/deqp-c3.c3m beside it. m8-2 stays as it is.
# Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B3=${B3:-$(dirname "${B:-$HOME/ogles2-build/m68k}")/m68k-es3}

bash "$HERE/../m8-2/build.sh" "$PKG"
C3=1 MODULE=gles3 B="$B3" bash "$REPO/ports/deqp/build.sh" > "$HERE/../out/m8-2c-deqp-build.txt" 2>&1 ||
    { tail -20 "$HERE/../out/m8-2c-deqp-build.txt"; echo "C3=1 MODULE=gles3 ports/deqp/build.sh failed"; exit 1; }
cp "$B3/deqp-gles3-c3/deqp-gles3" "$B3/deqp-gles3-c3/deqp-c3.c3m" "$PKG/bin/"
