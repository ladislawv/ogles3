#!/bin/bash
# The library's compile of one source file, for reading its code (in WSL):
#   bash tools/perf/listing.sh ogles3/draw.c
# writes $PERF_WORK/draw.lst (the assembler listing with each section's
# offsets, -g -Wa,-adhln) and $PERF_WORK/draw.s (gcc -S -g -dA: its line table
# annotated, which hotline.py reads), from the Makefile's own command for
# $B/obj/ogles3/draw.o (-g changes no code). The object in $B must be the
# build that ran for the offsets to match a profile.
set -eu
. "$(dirname "$0")/common.sh"
f=$(basename "$1" .c)
cd "$REPO"
cmd=$(make -n -B B="$B" "$B/obj/ogles3/$f.o" 2> /dev/null | grep -- "-c ogles3/$f.c" | head -1)
[ -n "$cmd" ] || { echo "no compile command for ogles3/$f.c"; exit 1; }
eval "$(echo "$cmd" | sed "s| -o [^ ]*| -o $WORK/$f.o -g -Wa,-adhln=$WORK/$f.lst|")"
eval "$(echo "$cmd" | sed "s| -c ogles3/$f.c -o [^ ]*| -S -g -dA ogles3/$f.c -o $WORK/$f.s|")"
echo "$WORK/$f.lst, $WORK/$f.s"
