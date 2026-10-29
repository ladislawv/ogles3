"""libprof.py PC.TXT LIB.map LIBNAME [N]: pc_sample's ready samples inside LIBNAME by
function, the N most (default 40): 'count object:function' lines after a header line.
PC.TXT: pc_sample's output (the Doom 3 port's tests/a1200/pc-sample; tools/perf/
drawbench.sh and q3cmp.sh run it); LIB.map: the library's link map, its objects the
build that ran; LIBNAME: ogles3.library or ogles2.library.
A sample falls at a translated block's start (Emu68 stops a task there): loop heads get
more than their share, so compare runs, not lines.
"""
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import libsyms  # noqa: E402

pc_txt, mapf, libname = sys.argv[1:4]
N = int(sys.argv[4]) if len(sys.argv) > 4 else 40
syms = libsyms.from_map(mapf)
base = libsyms.load_base(syms, pc_txt, libname)
pcs, total = libsyms.samples(pc_txt)
addrs = [a for a, n, k in syms]
end = syms[-1][0] + 0x4000
by = collections.Counter()
inlib = 0
for pc, c in pcs.items():
    o = pc - base
    if 0 <= o < end:
        i = bisect.bisect_right(addrs, o) - 1
        by[syms[i][1] if i >= 0 else "?"] += c
        inlib += c
print("%s: %d ready samples, %d in the library (%.1f%%)" % (libname, total, inlib, 100.0 * inlib / max(total, 1)))
for n, c in by.most_common(N):
    print("%6d %s" % (c, n))
