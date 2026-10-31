#!/bin/bash
# The full dEQP runs (docs/GLES3_PLAN.md): every case list of a package
# (caselists/, the CTS's must-pass lists by group), one package run a list,
# watched (tools/amictl/deqp_watch.py), the shaders compiled on the PC
# (tools/amictl/compile_bridge.py, started here when it is not running; the
# A1200's v3dglslc-remote with NOFALLBACK: never a compile on the A1200).
#   bash ports/deqp/run_full.sh PACKAGE [LIST...]     (in WSL, after . ./local.env)
#     PACKAGE  m7-1 (dEQP-GLES2), m8-2 (dEQP-GLES3), m8-3 (dEQP-GLES31)
#     LIST     caselists/<LIST>.txt; default every list of the package
# Per list one line "GROUP <list> rc=<rc> <counts> <results dir>" in
# $LOGDIR/<package>.log (default ~/deqp-full) beside the watcher's lines.
# When the A1200 hangs or restarts (the watcher's exit 3): "HUNG <list>
# <case>" there and in $LOGDIR/hung.txt, then the station is waited for (a
# reset by hand) and the list goes on after that case (caselists/<list>-rest
# <n>.txt). When dEQP crashed (the watcher's other non-zero exits with a
# case started and not ended: a SmartCrash or Guru requester keeps the task
# and its open files, so later lists could not fetch their results):
# "CRASHED <list> <case>" there and in $LOGDIR/hung.txt, the A1200 rebooted
# (amictl reboot), and the list goes on after that case the same way.
# Totals: python3 ports/deqp/summary.py tests/a1200/results/... .
set -u
cd "$(dirname "$0")/../.."
PKG=${1:?package (m7-1, m8-2, m8-3)}
shift
X="python3 tools/amictl/amictl.py"
OUTPKG=tests/a1200/out/ogles2-$PKG
P=Stuff:amictld/work/ogles2-$PKG
ES3=${ES3_BUILD:-$HOME/ogles3-build/m68k-es3}
LOGDIR=${LOGDIR:-$HOME/deqp-full}
mkdir -p "$LOGDIR"
LOG=$LOGDIR/$PKG.log
[ -d $OUTPKG/caselists ] || { echo "no $OUTPKG/caselists: bash tests/a1200/build_package.sh $PKG"; exit 1; }
[ -x $ES3/amictl/v3dglslc-remote ] || { echo "no $ES3/amictl/v3dglslc-remote: make ES3=1 B=$ES3"; exit 1; }
if [ $# -gt 0 ]; then lists="$*"; else lists=$(ls $OUTPKG/caselists | grep -v -- '-rest[0-9]*\.txt$' | sed 's/\.txt$//'); fi

bridge() {
    pgrep -f compile_bridge.py > /dev/null && return
    nohup python3 tools/amictl/compile_bridge.py >> "$LOGDIR/bridge.log" 2>&1 &
    sleep 3
    echo "$(date +%T) compile bridge started" | tee -a "$LOG"
}

# run_list NAME: one package run of caselists/NAME.txt; its watcher's exit status
run_list() {
    local name=$1 job rc d
    bridge
    $X put $ES3/amictl/v3dglslc-remote $P/bin/v3dglslc-remote > /dev/null 2>&1 || true
    job=$($X start -d $P bin/v3dglslc-remote WAIT NOFALLBACK COMPILER=$P/libs/v3dglslc3 2>/dev/null | tail -1)
    python3 tools/amictl/deqp_watch.py $OUTPKG "LIST=$name NAME=$name" --every ${WATCH_EVERY:-30} \
        --timeout ${TIMEOUT:-43200} >> "$LOG" 2>&1
    rc=$?
    d=$(ls -d tests/a1200/results/ogles2-$PKG-2* | tail -1)
    echo "$(date +%T) GROUP $name rc=$rc $(grep -o 'StatusCode="[A-Za-z]*"' $d/results/$name.qpa 2>/dev/null |
        sed 's/StatusCode=//; s/"//g' | sort | uniq -c | tr -s ' ' | tr '\n' ' ') $(basename $d)" | tee -a "$LOG"
    if [ $rc -ne 3 ]; then
        $X abort "$job" > /dev/null 2>&1
        $X wait "$job" 30 > /dev/null 2>&1
    fi
    LAST_RESULTS=$d
    return $rc
}

for L in $lists; do
    name=$L
    n=0
    while :; do
        run_list $name
        rc=$?
        [ $rc -eq 0 ] && break
        # the case that was running: the last "S" line without its "E"/"T"
        hung=$(awk '$1 == "S" {c = $2} $1 == "E" || $1 == "T" {c = ""} END {print c}' \
            $LAST_RESULTS/results/live-progress.txt 2>/dev/null)
        if [ $rc -eq 3 ]; then
            echo "$(date +%T) HUNG $L ${hung:-unknown} ($(basename $LAST_RESULTS))" | tee -a "$LOG" "$LOGDIR/hung.txt"
        else
            [ -n "$hung" ] || break # a failure of the run itself, not of a case
            echo "$(date +%T) CRASHED $L $hung ($(basename $LAST_RESULTS))" | tee -a "$LOG" "$LOGDIR/hung.txt"
            $X reboot > /dev/null 2>&1
            sleep 20
        fi
        $X up 7200 > /dev/null 2>&1 || { echo "$(date +%T) the station did not come back: stopped" | tee -a "$LOG"; exit 3; }
        echo "$(date +%T) the station is back" | tee -a "$LOG"
        [ -n "$hung" ] || break
        n=$((n + 1))
        # the rest of the list after the case that hung
        awk -v h="$hung" 'f {print} $0 == h {f = 1}' $OUTPKG/caselists/$name.txt > $OUTPKG/caselists/$L-rest$n.txt
        [ -s $OUTPKG/caselists/$L-rest$n.txt ] || break
        name=$L-rest$n
    done
done
echo "$(date +%T) DONE $PKG" | tee -a "$LOG"
