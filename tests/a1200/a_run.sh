#!/bin/bash
# Test packages on the A1200 through amigent (C:\- CPP\amigent, the fork with
# amictl's moves: amigentd on port 6502, its amigent-ctl), as ogles2pi3's
# tests/a1200/p5_run.sh does for the Pi 3 card. Builds each package
# (tests/a1200/build_package.sh, in WSL with local.env), and runs its Run-*
# script with amigent-ctl runpkg (only changed files uploaded, a line a
# minute while it runs, the batch stopped at exit status 4: the station
# down). The programs compile on the Amiga: each package has v3dglslc3 in
# its libs/. BRIDGE=1 (dEQP: m8-2, m8-3): on the PC instead, never on the
# A1200 (it may sit 20 minutes on one program): v3dglslc-remote runs
# detached from Stuff:amigent/bridge/ with NOFALLBACK (no bridge, a compile
# fails), and tools/compile_bridge/compile_bridge.py must run in WSL
# (BRIDGE_PORT, default 6802, ogles3's; ogles2pi3's is 6803).
#   bash tests/a1200/a_run.sh m8-5 [m8-1 ...]       (Git Bash)
#   BRIDGE=1 ARGS="CASES=dEQP-GLES31.functional.geometry_shading.* NAME=gs" bash tests/a1200/a_run.sh m8-3
# LIBS_INSTALL=1: the package's ogles3.library and v3dglslc3 copied into the
# Amiga's LIBS: first (an installed one wins over the package's libs/, which
# the Run-* scripts add after it; CLAUDE.md); AMIGENT_CTL: the amigent-ctl;
# AMIGENT_HOST: the A1200 (default: tools/amictl/station.local's host);
# OGLES3_WSL: the WSL distribution; TIMEOUT: the script's limit (s); ARGS:
# its KEY=VALUE words.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RES=$HERE/results
WREPO=$(echo "$REPO" | sed -E 's#^/([a-z])/#/mnt/\1/#')
in_wsl() { MSYS_NO_PATHCONV=1 wsl -d "${OGLES3_WSL:-Ubuntu-24.04}" "$@"; }
CTL=${AMIGENT_CTL:-"/c/- CPP/amigent/.venv/Scripts/amigent-ctl.exe"}
STATION=$(cygpath -w "$REPO/tools/amictl/station.local")
export AMIGENT_HOST=${AMIGENT_HOST:-$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['host'])" "$STATION")}
[ -n "$AMIGENT_HOST" ] || { echo "no station: AMIGENT_HOST, or tools/amictl/station.local's host"; exit 4; }
export AMIGENT_RESULTS=$RES
export AMIGENT_RATE=${AMIGENT_RATE:-400}
WORK=Stuff:amigent/work
# One batch at a time: amigentd has four command channels (ogles2pi3's p5_run.sh)
LOCK=$HERE/out/a_run.lock
mkdir -p "$HERE/out"
mkdir "$LOCK" 2> /dev/null || { echo "another a_run.sh is running ($LOCK; remove it if none is)"; exit 1; }
trap 'rmdir "$LOCK" 2> /dev/null' EXIT
ctl() { "$CTL" "$@"; }
need() {
    ctl "$@"
    local rc=$?
    if [ $rc = 4 ]; then
        echo "$(date +%T) STATION DOWN during: $*; batch stopped"
        exit 4
    fi
    return $rc
}
need mkdir "$WORK" > /dev/null 2>&1
BRIDGE_DIR=Stuff:amigent/bridge
stop_remote() { ctl run -t 30 'Break `Status COM=v3dglslc-remote` C' > /dev/null 2>&1; }
if [ "${BRIDGE:-}" = 1 ]; then
    in_wsl pgrep -f compile_bridge.py > /dev/null ||
        { echo "no compile_bridge.py running: start python3 tools/compile_bridge/compile_bridge.py in WSL first"; exit 1; }
    REMOTE=$(in_wsl sh -c 'wslpath -w "$HOME/ogles3-build/m68k/amictl/v3dglslc-remote"' | tr -d '\r')
    need mkdir "$BRIDGE_DIR" > /dev/null 2>&1
    need put "$REMOTE" "$BRIDGE_DIR/v3dglslc-remote" > /dev/null || { echo "no $REMOTE (make in WSL)"; exit 1; }
    need run -t 30 "Protect $BRIDGE_DIR/v3dglslc-remote +rwed" > /dev/null
