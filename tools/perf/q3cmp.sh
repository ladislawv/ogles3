#!/bin/bash
# Quake III (ioquake3's renderergl2) on ogles2.library 1.3 against ogles3.library,
# the same game, the library the only difference (in WSL):
#   bash tools/perf/q3cmp.sh build      the two games and their packages
#   bash tools/perf/q3cmp.sh run [N]    N pairs (default 2), 400 frames each: ms a frame
#   bash tools/perf/q3cmp.sh glue       the glue's per-function GL profile, both
#   bash tools/perf/q3cmp.sh probe      probe P's per-draw stages, both
#   bash tools/perf/q3cmp.sh pc         the PC sampler, by library function, both
#   bash tools/perf/q3cmp.sh shot       the last runs' screenshots (frame 400) compared
# The game: IOQ3_SRC (default ~/ioq3-amiga-ogles3, a working clone of
# github.com/ladislawv/ioq3-amiga-ogles3, forked from RTyp3r's), built by its build_ioq3_ogles2_wsl.sh
# against OGLES2_SDK (default ~/ogles2-build/release/ogles2/SDK) and its
# build_ioq3_ogles3_wsl.sh against OGLES3_SDK (default this copy's release
# SDK, ~/ogles3-build/release/ogles3/SDK: release/build_release.sh). The
# packages are m6-5's (Run-M1, the shader cache, Quake III's data at
# Stuff:Quake3-68k-beta10 on the A1200) with these games: ogles2-q3o2 with
# ogles2 1.3.2's library and compiler (release/dist), ogles2-q3o3 with this
# copy's; probe T off (its trace fills the log). With FRAMES the package
# prints no timedemo line: Quake's com_speeds (SPEEDS=1) is the measure, the
# demo's frames after loading (about 200 of the 400). Each package's first
# run compiles its shaders (about 2 minutes): run discards nothing, so run
# once before measuring. LIBS_INSTALL=1: see common.sh. A pair: about 90 s.
set -eu
. "$(dirname "$0")/common.sh"
cmd=${1:-run}
shift || true
IOQ3=${IOQ3_SRC:-$HOME/ioq3-amiga-ogles3}
SDK2=${OGLES2_SDK:-$HOME/ogles2-build/release/ogles2/SDK}
SDK3=${OGLES3_SDK:-$HOME/ogles3-build/release/ogles3/SDK}
LHA=${LHA:-$(command -v lha || echo "$HOME/tools/bin/lha")}
P2=$WORK/pkg/ogles2-q3o2
P3=$WORK/pkg/ogles2-q3o3

frames() { # RESULTS_DIR: the demo frames' averages
    grep "^frame:" "$1/qconsole.log" | awk 'NR>3 {n++; a+=$3; c+=$9; r+=$13; b+=$15}
        END {if (n) printf "%d frames: %.2f ms (%.1f fps), client %.2f, front end %.2f, back end %.2f\n",
             n, a/n, 1000/(a/n), c/n, r/n, b/n}'
}
run2() { # VERSION ARGS...: one run of package q3VERSION, its results directory
    local v=$1
    shift
    runpkg "$WORK/pkg/ogles2-q3$v" "$@"
}

case $cmd in
build)
    [ -f "$IOQ3/build_ioq3_ogles3_wsl.sh" ] || { echo "$IOQ3 has no build_ioq3_ogles3_wsl.sh (IOQ3_SRC)"; exit 1; }
    OGLES2_SDK=$SDK2 OUTDIR=$WORK/o2 bash "$IOQ3/build_ioq3_ogles2_wsl.sh" > "$WORK/o2.log" 2>&1 ||
        { tail "$WORK/o2.log"; exit 1; }
    OGLES3_SDK=$SDK3 OUTDIR=$WORK/o3 bash "$IOQ3/build_ioq3_ogles3_wsl.sh" > "$WORK/o3.log" 2>&1 ||
        { tail "$WORK/o3.log"; exit 1; }
    IOQ3_SRC=$IOQ3 bash "$REPO/tests/a1200/build_package.sh" m6-5 > "$WORK/m6-5.log" 2>&1 ||
        { tail "$WORK/m6-5.log"; exit 1; }
    rm -rf "$WORK/rel2"
    mkdir -p "$WORK/rel2" "$WORK/pkg"
    # ogles2 1.3.3's archive, from the 1.x copy (OGLES2_LHA; this repository's
    # release/dist/ holds ogles3's only)
    (cd "$WORK/rel2" && "$LHA" -xq "${OGLES2_LHA:-/mnt/c/- CPP/ogles2/release/dist/ogles2-1.3.3.lha}")
    L2=$(find "$WORK/rel2" -name ogles2.library | head -1)
    pc_sample || true
    for v in o2 o3; do
        P=$WORK/pkg/ogles2-q3$v
        rm -rf "$P"
        cp -r "$REPO/tests/a1200/out/ogles2-m6-5" "$P"
        rm -rf "$P/results" "$P/cache/"*
        cp "$WORK/$v/quake3-gl2" "$P/bin/"
        cp "$WORK/o2/"*.dll "$P/bin/"
        cp "$WORK/o2/"*.dll "$P/home/baseq3/"
        [ -f "$WORK/pc_sample" ] && cp "$WORK/pc_sample" "$P/"
        sed -i -e 's|^SetEnv OGLES3/Probe TP$|; probes off|' \
               -e 's|^  M6R5:bin/quake3-gl2 \(.*+wait {FRAMES}.*\)$|  ;pc Run >NIL: M6R5:pc_sample quake3-gl2 M6R5:results/pc.txt "" 120\n  M6R5:bin/quake3-gl2 \1\n  Wait 4|' \
               "$P/Run-M1"
    done
    rm -f "$P2/libs/"*
    cp "$L2" "$(dirname "$L2")/v3dglslc" "$P2/libs/"
    sed -i -e 's/ogles3\.library/ogles2.library/g' -e 's|OGLES3/|OGLES2/|g' -e 's/OGLES3Cache/OGLES2Cache/g' "$P2/Run-M1"
    ls "$P2/libs" "$P3/libs"
    ;;
