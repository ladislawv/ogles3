#!/usr/bin/env python3
"""Writes the separable geometry programs tests/m8_es31 links (make test-v3dp):
es32gs's geometry shader alone, and with its vertex shader, through the
compiler's SERVE requests (V3DGLSL_LINK_SEPARABLE; the command line links no
separable programs). The files are named by their hash afterwards
(v3dp_hashname.py), as the runtime looks for them.
usage: separable_gs.py V3DGLSLC SRC_DIR OUT_DIR"""
import os
import struct
import subprocess
import sys

V3DGLSL_LINK_SEPARABLE = 10


def string(b):
    return struct.pack(">I", 0xFFFFFFFF) if b is None else struct.pack(">I", len(b)) + b


def main():
    compiler, src, out = sys.argv[1:4]
    vs = open(os.path.join(src, "es32gs.vert"), "rb").read()
    gs = open(os.path.join(src, "es32gs.geom"), "rb").read()
    bindings = b"GEOM %d\n" % len(gs) + gs  # V3DGLSL_GEOM_MARK
    for name, v in (("es32gs-sep-g", None), ("es32gs-sep-vg", vs)):
        path = os.path.join(out, name + ".v3dp")
        if os.path.exists(path):
            os.remove(path)
        req = (b"V3Q1" + struct.pack(">II", V3DGLSL_LINK_SEPARABLE, 0) + string(v) + string(None) +
               string(bindings) + string(None) + string(path.encode()))
        p = subprocess.run([compiler, "SERVE"], input=req, capture_output=True)
        if p.returncode or not os.path.exists(path):
            sys.exit("separable_gs.py: %s not linked:\n%s" % (name, p.stderr.decode(errors="replace")[-800:]))
        print("%s: %d bytes" % (path, os.path.getsize(path)))


main()
