#!/bin/bash
# Fills the ogles2-b26 package directory ($1): Boing 2026 measured
# (docs/GLES3_PLAN.md, "Plan: Boing 2026 for ogles3"). bin2/: the GLES2
# example as ogles2 1.3.2 released it (OGLES2_REL, its Examples/ and Libs/);
# bin3/: the same program as the ogles3 release ships it (OGLES3_REL), and
# the ES 3 copy, release/examples/boing2026-ogles3, built here with its
# shaders compiled by this tree's offline compiler. The ogles3 library is
# this tree's; one installed in LIBS: is what runs (Run-B26 lists it).
# Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles3-build/m68k}
V=${V:-$HOME/ogles3-build/v3dglslc}
TC=${TC:-$HOME/amiga-gcc-install}
OGLES2_REL=${OGLES2_REL:-$HOME/ogles2-build/release/ogles2}
OGLES3_REL=${OGLES3_REL:-$HOME/ogles3-build/release/ogles3}
[ -f "$OGLES2_REL/Examples/boing2026" ] && [ -f "$OGLES3_REL/Examples/boing2026" ] ||
    { echo "needs the ogles2 and ogles3 releases at $OGLES2_REL and $OGLES3_REL (build_release.sh)"; exit 1; }
make -C "$REPO" -j8 B="$B" "$B/ogles3.library" "$B/libogles3.a" > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin2/shaders" "$PKG/bin3/shaders"
cp "$OGLES2_REL/Libs/ogles2.library" "$OGLES2_REL/Libs/v3dglslc" "$PKG/libs/"
cp "$B/ogles3.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc3"
cp "$OGLES2_REL/Examples/boing2026" "$PKG/bin2/"
cp "$OGLES2_REL"/Examples/shaders/* "$PKG/bin2/shaders/"
cp "$OGLES3_REL/Examples/boing2026" "$PKG/bin3/"
cp "$OGLES3_REL"/Examples/shaders/* "$PKG/bin3/shaders/"

# The ES 3 copy, built from the SDK the ogles3 release made and this tree's
# generated headers (its import library and inline calls).
E=$REPO/release/examples/boing2026-ogles3
[ -f "$E/boing2026-ogles3.c" ] || exit 0
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
make -C "$E" -s CC="$TC/bin/m68k-amigaos-gcc" SDK="$OGLES3_REL/SDK" \
    CFLAGS="-O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall -I$B/gen/include -I$REPO/include" \
    LIBDIR="$B" OUTDIR="$W" "$W/boing2026-ogles3"
$TC/bin/m68k-amigaos-strip -o "$PKG/bin3/boing2026-ogles3" "$W/boing2026-ogles3"
make -C "$E" -s shaders V3DGLSLC="$V/host/v3dglslc" OGLES3="$REPO" BUILD="$W/v3dp" > /dev/null
cp "$W"/v3dp/* "$PKG/bin3/shaders/"
