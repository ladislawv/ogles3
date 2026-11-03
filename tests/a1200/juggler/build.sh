#!/bin/bash
# Fills the ogles2-juggler package directory ($1): Juggler RT
# (release/examples/juggler-ogles3, docs/GLES3_PLAN.md, "Plan: Juggler RT"),
# with its shaders compiled by this tree's offline compiler and this tree's
# ogles3 library. One installed in LIBS: is what runs (Run-Juggler lists
# it). Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles3-build/m68k}
V=${V:-$HOME/ogles3-build/v3dglslc}
TC=${TC:-$HOME/amiga-gcc-install}
OGLES3_REL=${OGLES3_REL:-$HOME/ogles3-build/release/ogles3}
# make splits paths at spaces: the repository through a link without any
L=$HOME/.juggler-repo
ln -sfn "$REPO" "$L"
make -C "$L" -j8 B="$B" "$B/ogles3.library" "$B/libogles3.a" > /dev/null
mkdir -p "$PKG/libs" "$PKG/bin/shaders" "$PKG/bin/scenes"
cp "$B/ogles3.library" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc3"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
E=$L/release/examples/juggler-ogles3
make -C "$E" -s header
make -C "$E" -s CC="$TC/bin/m68k-amigaos-gcc" SDK="$OGLES3_REL/SDK" \
    CFLAGS="-O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall -I$B/gen/include -I$L/include" \
    LIBDIR="$B" OUTDIR="$W" "$W/juggler-ogles3" 2>&1 | grep -v lang_reloc || true
$TC/bin/m68k-amigaos-strip -o "$PKG/bin/juggler-ogles3" "$W/juggler-ogles3"
make -C "$E" -s shaders V3DGLSLC="$V/host/v3dglslc" OGLES3="$L" BUILD="$W/v3dp" > /dev/null
cp "$W"/v3dp/* "$PKG/bin/shaders/"
cp "$E"/scenes/*.dat "$PKG/bin/scenes/"
