#!/bin/bash
# tests/perf/drawbench on the A1200 (package tests/a1200/drawbench), in WSL:
#   bash tools/perf/drawbench.sh run [Run-Bench args]   ogles2 against ogles3, a table
#   bash tools/perf/drawbench.sh probe TEST             probe P's stages of TEST, both
#   bash tools/perf/drawbench.sh profile TEST [MS]      the PC sampler on TEST, both:
#                                                       us a call by function
#   bash tools/perf/drawbench.sh lines DIR FUNC         a profile's samples in draw.c's
#                                                       FUNC by source line (DIR: its
#                                                       results/, the build that ran)
# Run-Bench args: ONLY=2|3|3es2, TEST=<name>, MS=<ms a test> (700). The package is
# built from this copy ($B: the library, libogles3.a) and ogles2 1.3's release SDK
# and library (tests/a1200/drawbench/build.sh). LIBS_INSTALL=1: see common.sh.
# Tests and their names: tests/perf/drawbench.c. One run of all: about 30 s.
set -eu
. "$(dirname "$0")/common.sh"
cmd=${1:-run}
shift || true

build() {
    make -C "$REPO" -j8 B="$B" "$B/ogles3.library" 2>&1 | grep -E "warning|error" || true
    local pcs=
    pc_sample 2> /dev/null && pcs=$WORK/pc_sample
    PC_SAMPLE=$pcs bash "$REPO/tests/a1200/build_package.sh" drawbench > "$WORK/drawbench-pkg.log" 2>&1 ||
        { tail "$WORK/drawbench-pkg.log"; exit 1; }
    install_lib
}

case $cmd in
run)
    build
    d=$(runpkg "$REPO/tests/a1200/out/ogles2-drawbench" "$@")
    echo "$(basename "$(dirname "$d/x")")"
    python3 -I "$PERF/dbcmp.py" "$d/results"
    ;;
probe)
    build
    for v in 2 3; do
        $A run "Delete T:ogles$v.log QUIET" > /dev/null
        $A setenv OGLES$v/Probe P > /dev/null
        $A run -t 120 -d Stuff:amictld/work/ogles2-drawbench "Execute Run-Bench ONLY=$v TEST=$1 MS=1500" > /dev/null
        $A unsetenv OGLES$v/Probe > /dev/null
        $A get T:ogles$v.log "$WORK/probe$v.log" > /dev/null
        grep "timing: per frame" "$WORK/probe$v.log" | tail -2 | awk -v v=ogles$v '
          /draws,/ {n=$5}
          /in draws:/ {printf "%s (%d draws a frame), us a draw: checks %.2f shaders %.2f textures %.2f uniforms %.2f vertex %.2f record %.2f indices %.2f binning %.2f\n", v, n, $7/n, $10/n, $12/n, $14/n, $16/n, $19/n, $21/n, $23/n}'
    done
    ;;
profile)
    pc_sample || exit 1
    build
    d=$(runpkg "$REPO/tests/a1200/out/ogles2-drawbench" "TEST=$1 PC=1 MS=${2:-4000} ONLY=all")
    echo "$(basename "$(dirname "$d/x")")"
    python3 -I "$PERF/libprof.py" "$d/results/2-pc.txt" "$HOME/ogles2-build/m68k/ogles2.library.map" ogles2.library 600 \
        > "$d/results/2-prof.txt"
    python3 -I "$PERF/libprof.py" "$d/results/3-pc.txt" "$B/ogles3.library.map" ogles3.library 600 \
        > "$d/results/3-prof.txt"
    n2=$(sed -n 's/^TEST \S* \S* *[0-9]* ns (\([0-9]*\)).*/\1/p' "$d/results/2.txt" | head -1)
    n3=$(sed -n 's/^TEST \S* \S* *[0-9]* ns (\([0-9]*\)).*/\1/p' "$d/results/3.txt" | head -1)
    python3 -I "$PERF/dbcmp.py" "$d/results"
    python3 -I "$PERF/cmpprof.py" --per "$n2" "$n3" "$d/results/2-prof.txt" -- "$d/results/3-prof.txt"
    ;;
lines)
    bash "$PERF/listing.sh" ogles3/draw.c > /dev/null
    pc=$1/3-pc.txt
    [ -f "$pc" ] || pc=$1/pc.txt
    python3 -I "$PERF/hotpc.py" "$pc" "$B/ogles3.library.map" ogles3.library "$2" > "$WORK/hot.txt"
    python3 -I "$PERF/hotline.py" "$WORK/draw.lst" "$WORK/draw.s" "$REPO/ogles3/draw.c" "$2" "$WORK/hot.txt" 30
    ;;
*)
    sed -n '2,14p' "$0"
    exit 1
    ;;
esac
