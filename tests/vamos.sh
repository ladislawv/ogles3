#!/bin/bash
# Runs a state-only test (one that takes NOGPU: smoke, m2_link, m8_es3, ...)
# under vamos, the AmigaOS emulator, with the build's ogles2.library in LIBS:.
# No GPU and no FPU there: only what needs neither (contexts, queries,
# program tables, errors). Run in WSL:
#   B=~/ogles2-build/m68k bash tests/vamos.sh smoke NOGPU
#   B=~/ogles3-build/m68k-es3 bash tests/vamos.sh m8_es3 NOGPU ES3
# The scratch volume ($B/vamos) is the current directory, T:, ENV: and
# OGLES2Cache:. vamos prints a Python traceback when it expunges the library
# at the end (its Forbid() has no task then): harmless, filtered out here.
set -u
B=${B:-$HOME/ogles2-build/m68k}
VAMOS=${VAMOS:-$HOME/amitools-venv/bin/vamos}
T=${1:?usage: tests/vamos.sh <test> [args...]}
shift
[ -f "$B/tests/$T" ] && [ -f "$B/ogles2.library" ] || { echo "no $B/tests/$T or $B/ogles2.library: make first"; exit 1; }
W=$B/vamos
mkdir -p "$W/t" "$W/libs" "$W/cache"
cp "$B/ogles2.library" "$W/libs/"
cp "$B/tests/$T" "$W/"
cd "$W"
timeout 300 "$VAMOS" -C 68040 -H disable -m 65536 -s 256 -V "w:$W" \
    -a LIBS:w:libs -a T:w:t -a ENV:w:t -a OGLES2Cache:w:cache -- "$T" "$@" 2>&1 |
    grep -vE 'WARNING|^  |^Traceback|Error:|^    '
