#!/bin/bash
# Runs dEQP cases on the A1200 with their shaders compiled on the PC
# (docs/GLES3_PLAN.md §1, "Compiling on the PC"), and prints the results'
# status counts.
#   usage: bash ports/deqp/run_pc.sh [--package m8-2|m7-1] [--images] [--build] PATTERN NAME
#     PATTERN  a dEQP case pattern, e.g. 'dEQP-GLES3.functional.transform_feedback.*'
#     NAME     the log's name: results/NAME.qpa in the run's results directory
#     --images keep the images in the log (Run-M1 IMAGES=1; ports/deqp/qpa_images.py)
#     --build  build the package first (tests/a1200/build_package.sh)
# Needs, after `. ./local.env` in WSL: the station (tools/amictl, station.local),
# `python3 tools/amictl/compile_bridge.py` running in another shell (it runs
# the host v3dglslc), and the package run once before (its drawer on the
# A1200). Without a bridge v3dglslc-remote hands every request to the
# A1200's own compiler: the run is right, only slow.
set -u
PKG=m8-2
KEYS=""
BUILD=0
while [ $# -gt 2 ]; do
    case "$1" in
        --package) PKG=$2; shift 2 ;;
        --images) KEYS="IMAGES=1"; shift ;;
        --build) BUILD=1; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
[ $# -eq 2 ] || { sed -n 4,8p "$0"; exit 2; }
PATTERN=$1
NAME=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
X="python3 tools/amictl/amictl.py"
P=Stuff:amictld/work/ogles2-$PKG # the station's work drawer for the package
ES3=${ES3_BUILD:-$HOME/ogles3-build/m68k-es3}

[ $BUILD = 1 ] && { bash tests/a1200/build_package.sh $PKG 2>&1 | tail -1; }
[ -d tests/a1200/out/ogles2-$PKG ] || { echo "no package tests/a1200/out/ogles2-$PKG: --build"; exit 1; }
[ -x $ES3/amictl/v3dglslc-remote ] || { echo "no $ES3/amictl/v3dglslc-remote: make ES3=1 B=$ES3"; exit 1; }
$X put $ES3/amictl/v3dglslc-remote $P/bin/v3dglslc-remote > /dev/null ||
    { echo "cannot put v3dglslc-remote into $P/bin: run the package once without the PC (amictl runpkg)"; exit 1; }
job=$($X start -d $P bin/v3dglslc-remote WAIT COMPILER=$P/libs/v3dglslc)
$X runpkg tests/a1200/out/ogles2-$PKG -t ${TIMEOUT:-3000} -a "CASES=$PATTERN NAME=$NAME${KEYS:+ $KEYS}" | tail -1
d=$(ls -d tests/a1200/results/ogles2-$PKG-2* | tail -1)
echo "$NAME ($d): $(grep -o 'StatusCode="[A-Za-z]*"' $d/results/$NAME.qpa 2>/dev/null |
    sed 's/StatusCode=//' | sort | uniq -c | tr '\n' ' ')"
$X abort $job > /dev/null
$X wait $job 30 | tail -1
