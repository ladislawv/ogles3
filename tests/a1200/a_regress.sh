#!/bin/bash
# The regression run on the final library and compiler, through amigent
# (tests/a1200/a_run.sh): the test packages, then dEQP-GLES2, -GLES3 and
# -GLES31 whole, a must-pass list at a time (each package built once, then
# NOBUILD=1), every program compiled on the PC (BRIDGE=1: start
# tools/compile_bridge/compile_bridge.py --port 6802 in WSL first), and the
# speed (drawbench). docs/GLES3_PLAN.md, "The GLES2 regression set" and the
# full runs. A line per run into tests/a1200/out/regress-<date>.txt.
#   bash tests/a1200/a_regress.sh [packages|deqp2|deqp3|deqp31|speed ...]   (default: all, in that order)
# The library and compiler under test go into LIBS: first (LIBS_INSTALL=1 with m8-5).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/out
SUM=$OUT/regress-$(date +%Y%m%d-%H%M).txt
say() { echo "$(date +%T) $*" | tee -a "$SUM"; }
one() { # package, then a_run.sh's last line
    local r
    r=$(bash "$HERE/a_run.sh" "$1" 2>&1 | grep -E "PASS,|NO RESULTS|STATION DOWN|failed|did not" | tail -1)
    say "$1: ${r#* $1: }"
    case $r in *"STATION DOWN"*) exit 4 ;; esac
}
deqp() { # package: each must-pass list (not the -pNN parts)
    local p=$1 g first=1 r
    for g in $(ls "$OUT/ogles2-$p/caselists" 2>/dev/null | grep -v -- '-p[0-9]' | sed 's/\.txt$//'); do
        r=$(NOBUILD=$([ $first = 1 ] && echo 0 || echo 1) BRIDGE=1 TIMEOUT=${T:-14400} ARGS="LIST=$g NAME=$g" \
            bash "$HERE/a_run.sh" "$p" 2>&1 | grep -E "^   $p: $g:|NO RESULTS|STATION DOWN|failed|did not" | tail -1)
        first=0
        say "$p $g: ${r#*: $g: }"
        case $r in *"STATION DOWN"*) exit 4 ;; esac
    done
}
say "regression run: ${*:-all}"
for part in ${*:-packages deqp2 deqp3 deqp31 speed}; do
    case $part in
    packages)
        LIBS_INSTALL=1 one m8-5 # the library and compiler under test into LIBS:
        for p in m5-1 m6-6 m6-8 m6-10 m7-2 m7-3; do BUILD_ENV='B=$BS' one $p; done
        for p in m7-4 m2-5 m6-3 m6-4 m6-1 m6-2 m6-5 m8-1 m8-4 m8-6; do one $p; done ;;
    deqp2) deqp m7-1 ;;
    deqp3) deqp m8-2 ;;
    deqp31) deqp m8-3 ;;
    speed) one drawbench ;;
    *) say "unknown part $part" ;;
    esac
done
say "done: $SUM"
