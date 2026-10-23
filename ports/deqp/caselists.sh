#!/bin/bash
# Splits the dEQP-GLES2 must-pass list (the CTS's AOSP list, the cases a
# conformant GLES2 implementation passes) into one file per group, for runs
# of a group at a time: info.txt, <group>.txt for each functional group,
# shaders-<subgroup>.txt for the shader tests (10,000 of the 17,000).
# MODULE=gles3 splits dEQP-GLES3's (gles3-main.txt, 44,768 cases, 21,239 of
# them shader tests) the same way.
#   bash ports/deqp/caselists.sh OUTDIR [CTS_SRC]
set -eu
OUT=${1:?output directory}
CTS_SRC=${2:-$HOME/vk-gl-cts}
MODULE=${MODULE:-gles2}
LIST=$CTS_SRC/external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main/$MODULE-main.txt
[ -f "$LIST" ] || { echo "no $LIST"; exit 1; }
mkdir -p "$OUT"
rm -f "$OUT"/*.txt
tr -d '\r' < "$LIST" | awk -F. -v out="$OUT" '
    $2 == "info"                          { f = "info" }
    $2 == "functional" && $3 == "shaders" { f = "shaders-" $4 }
    $2 == "functional" && $3 != "shaders" { f = $3 }
    NF > 1                                { print > (out "/" f ".txt") }'
wc -l "$OUT"/*.txt | sort -n | tail -n +1
