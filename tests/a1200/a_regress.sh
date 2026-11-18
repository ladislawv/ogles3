#!/bin/bash
# The regression run on the final library and compiler, through amigent
# (tests/a1200/a_run.sh): the test packages, then dEQP-GLES2, -GLES3 and
# -GLES31 whole, a must-pass list at a time (each package built once, then
# NOBUILD=1), every program compiled on the PC (BRIDGE=1: start
# tools/compile_bridge/compile_bridge.py --port 6802 in WSL first), and the
# speed (drawbench). docs/GLES3_PLAN.md, "The GLES2 regression set" and the
# full runs. A line per run into tests/a1200/out/regress-<date>.txt.
#   bash tests/a1200/a_regress.sh [packages|deqp2|deqp3|deqp31|speed|<package> ...]   (default: all, in that order)
# FROM=<group>: the first dEQP part from that must-pass list on (a paused run
# resumed), the parts after it whole.
# The library and compiler under test go into LIBS: first (LIBS_INSTALL=1 with m8-5).
# C3=1: dEQP-GLES3 and -GLES31 from m8-2c and m8-3c, the precision checks on
# the Pi's ARM cores (ports/deqp/c3: the same results, the built-in function
# groups in a fraction of the time).
set -u
D3=m8-2
D31=m8-3
[ "${C3:-}" = 1 ] && { D3=m8-2c; D31=m8-3c; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/out
SUM=$OUT/regress-$(date +%Y%m%d-%H%M).txt
say() { echo "$(date +%T) $*" | tee -a "$SUM"; }
CTL=${AMIGENT_CTL:-"/c/- CPP/amigent/.venv/Scripts/amigent-ctl.exe"}
STATION=$(cygpath -w "$HERE/../../tools/amictl/station.local")
export AMIGENT_HOST=${AMIGENT_HOST:-$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['host'])" "$STATION")}
one() { # package, then a_run.sh's last line
    local r
    r=$(bash "$HERE/a_run.sh" "$1" 2>&1 | grep -E "PASS,|NO RESULTS|STATION DOWN|failed|did not|another a_run" | tail -1)
    say "$1: ${r#* $1: }"
    case $r in *"STATION DOWN"* | *"another a_run"* | "") say "stopped: ${r:-no answer from a_run.sh}"; exit 4 ;; esac
    # the GLES2 regression packages' ES 2.0 (their Run-M1), not left for the next
    "$CTL" unsetenv OGLES3/Version > /dev/null 2>&1
}
deqp() { # package: each must-pass list (not the -pNN parts)
    local p=$1 g first=1 r
    # FROM=<group>: that group and the ones after it (a paused run resumed)
    for g in $(ls "$OUT/ogles2-$p/caselists" 2>/dev/null | grep -v -- '-p[0-9]' | sed 's/\.txt$//' |
               awk -v f="${FROM:-}" 'f == "" || $0 >= f'); do
        r=$(NOBUILD=$([ $first = 1 ] && echo 0 || echo 1) BRIDGE=1 TIMEOUT=${T:-14400} ARGS="LIST=$g NAME=$g" \
            bash "$HERE/a_run.sh" "$p" 2>&1 | grep -E "^   $p: $g:|NO RESULTS|STATION DOWN|failed|did not|another a_run" | tail -1)
        first=0
        say "$p $g: ${r#*: $g: }"
        case $r in *"STATION DOWN"* | *"another a_run"* | "") say "stopped: ${r:-no answer from a_run.sh}"; exit 4 ;; esac
    done
    FROM= # the paused part's: the parts after it whole
}
say "regression run: ${*:-all}"
for part in ${*:-packages deqp2 deqp3 deqp31 speed}; do
    case $part in
    packages)
        LIBS_INSTALL=1 one m8-5 # the library and compiler under test into LIBS:
        for p in m5-1 m6-6 m6-8 m6-10 m7-2 m7-3; do BUILD_ENV='B=$BS' one $p; done
        for p in m7-4 m2-5 m6-3 m6-4 m6-1 m6-2 m6-5 m8-1 m8-4 m8-6; do one $p; done ;;
    deqp2) deqp m7-1 ;;
    deqp3) deqp $D3 ;;
    deqp31) deqp $D31 ;;
    speed) one drawbench ;;
    m5-1 | m6-6 | m6-8 | m6-10 | m7-2 | m7-3) BUILD_ENV='B=$BS' one "$part" ;; # built against the sync build
    *) one "$part" ;; # a package by its name
    esac
done
say "done: $SUM"
