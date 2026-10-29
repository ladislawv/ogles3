#!/bin/bash
# Fills the ogles2-drawbench package directory ($1): tests/perf/drawbench.c
# built twice, against this tree's libogles3.a (bin/drawbench3) and against
# ogles2's SDK (bin/drawbench2, OGLES2_SDK; ogles2.library and its compiler
# from OGLES2_LIBS, both from the ogles2 release unpacked by ogles2's
# build_release.sh). The ogles3 library and compiler are this tree's; one
# installed in LIBS: is what runs (Run-Bench lists it). Called by
# ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
B=${B:-$HOME/ogles2-build/m68k}
V=${V:-$HOME/ogles2-build/v3dglslc}
TC=${TC:-$HOME/amiga-gcc-install}
OGLES2_SDK=${OGLES2_SDK:-$HOME/ogles2-build/release/ogles2/SDK}
OGLES2_LIBS=${OGLES2_LIBS:-$HOME/ogles2-build/release/ogles2/Libs}
[ -f "$OGLES2_SDK/lib/libogles2.a" ] && [ -f "$OGLES2_LIBS/ogles2.library" ] ||
    { echo "no ogles2 release at $OGLES2_SDK and $OGLES2_LIBS"; exit 1; }
make -C "$REPO" -j8 B="$B" "$B/ogles3.library" "$B/libogles3.a" > /dev/null
CC=$TC/bin/m68k-amigaos-gcc
FL="-std=gnu99 -O2 -mcpu=68020 -m68881 -mcrt=clib2 -fno-builtin -Wall"
mkdir -p "$PKG/libs" "$PKG/bin"
$CC $FL -I"$REPO/include" -I"$B/gen/include" "$REPO/tests/perf/drawbench.c" -L"$B" -logles3 -o "$PKG/bin/drawbench3"
$CC $FL -DBENCH_OGLES2 -I"$OGLES2_SDK/include" "$REPO/tests/perf/drawbench.c" -L"$OGLES2_SDK/lib" -logles2 \
    -o "$PKG/bin/drawbench2"
cp "$B/ogles3.library" "$OGLES2_LIBS/ogles2.library" "$OGLES2_LIBS/v3dglslc" "$PKG/libs/"
cp "$V/v3dglslc.stripped" "$PKG/libs/v3dglslc3"
# PC_SAMPLE: the Doom 3 port's PC sampler, for Run-Bench PC=1 (tools/perf/drawbench.sh
# builds it and gives it here).
if [ -n "${PC_SAMPLE:-}" ] && [ -f "$PC_SAMPLE" ]; then
    cp "$PC_SAMPLE" "$PKG/bin/"
fi