run)
    install_lib
    for i in $(seq "${1:-2}"); do
        for v in o2 o3; do
            d=$(run2 $v FRAMES=400 SPEEDS=1)
            echo "$v $(basename "$d"): $(frames "$d/results")"
        done
    done
    ;;
glue)
    install_lib
    for v in o2 o3; do
        d=$(run2 $v FRAMES=400 PROFILE=1)
        echo "$v $(basename "$d")"
        grep "glue profile: gl" "$d/results/1-timedemo.txt" | awk '{print $3, $4, $6}' | sort > "$WORK/glue-$v.txt"
    done
    echo "per frame (loading included): calls, ogles2 us, ogles3 us"
    join -a1 -a2 -e 0 -o 0,1.2,1.3,2.3 "$WORK/glue-o2.txt" "$WORK/glue-o3.txt" |
        awk '{printf "%-26s %5s calls %7d -> %7d us  %+6d\n", $1, $2, $3, $4, $4 - $3}' | sort -k6 -n -r | head -25
    ;;
probe)
    install_lib
    sed -i 's|^; probes off$|SetEnv OGLES2/Probe P|' "$P2/Run-M1"
    sed -i 's|^; probes off$|SetEnv OGLES3/Probe P|' "$P3/Run-M1"
    for v in o2 o3; do
        d=$(run2 $v FRAMES=400)
        echo "$v $(basename "$d")"
        grep -E "timing: per (frame|draw)" "$d/results/ogles2.log" | tail -3
    done
    sed -i 's|^SetEnv OGLES[23]/Probe P$|; probes off|' "$P2/Run-M1" "$P3/Run-M1"
    ;;
pc)
    [ -f "$P3/pc_sample" ] || { echo "no PC sampler in the packages (DOOM3_REPO, then build)"; exit 1; }
    install_lib
    sed -i 's|^  ;pc Run|  Run|' "$P2/Run-M1" "$P3/Run-M1"
    d3=$(run2 o3 FRAMES=400 SPEEDS=1)
    d2=$(run2 o2 FRAMES=400 SPEEDS=1)
    sed -i 's|^  Run >NIL: M6R5:pc_sample|  ;pc Run >NIL: M6R5:pc_sample|' "$P2/Run-M1" "$P3/Run-M1"
    python3 -I "$PERF/libprof.py" "$d2/results/pc.txt" "$HOME/ogles2-build/m68k/ogles2.library.map" ogles2.library 600 \
        > "$d2/results/libprof.txt"
    python3 -I "$PERF/libprof.py" "$d3/results/pc.txt" "$B/ogles3.library.map" ogles3.library 600 > "$d3/results/libprof.txt"
    echo "$(basename "$d2") against $(basename "$d3") (loading included: v3dp_load, blake3, link are not the frame's)"
    python3 -I "$PERF/cmpprof.py" "$d2/results/libprof.txt" -- "$d3/results/libprof.txt"
    ;;
shot)
    R=$REPO/tests/a1200/results
    a=$(ls -d "$R"/ogles2-q3o3-* | tail -1)
    b=$(ls -d "$R"/ogles2-q3o2-* | tail -1)
    echo "$(basename "$a") against $(basename "$b"):"
    python3 -I "$PERF/tgacmp.py" "$a/results/shot0000.tga" "$b/results/shot0000.tga"
    ;;
*)
    sed -n '2,25p' "$0"
    exit 1
    ;;
esac
