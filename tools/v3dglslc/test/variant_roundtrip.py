#!/usr/bin/env python3
"""The draw-time variants' round trip (docs/GLES3_PLAN.md stage A item 5):
every VARIANT key of a program's .v3dp, compiled again on its own through
`v3dglslc -x` (Mesa patch 0015), must give the same variant, byte for byte,
in the program's .v3dx; that file's VARIANT_OF must be the CRC of the .v3dp's
STAGE_PARAMS, UNIFORMS and ATTRIBUTES; and a second run must add nothing
(the merge with the existing file). Written from FORMAT.md.

  usage: variant_roundtrip.py V3DGLSLC DIR VS FS
DIR holds VS and FS and gets the files (the .v3dp first, written here).
"""
import os, struct, subprocess, sys, zlib

KEY_BYTES = {0: 88, 1: 88, 4: 296}


def sections(path):
    d = open(path, "rb").read()
    assert d[:4] == b"V3DP", path
    major, nsec = struct.unpack_from("<H", d, 4)[0], struct.unpack_from("<I", d, 24)[0]
    assert zlib.crc32(d[32:]) == struct.unpack_from("<I", d, 28)[0], "CRC of " + path
    out, o = [], 32
    for _ in range(nsec):
        t, n = struct.unpack_from("<II", d, o)
        out.append((t, d[o + 8:o + 8 + n], d[o:o + 8 + n + (-n & 3)]))
        o += 8 + n + (-n & 3)
    return major, out


def run(v3dglslc, d, args):
    p = subprocess.run([v3dglslc] + args, cwd=d, capture_output=True, text=True)
    if p.returncode:
        sys.exit("v3dglslc %s failed:\n%s%s" % (" ".join(args), p.stdout, p.stderr))


def main():
    v3dglslc, d, vs, fs = sys.argv[1:5]
    base = "%s-%s" % (os.path.splitext(vs)[0], os.path.splitext(fs)[0])
    run(v3dglslc, d, ["-o", ".", vs, fs])
    major, secs = sections(os.path.join(d, base + ".v3dp"))
    variants = [p for t, p, _ in secs if t == 6]
    layout = b"".join(raw for t, _, raw in secs if t in (3, 4, 5))
    keys = b""
    for p in variants:
        stage = struct.unpack_from("<I", p)[0]
        keys += p[:4 + KEY_BYTES[stage]]
    open(os.path.join(d, base + ".keys"), "wb").write(keys)
    xpath = os.path.join(d, base + ".v3dx")
    if os.path.exists(xpath):
        os.remove(xpath)
    for attempt in (1, 2):
        run(v3dglslc, d, ["-x", base + ".keys", "-o", ".", vs, fs])
        xmajor, xsecs = sections(xpath)
        types = [t for t, _, _ in xsecs]
        if xmajor != major or types[:3] != [1, 2, 9]:
            sys.exit("%s: header %d or sections %s" % (xpath, xmajor, types[:3]))
        if xsecs[0][1] != secs[0][1]:
            sys.exit("%s: PROGRAM differs from the .v3dp's" % xpath)
        if struct.unpack_from("<I", xsecs[2][1])[0] != zlib.crc32(layout):
            sys.exit("%s: VARIANT_OF is not the .v3dp's layout CRC" % xpath)
        dump = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                              "..", "..", "..", "v3dp", "v3dp_dump.py"), xpath],
                              capture_output=True, text=True)
        if dump.returncode:
            sys.exit("v3dp_dump.py refuses %s: %s" % (xpath, dump.stderr))
        xvars = [p for t, p, _ in xsecs if t == 6]
        if sorted(xvars) != sorted(variants):
            same = sum(1 for p in variants if p in xvars)
            sys.exit("%s, run %d: %d variants, %d of the .v3dp's %d the same" %
                     (xpath, attempt, len(xvars), same, len(variants)))
    print("%s: %d variants compiled again from their keys, byte for byte; merged twice" %
          (base, len(variants)))


if __name__ == "__main__":
    main()
