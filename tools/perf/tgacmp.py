"""tgacmp.py A.tga B.tga: how many pixels differ, and by more than 16 and 48 levels
(uncompressed true-colour TGA, as Quake III's screenshots; q3cmp.sh shot). Two runs of
the same library differ by about 0.15% at frame 400 of demo four (2026-10-29)."""
import struct
import sys


def load(p):
    b = open(p, "rb").read()
    idl, it = b[0], b[2]
    w, h = struct.unpack("<HH", b[12:16])
    bpp = b[16] // 8
    assert it == 2, "uncompressed true-colour only"
    off = 18 + idl
    return w, h, bpp, b[off:off + w * h * bpp]


wa, ha, ca, a = load(sys.argv[1])
wb, hb, cb, bb = load(sys.argv[2])
assert (wa, ha, ca) == (wb, hb, cb), ((wa, ha, ca), (wb, hb, cb))
n = wa * ha
d0 = d16 = d48 = 0
for i in range(n):
    m = max(abs(a[i * ca + k] - bb[i * ca + k]) for k in range(3))
    d0 += m > 0
    d16 += m > 16
    d48 += m > 48
print("%dx%d: %d pixels differ (%.2f%%), %d by more than 16, %d by more than 48" % (wa, ha, d0, 100.0 * d0 / n, d16, d48))
