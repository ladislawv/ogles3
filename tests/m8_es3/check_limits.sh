#!/bin/bash
# The library's OpenGL ES 3.0 limits against the compiler's (docs/GLES3_PLAN.md
# §4 item 1): `m8_es3 LIMITS` under vamos with an ES3=1 build, and
# `v3dglslc -L`, which must agree line for line (GLSL's gl_Max* constants and
# the compiler's checks come from the same values). Run in WSL:
#   B=~/ogles3-build/m68k-es3 V3DGLSLC=~/ogles3-build/v3dglslc/host/v3dglslc \
#       bash tests/m8_es3/check_limits.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B=${B:-$HOME/ogles2-build/m68k-es3}
V3DGLSLC=${V3DGLSLC:-$HOME/ogles2-build/v3dglslc/host/v3dglslc}
B=$B bash "$HERE/../vamos.sh" m8_es3 LIMITS NOGPU > "$B/limits_lib.txt"
grep -q '^GL_MAX_VERTEX_ATTRIBS ' "$B/limits_lib.txt" || { cat "$B/limits_lib.txt"; exit 1; }
# The compiler's list for the names the library's has (ES 3.1's come in stage D).
"$V3DGLSLC" -L | grep -f <(cut -d' ' -f1 "$B/limits_lib.txt" | sed 's/^/^/; s/$/ /') > "$B/limits_compiler.txt"
if diff "$B/limits_compiler.txt" "$B/limits_lib.txt"; then
    echo "limits: identical ($(wc -l < "$B/limits_lib.txt") lines)"
else
    echo "limits: DIFFERENT (< compiler, > library)"
    exit 1
fi
