#!/bin/bash
# Applies v3dglslc's Mesa patches (tools/v3dglslc/patches/*.patch, in order)
# to a Mesa tree at the pinned commit. build_mesa.sh checks out the commit
# only; the compiler needs these changes in the tree it builds from.
#   usage: apply_mesa_patches.sh [MESA_TREE]   (default ~/mesa-build/mesa)
# A patch already applied is skipped, so it can run again. Then rebuild:
#   ninja -C MESA_TREE/build && ninja -C MESA_TREE/build install
# On WSL1 also run patch_drmshim.sh (drm-shim's memfd_create fallback).
set -eu
M=${1:-$HOME/mesa-build/mesa}
P="$(cd "$(dirname "$0")/../../tools/v3dglslc/patches" && pwd)"
cd "$M"
for f in "$P"/[0-9]*.patch; do
    n=$(basename "$f")
    if git apply --check "$f" 2>/dev/null; then
        git apply "$f"
        echo "applied  $n"
    elif git apply --check --reverse "$f" 2>/dev/null; then
        echo "present  $n"
    else
        echo "FAILED   $n: neither applies nor is already applied (another Mesa commit?)"
        exit 1
    fi
done
git log -1 --format='mesa %h' && git diff --stat | tail -1
