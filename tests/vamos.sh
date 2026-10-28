#!/bin/bash
# Runs a state-only test (one that takes NOGPU: smoke, m2_link, m8_es3, ...)
# under vamos, the AmigaOS emulator, with the build's ogles3.library in LIBS:.
# No GPU and no FPU there: only what needs neither (contexts, queries,
# program tables, errors). Run in WSL:
#   B=~/ogles2-build/m68k bash tests/vamos.sh smoke NOGPU
#   B=~/ogles3-build/m68k-es3 bash tests/vamos.sh m8_es3 NOGPU ES3
# The scratch volume ($B/vamos) is the current directory (with shaders/, and
# the build's compiled test programs from `make test-v3dp`, as PROGDIR:), T:,
# ENV: and OGLES3Cache:. vamos prints a Python traceback when it expunges the library
# at the end (its Forbid() has no task then): harmless, filtered out here.
set -u
B=${B:-$HOME/ogles2-build/m68k}
VAMOS=${VAMOS:-$HOME/amitools-venv/bin/vamos}
T=${1:?usage: tests/vamos.sh <test> [args...]}
shift
[ -f "$B/tests/$T" ] && [ -f "$B/ogles3.library" ] || { echo "no $B/tests/$T or $B/ogles3.library: make first"; exit 1; }
W=$B/vamos
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$W/t" "$W/libs" "$W/cache" "$W/shaders"
cp "$B/ogles3.library" "$W/libs/"
cp "$B/tests/$T" "$W/"
# The test shaders and their files (make test-v3dp), for tests that link.
cp "$REPO"/tools/v3dglslc/test/shaders/* "$W/shaders/"
sed -i 's/\r$//' "$W"/shaders/*
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$W/" 2>/dev/null || true
# Under vamos the library finds them in neither PROGDIR: nor OGLES3Cache:
# (vamos lists no assigns in the DOS list, so the cache is T:): in T:.
cp "$B"/v3dp/*.v3dp "$B"/v3dp/*.v3dw "$W/t/" 2>/dev/null || true
cd "$W"
timeout 300 "$VAMOS" -C 68040 -H disable -m 65536 -s 256 -V "w:$W" \
    -a LIBS:w:libs -a T:w:t -a ENV:w:t -a OGLES3Cache:w:cache -- "$T" "$@" 2> "$W/vamos.err"
rc=$?
# vamos's own messages: its errors, not its warnings or the expunge traceback.
grep -E 'ERROR' "$W/vamos.err" | grep -v 'Traceback'
exit $rc
