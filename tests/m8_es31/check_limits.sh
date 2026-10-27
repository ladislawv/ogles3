#!/bin/bash
# The library's OpenGL ES 3.1 limits against the compiler's (docs/GLES3_PLAN.md
# §7): `m8_es31 LIMITS` under vamos with an ES31=1 build, and `v3dglslc -L`,
# which must agree line for line (ES 3.0's: tests/m8_es3/check_limits.sh).
# Run in WSL:
#   B=~/ogles3-build/m68k-es31 V3DGLSLC=~/ogles3-build/v3dglslc/host/v3dglslc \
#       bash tests/m8_es31/check_limits.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B=${B:-$HOME/ogles3-build/m68k-es31}
V3DGLSLC=${V3DGLSLC:-$HOME/ogles3-build/v3dglslc/host/v3dglslc}
B=$B bash "$HERE/../vamos.sh" m8_es31 LIMITS NOGPU > "$B/limits31_lib.txt"
grep -q '^GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS ' "$B/limits31_lib.txt" || { cat "$B/limits31_lib.txt"; exit 1; }
"$V3DGLSLC" -L | grep -f <(cut -d' ' -f1 "$B/limits31_lib.txt" | sed 's/^/^/; s/$/ /') > "$B/limits31_compiler.txt"
if diff "$B/limits31_compiler.txt" "$B/limits31_lib.txt"; then
    echo "limits: identical ($(wc -l < "$B/limits31_lib.txt") lines)"
else
    echo "limits: DIFFERENT (< compiler, > library)"
    exit 1
fi
