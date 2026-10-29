#!/usr/bin/env python3
"""Find a 32-bit multiply followed by a shift by a register count of its
result, in GCC assembly output (-S).

Emu68 (the PiStorm's 68k JIT) runs that pair wrongly: after muls.l/mulu.l
into dN (the 32-bit result forms), lsr.l Dx,dN (or any shift by a register
count) sees the product's upper 32 bits too: (1 * 0x9E3779B1) >> 28 came out
0xFFFFFFF9, not 9 (the A1200, 2026-10-29, docs/GLES3_PLAN.md). A shift by an
immediate count, swap, and the 64-bit forms (mulu.l Dx,Dh:Dl) were right.
ogles3's index caches hashed so, and their slots out of the tables wrote over
memory.

The shift is looked for in the three instructions after the multiply, until
dN is written by something else.
  usage: check_mulshift.py file.s [...]     exit status 1 if anything is found
"""
import re, sys

MUL = re.compile(r"^\s*mul[su]\.l\s+[^,]+,\s*(%?d[0-7])\s*$")
SHIFT_REG = re.compile(r"^\s*(?:ls|as|ro|rox)[lr]\.[bwl]\s+(%?d[0-7])\s*,\s*(%?d[0-7])\s*$")
WRITES = re.compile(r",\s*(%?d[0-7])\s*$")
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):")


def reg(r):
    return r.lstrip("%")


def scan(path):
    found = []
    func = "?"
    lines = [l.split("|")[0].rstrip() for l in open(path, errors="replace")]
    for n, line in enumerate(lines):
        m = LABEL.match(line)
        if m and not m.group(1).startswith("."):
            func = m.group(1)
        m = MUL.match(line)
        if not m:
            continue
        d = reg(m.group(1))
        for k in range(n + 1, min(n + 4, len(lines))):
            s = SHIFT_REG.match(lines[k])
            if s and reg(s.group(2)) == d:
                found.append("%s:%d (%s): %s / %s" % (path, n + 1, func, line.strip(), lines[k].strip()))
                break
            w = WRITES.search(lines[k])
            if w and reg(w.group(1)) == d:
                break
    return found


def main(argv):
    found = []
    for p in argv:
        found += scan(p)
    for f in found:
        print(f)
    print("%d multiply(s) followed by a shift by a register count (Emu68 runs them wrongly)" % len(found))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
