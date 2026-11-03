#!/bin/bash
# Juggler RT's PC check (tools/pccheck.c): builds it with the host's gcc,
# Mesa's EGL and GLES, and runs it. usage: tools/pccheck.sh OUTDIR [robot.rgb]
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
mkdir -p "$OUT"
make -C "$HERE" -s header
gcc -O2 -Wall -o "$OUT/pccheck" "$HERE/tools/pccheck.c" -lEGL -lGLESv2 -lm
"$OUT/pccheck" "$OUT" "${2:-}"
