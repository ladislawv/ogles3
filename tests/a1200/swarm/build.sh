#!/bin/bash
# Fills the ogles2-swarm package directory ($1): tests/perf/swarmbench, Boing
# Swarm's step 1 (docs/GLES3_PLAN.md, "Plan: Boing Swarm"), with its shaders
# compiled by this tree's offline compiler and this tree's ogles3 library.
# One installed in LIBS: is what runs (Run-Swarm lists it). Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles3-build/m68k}
V=${V:-$HOME/ogles3-build/v3dglslc}
TC=${TC:-$HOME/amiga-gcc-install}
S=$REPO/tests/perf/swarmbench
make -C "$REPO" -j8 B="$B" "$B/ogles3.library" "$B/libogles3.a" > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin/shaders"
cp "$B/ogles3.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc3"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
make -C "$S" -s CC="$TC/bin/m68k-amigaos-gcc" B="$B" REPO="$REPO" OUTDIR="$W" "$W/swarmbench"
$TC/bin/m68k-amigaos-strip -o "$PKG/bin/swarmbench" "$W/swarmbench"
make -C "$S" -s shaders V3DGLSLC="$V/host/v3dglslc" REPO="$REPO" BUILD="$W/v3dp" > /dev/null
cp "$W"/v3dp/* "$PKG/bin/shaders/"

# Boing Swarm itself, release/examples/boingswarm-ogles3, built from the SDK
# the ogles3 release made and this tree's generated headers (as b26's).
OGLES3_REL=${OGLES3_REL:-$HOME/ogles3-build/release/ogles3}
E=$REPO/release/examples/boingswarm-ogles3
make -C "$E" -s header
make -C "$E" -s CC="$TC/bin/m68k-amigaos-gcc" SDK="$OGLES3_REL/SDK" \
    CFLAGS="-O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall -I$B/gen/include -I$REPO/include" \
    LIBDIR="$B" OUTDIR="$W" "$W/boingswarm-ogles3"
$TC/bin/m68k-amigaos-strip -o "$PKG/bin/boingswarm-ogles3" "$W/boingswarm-ogles3"
make -C "$E" -s shaders V3DGLSLC="$V/host/v3dglslc" OGLES3="$REPO" BUILD="$W/v3dp-s" > /dev/null
cp "$W"/v3dp-s/* "$PKG/bin/shaders/"
