"""hotline.py LISTING ASM_DA SOURCE FUNC HOTPC_OUTPUT [N]: hotpc.py's samples in FUNC by C
source line, the N most (default 25).
LISTING: the object's assembler listing (-Wa,-adhln: section offsets); ASM_DA: the same
compile's gcc -S -g -dA output, whose annotated line table names each .LM label's line;
SOURCE: the C file (tools/perf/listing.sh makes both). The line of the last .LM label
before an instruction is GCC's: with scheduling it is approximate.
"""
import collections
import re
import sys

lst, asm, srcf, func, hot = sys.argv[1:6]
N = int(sys.argv[6]) if len(sys.argv) > 6 else 25
lm_line = {}
cur = None
for l in open(asm, errors="replace"):
    m = re.search(r"set address \*(\.LM\d+)", l)
    if m:
        cur = m.group(1)
        continue
    m = re.search(r"\|.*\bline (\d+)", l)
    if m and cur:
        lm_line[cur] = int(m.group(1))
        cur = None
infunc = False
start = None
marks = []  # (offset from the function's start, line)
last_line = None
for l in open(lst, errors="replace"):
    if re.search(r"\t_%s:\s*$" % re.escape(func), l):
        infunc = True
        continue
    if not infunc:
        continue
    if re.search(r"\t_[A-Za-z0-9_.]+:\s*$", l) and ".L" not in l:
        break
    m = re.search(r"\t(\.LM\d+):", l)
    if m and m.group(1) in lm_line:
        last_line = lm_line[m.group(1)]
        continue
    m = re.match(r"^\s*\d+\s+([0-9a-f]{4}) [0-9A-F]{4}", l)
    if m:
        off = int(m.group(1), 16)
        if start is None:
            start = off
        marks.append((off - start, last_line))
src = open(srcf, errors="replace").read().split("\n")
by = collections.Counter()
total = 0
for l in open(hot):
    m = re.match(r"\s*\+0x([0-9a-f]+)\s+(\d+)", l)
    if not m:
        continue
    off, c = int(m.group(1), 16), int(m.group(2))
    line = None
    for o, ln in marks:
        if o <= off:
            line = ln
        else:
            break
    by[line] += c
    total += c
print("%s: %d samples" % (func, total))
for ln, c in by.most_common(N):
    print("%5d %4d  %s" % (ln or 0, c, src[ln - 1].strip()[:90] if ln else "?"))
