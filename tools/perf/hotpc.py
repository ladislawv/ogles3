"""hotpc.py PC.TXT LIB.map LIBNAME FUNC...: the samples inside those functions by offset
from the function's start ('+0xOFFSET COUNT' lines after 'FUNC (section K): N samples'),
for hotline.py. Arguments as libprof.py's; FUNC without its object (draw_arrays).
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import libsyms  # noqa: E402

pc_txt, mapf, libname = sys.argv[1:4]
funcs = sys.argv[4:]
syms = libsyms.from_map(mapf)
base = libsyms.load_base(syms, pc_txt, libname)
pcs, total = libsyms.samples(pc_txt)
addrs = [a for a, n, k in syms]
for f in funcs:
    out = []
    for pc, c in pcs.items():
        o = pc - base
        i = bisect.bisect_right(addrs, o) - 1
        if i >= 0 and syms[i][1].split(":", 1)[1] == f:
            out.append((o - syms[i][0], c, syms[i][2]))
    out.sort()
    print("%s (section %s): %d samples" % (f, out[0][2] if out else "?", sum(c for _, c, _ in out)))
    for off, c, _ in out:
        print("  +0x%04x %4d" % (off, c))
