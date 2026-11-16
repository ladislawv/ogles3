#!/bin/bash
# Test packages on the A1200 through amigent (C:\- CPP\amigent, the fork with
# amictl's moves: amigentd on port 6502, its amigent-ctl), as ogles2pi3's
# tests/a1200/p5_run.sh does for the Pi 3 card. Builds each package
# (tests/a1200/build_package.sh, in WSL with local.env), and runs its Run-*
# script with amigent-ctl runpkg (only changed files uploaded, a line a
# minute while it runs, the batch stopped at exit status 4: the station
# down). The programs compile on the Amiga: each package has v3dglslc3 in
# its libs/.
#   bash tests/a1200/a_run.sh m8-5 [m8-1 ...]       (Git Bash)
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
    stamp=$(date +%Y%m%d-%H%M%S)
    echo "$(date +%T) $t: uploading and running"
    ctl runpkg "$PKG" -t ${TIMEOUT:-1800} -w "$WORK" -r "$RES" ${ARGS:+-a "$ARGS"} 2>&1 |
        grep --line-buffered -E '^[0-9:]{8} |rc=|^results:|STATION DOWN|WARNING' | sed -u "s/^/   $t: /"
    rc=${PIPESTATUS[0]}
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
done
