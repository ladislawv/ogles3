"""The library's functions at their places in the loaded library, for the PC sampler's
profiles (libprof.py, hotpc.py): every object's own code-hunk symbols (static functions
too: hunksyms.py) placed at the object's .text address in the link map
(ogles3.library.map, ogles2.library.map), and the load address from pc_sample's
'lib NAME LOW HIGH' line: LOW is the lowest vector target, the lowest of library.c's
LibVectors functions. The objects must be the build that ran (the map names them).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hunksyms  # noqa: E402

VECTORS = {"LibOpen", "LibClose", "LibExpunge", "LibReserved", "LibGetDispatchTable", "LibPrivateLogFlush"}


def from_map(mapf):
    """[(offset in the library's code, 'object:function', code hunk index)], sorted."""
    syms = []
    cache = {}
    seen = {}
    for line in open(mapf):
        m = re.match(r"^ \.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if not m or int(m.group(2), 16) == 0:
            continue
        base, obj = int(m.group(1), 16), m.group(3)
        am = re.match(r"(.*\.a)\((.*)\)$", obj)
        path, member = (am.group(1), am.group(2)) if am else (obj, None)
        if path not in cache:
            cache[path] = hunksyms.symbols(path)
        tab = cache[path]
        key = member if member else (list(tab.keys())[0] if tab else None)
        secs = tab.get(key, [])
        k = seen.get(obj, 0)  # an object's second .text entry is its second code hunk
        seen[obj] = k + 1
        short = (member or path.rsplit("/", 1)[-1]).replace(".o", "")
        for off, n in (secs[k] if k < len(secs) else []):
            syms.append((base + off, short + ":" + n, k))
    syms.sort()
    return syms


def load_base(syms, pc_txt, libname):
    voff = min(a for a, n, k in syms if n.split(":")[1] in VECTORS)
    for line in open(pc_txt):
        p = line.split()
        if p and p[0] == "lib" and p[1] == libname:
            return int(p[2], 16) - voff
    raise SystemExit("%s: no 'lib %s' line" % (pc_txt, libname))


def samples(pc_txt):
    """{pc: ready samples} from pc_sample's 'pc ADDRESS COUNT' lines, and their total."""
    pcs = {}
    total = 0
    for line in open(pc_txt):
        p = line.split()
        if p and p[0] == "pc":
            pcs[int(p[1], 16)] = pcs.get(int(p[1], 16), 0) + int(p[2])
            total += int(p[2])
    return pcs, total
