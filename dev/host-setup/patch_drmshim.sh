#!/bin/bash
# WSL1 has no memfd_create(); let drm-shim fall back to an unlinked temp file.
# Works on $MESA (default ~/mesa-build/mesa), as build_mesa.sh does: the gles3
# copy's local.env names its own tree.
set -e
MESA=${MESA:-$HOME/mesa-build/mesa}
W=$(dirname "$MESA")
F=$MESA/src/drm-shim/device.c
if ! grep -q "WSL1 fallback" "$F"; then
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '   shim_device.mem_fd = memfd_create("shim mem", MFD_CLOEXEC);\n'
new = (old +
       '   if (shim_device.mem_fd == -1) {\n'
       '      /* WSL1 fallback: no memfd_create(), use an unlinked temp file. */\n'
       '      char tmpl[] = "/tmp/drm-shim-mem-XXXXXX";\n'
       '      shim_device.mem_fd = mkstemp(tmpl);\n'
       '      if (shim_device.mem_fd != -1)\n'
       '         unlink(tmpl);\n'
       '   }\n')
assert old in s
s = s.replace(old, new)
if '#include <stdlib.h>' not in s:
    s = '#include <stdlib.h>\n' + s
if '#include <unistd.h>' not in s:
    s = '#include <unistd.h>\n' + s
open(p, 'w').write(s)
print("patched")
PY
fi
cd "$MESA"
git diff --stat
. "$W/venv/bin/activate"
ninja -C build -j24 >"$W/rebuild.log" 2>&1 && ninja -C build install >>"$W/rebuild.log" 2>&1 && echo rebuilt
git diff > "$W/drm-shim-wsl1.patch"
