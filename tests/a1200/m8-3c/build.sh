#!/bin/bash
# Fills the ogles2-m8-3c package directory ($1): m8-3 (../m8-3/build.sh:
# dEQP-GLES31, its library, compiler and lists) with the C3=1 dEQP
# (ports/deqp/c3/README.md): the built-in functions' precision checks also on
# the Pi's free ARM cores, bin/deqp-c3.c3m beside it. m8-3 stays as it is.
# Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B31=${B31:-$(dirname "${B:-$HOME/ogles2-build/m68k}")/m68k-es31}

bash "$HERE/../m8-3/build.sh" "$PKG"
C3=1 MODULE=gles31 B="$B31" bash "$REPO/ports/deqp/build.sh" > "$HERE/../out/m8-3c-deqp-build.txt" 2>&1 ||
    { tail -20 "$HERE/../out/m8-3c-deqp-build.txt"; echo "C3=1 MODULE=gles31 ports/deqp/build.sh failed"; exit 1; }
cp "$B31/deqp-gles31-c3/deqp-gles31" "$B31/deqp-gles31-c3/deqp-c3.c3m" "$PKG/bin/"
