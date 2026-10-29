# Sourced by tools/perf's scripts (in WSL): paths and the A1200 helpers.
#   PERF_WORK   their work directory (default ~/ogles3-perf): Quake III builds and
#               packages, listings, the PC sampler
#   B, V        this copy's library and compiler builds (local.env, sourced here)
#   DOOM3_REPO  the Doom 3 port's checkout, for its PC sampler's source
#               (tests/a1200/pc-sample/src/pc_sample.c; default /mnt/c/GitHub/doom3)
#   LIBS_INSTALL=1  lets a script copy $B/ogles3.library over LIBS:ogles3.library
#               on the A1200 (a system file: ask its owner; CLAUDE.md)
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
[ -f "$REPO/local.env" ] && . "$REPO/local.env" > /dev/null
B=${B:-$HOME/ogles2-build/m68k}
V=${V:-$HOME/ogles2-build/v3dglslc}
PERF=$REPO/tools/perf
WORK=${PERF_WORK:-$HOME/ogles3-perf}
TC=${TC:-$HOME/amiga-gcc-install}
TC16=${TC16:-$HOME/amiga-gcc162-install}
DOOM3_REPO=${DOOM3_REPO:-/mnt/c/GitHub/doom3}
A="python3 $REPO/tools/amictl/amictl.py"
mkdir -p "$WORK"

# The PC sampler (the Doom 3 port's), built once into $WORK/pc_sample.
pc_sample() {
    local src=$DOOM3_REPO/tests/a1200/pc-sample/src/pc_sample.c
    [ -f "$WORK/pc_sample" ] && return 0
    [ -f "$src" ] || { echo "no $src (DOOM3_REPO): no PC sampler" >&2; return 1; }
    "$TC16/bin/m68k-amigaos-gcc" -O2 -mcpu=68020 -m68881 -noixemul -Wall "$src" -o "$WORK/pc_sample"
}

# The library under test in LIBS: when one is installed there (it wins over a
# package's libs/): only with LIBS_INSTALL=1. Without one installed, the
# package's own copy is the one that runs.
install_lib() {
    if $A run "List LIBS:ogles3.library QUICK" 2> /dev/null | grep -q ogles3.library; then
        if [ "${LIBS_INSTALL:-0}" = 1 ]; then
            $A put "$B/ogles3.library" LIBS:ogles3.library > /dev/null
            echo "LIBS:ogles3.library is $B/ogles3.library" >&2
        else
            echo "warning: LIBS:ogles3.library is installed and runs instead of this build" \
                 "(LIBS_INSTALL=1 replaces it)" >&2
        fi
    fi
}

# runpkg DIR ARGS...: run a package, print its results directory.
runpkg() {
    local dir=$1 out d
    shift
    out=$($A runpkg "$dir" ${1:+-a "$*"} -t 900 2>&1)
    d=$(echo "$out" | tail -1 | sed -n 's/.*-> //p')
    if [ -z "$d" ]; then
        echo "$out" | tail -5 >&2
        echo "runpkg $dir: no results (is the station up? python3 tools/amictl/amictl.py hello)" >&2
        exit 1
    fi
    echo "$d"
}