fi
for t in "$@"; do
    echo "$(date +%T) $t: building"
    in_wsl bash -c "cd '$WREPO' && . ./local.env > /dev/null && OUT=\$V bash tests/a1200/build_package.sh $t" \
        > "$HERE/out/a_run-$t-build.txt" 2>&1 || { echo "$t: build failed (out/a_run-$t-build.txt)"; continue; }
    PKG=$HERE/out/ogles2-$t
    if [ "${LIBS_INSTALL:-}" = 1 ]; then
        echo "$(date +%T) $t: its ogles3.library and v3dglslc3 into LIBS:"
        need run -t 30 "Avail FLUSH" > /dev/null
        [ -f "$PKG/libs/ogles3.library" ] && need put "$PKG/libs/ogles3.library" LIBS:ogles3.library > /dev/null
        [ -f "$PKG/libs/v3dglslc3" ] && need put "$PKG/libs/v3dglslc3" LIBS:v3dglslc3 > /dev/null
    fi
    if [ "${BRIDGE:-}" = 1 ]; then
        # detached (Run >NIL:): it holds none of amigentd's four command channels
        stop_remote
        need start -d "$BRIDGE_DIR" "v3dglslc-remote >T:a-remote.txt PORT=${BRIDGE_PORT:-6802} WAIT NOFALLBACK" \
            "TOKEN=Stuff:amictld/amictld.token COMPILER=LIBS:v3dglslc3" > /dev/null
        up=0
        for i in 1 2 3 4 5 6; do
            sleep 2
            if ctl run -t 20 "Status" 2> /dev/null | grep -q "v3dglslc-remote"; then up=1; break; fi
        done
        if [ $up = 0 ]; then
            echo "$(date +%T) $t: v3dglslc-remote did not start ($(ctl tail -n 300 T:a-remote.txt 2>&1 | tr -d '\r' | tail -2))"
            continue
        fi
    fi
    stamp=$(date +%Y%m%d-%H%M%S)
    echo "$(date +%T) $t: uploading and running"
    ctl runpkg "$PKG" -t ${TIMEOUT:-1800} -w "$WORK" -r "$RES" ${ARGS:+-a "$ARGS"} 2>&1 |
        grep --line-buffered -E '^[0-9:]{8} |rc=|^results:|STATION DOWN|WARNING' | sed -u "s/^/   $t: /"
    rc=${PIPESTATUS[0]}
    [ "${BRIDGE:-}" = 1 ] && stop_remote
    if [ "$rc" = 4 ]; then
        echo "$(date +%T) STATION DOWN during $t's run; batch stopped"
        exit 4
    fi
    R=$(ls -d "$RES/ogles2-$t"-2[0-9]* 2>/dev/null | tail -1)
    if [ -z "$R" ] || [[ "${R##*ogles2-$t-}" < "$stamp" ]]; then
        echo "$(date +%T) $t: NO RESULTS from this run"
        continue
    fi
    echo "$(date +%T) $t: $(grep -h '^PASS:' "$R"/results/*.txt 2>/dev/null | wc -l) PASS," \
        "$(grep -h '^FAIL:' "$R"/results/*.txt 2>/dev/null | wc -l) FAIL ($R)"
    # dEQP's logs: each one's status counts
    for q in "$R"/results/*.qpa; do
        [ -f "$q" ] || continue
        echo "   $t: $(basename "$q" .qpa): $(grep -o 'StatusCode="[A-Za-z]*"' "$q" | sed 's/StatusCode=//' |
            sort | uniq -c | tr -s ' \n' ' ')"
    done
done
