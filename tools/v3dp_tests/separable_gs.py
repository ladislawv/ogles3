#!/usr/bin/env python3
"""Writes the separable geometry programs and the program pipeline the tests
link (make test-v3dp), through the compiler's SERVE requests (the command
line links no separable programs and no pipelines):
- es32gs's geometry shader alone, and with its vertex shader (m8_es31);
- es32gsp's vertex, geometry and fragment shaders each a separable program,
  and their pipeline's variants (V3DGLSL_LINK_PIPELINE with "PIP3 ... G"),
  named as the library looks for them: its hash's first 24 hex digits
  ".v3dx", its VARIANT_OF the layout CRC the library composes from the
  three programs' (v3dp.c's ogles2_v3dp_compose).
The program files are named by their hash afterwards (v3dp_hashname.py).
usage: separable_gs.py V3DGLSLC SRC_DIR OUT_DIR"""
import os
import struct
import subprocess
import sys
import zlib

V3DGLSL_LINK_SEPARABLE = 10
V3DGLSL_LINK_PIPELINE = 11


def string(b):
    return struct.pack(">I", 0xFFFFFFFF) if b is None else struct.pack(">I", len(b)) + b


def serve(compiler, op, vs, fs, bindings, path, crc=0):
    req = (b"V3Q1" + struct.pack(">II", op, crc) + string(vs) + string(fs) + string(bindings) + string(None) +
           string(path.encode()))
    return subprocess.run([compiler, "SERVE"], input=req, capture_output=True)


def geom(gs):
    return b"GEOM %d\n" % len(gs) + gs  # V3DGLSL_GEOM_MARK


def file_hash_and_crc(path):
    """The PROGRAM section's hash, and the layout CRC the library's loader
    takes: STAGE_PARAMS to ATTRIBUTES (sections 3 to 5), headers and padding."""
    d = open(path, "rb").read()
    o, start, end = 32, None, None
    while o < len(d):
        t, n = struct.unpack("<II", d[o:o + 8])
        if 3 <= t <= 5:
            if start is None:
                start = o
            end = o + 8 + ((n + 3) & ~3)
        o += 8 + ((n + 3) & ~3)
    return d[40:72], (zlib.crc32(d[start:end]) & 0xFFFFFFFF) if start is not None else 0


def rotl(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def main():
    compiler, src, out = sys.argv[1:4]
    read = lambda n: open(os.path.join(src, n), "rb").read()

    vs, gs = read("es32gs.vert"), read("es32gs.geom")
    for name, v in (("es32gs-sep-g", None), ("es32gs-sep-vg", vs)):
        path = os.path.join(out, name + ".v3dp")
        if os.path.exists(path):
            os.remove(path)
        p = serve(compiler, V3DGLSL_LINK_SEPARABLE, v, None, geom(gs), path)
        if p.returncode or not os.path.exists(path):
            sys.exit("separable_gs.py: %s not linked:\n%s" % (name, p.stderr.decode(errors="replace")[-800:]))
        print("%s: %d bytes" % (path, os.path.getsize(path)))

    # the pipeline of three separable programs
    pv, pg, pf = read("es32gsp.vert"), read("es32gsp.geom"), read("es32gsp.frag")
    files = []
    for name, v, f, b in (("es32gsp-sep-v", pv, None, None), ("es32gsp-sep-g", None, None, geom(pg)),
                          ("es32gsp-sep-f", None, pf, None)):
        path = os.path.join(out, name + ".v3dp")
        if os.path.exists(path):
            os.remove(path)
        p = serve(compiler, V3DGLSL_LINK_SEPARABLE, v, f, b, path)
        if p.returncode or not os.path.exists(path):
            sys.exit("separable_gs.py: %s not linked:\n%s" % (name, p.stderr.decode(errors="replace")[-800:]))
        files.append(file_hash_and_crc(path))
    (ha, ca), (hg, cg), (hb, cb) = files
    crc = ca ^ rotl(cb, 7) ^ 0x50495045 ^ rotl(cg, 13) ^ 0x50495047
    line = b"PIP3 " + ha.hex().encode() + b" " + hg.hex().encode() + b" " + hb.hex().encode() + b" G\n"
    tmp = os.path.join(out, "es32gsp-pipeline.v3dp")
    x = tmp[:-5] + ".v3dx"
    if os.path.exists(x):
        os.remove(x)
    p = serve(compiler, V3DGLSL_LINK_PIPELINE, pv, pf, geom(pg) + line + b"SEPA\n", tmp, crc)
    if p.returncode or not os.path.exists(x):
        sys.exit("separable_gs.py: the pipeline not linked:\n%s" % p.stderr.decode(errors="replace")[-800:])
    named = os.path.join(out, open(x, "rb").read()[40:72].hex()[:24] + ".v3dx")
    os.replace(x, named)
    print("%s: the pipeline's variants, %d bytes" % (named, os.path.getsize(named)))


main()
