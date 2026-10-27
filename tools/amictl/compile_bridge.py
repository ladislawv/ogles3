#!/usr/bin/env python3
"""The PC's side of v3dglslc-remote (tools/amictl/server/v3dglslc_remote.c,
docs/GLES3_PLAN.md "Compiling on the PC"): connects to it on the A1200,
compiles each request ogles2.library sends there with the host v3dglslc
(SERVE: the same requests as frames on its stdin, the same files byte for
byte) and sends the written files back.

    python3 tools/amictl/compile_bridge.py [--port 6802] [--compiler PATH]
    python3 tools/amictl/compile_bridge.py --check

The station's address and token are amictl's (station.local). It runs until
stopped (Ctrl-C), connecting again whenever the link ends. --check needs no
Amiga: the test shaders (tools/v3dglslc/test/shaders) as LINK requests
through the same path, each .v3dp compared with the command line's.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import amictl  # noqa: E402  (its config: the station's address and token)

V3DGLSL_LINK, V3DGLSL_QUIT = 1, 2  # v3dp/v3dglsl_port.h
NULL = 0xFFFFFFFF


def default_compiler():
    return os.environ.get("V3DGLSLC", os.path.expanduser("~/ogles3-build/v3dglslc/host/v3dglslc"))


def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def string(b):
    return u32(NULL) if b is None else u32(len(b)) + b


class Compiler:
    """The host v3dglslc in SERVE mode; started again if it ends."""

    def __init__(self, path):
        self.path = path
        self.p = None
        self.log = open(os.path.join(tempfile.gettempdir(), "compile_bridge.log"), "ab")

    def start(self):
        self.p = subprocess.Popen([self.path, "SERVE"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.log)

    def restart(self):
        """Ended; the next request starts it again."""
        if self.p and self.p.poll() is None:
            self.p.stdin.close()
            self.p.wait()
        self.p = None

    def request(self, op, crc, vs, fs, bindings, keys, out_path):
        """(result, log) of one request; the compiler writes out_path's files."""
        if not self.p or self.p.poll() is not None:
            self.start()
        self.p.stdin.write(b"V3Q1" + u32(op) + u32(crc) + string(vs) + string(fs) + string(bindings) +
                           string(keys) + string(out_path.encode()))
        self.p.stdin.flush()
        head = self.p.stdout.read(12)
        if len(head) != 12 or head[:4] != b"V3A1":
            self.p.kill()
            self.p = None
            return 2, b"compile_bridge: the host compiler ended"
        result, n = struct.unpack(">iI", head[4:])
        return result, self.p.stdout.read(n)


def serve_request(comp, op, crc, vs, fs, bindings, keys, name, xfile):
    """One request in a directory of its own: (result, log, [(name, bytes)])
    with the files the compiler wrote or changed."""
    d = tempfile.mkdtemp(prefix="v3dr")
    try:
        sent = {}
        if xfile is not None and name and name.endswith(".v3dp"):
            sent[name[:-5] + ".v3dx"] = xfile
        for f, b in sent.items():
            with open(os.path.join(d, f), "wb") as fh:
                fh.write(b)
        out_path = os.path.join(d, name) if name else os.path.join(d, "none.v3dp")
        result, log = comp.request(op, crc, vs, fs, bindings, keys, out_path)
        files = []
        for f in sorted(os.listdir(d)):
            with open(os.path.join(d, f), "rb") as fh:
                b = fh.read()
            if sent.get(f) != b:
                files.append((f, b))
        return result, log, files
    finally:
        shutil.rmtree(d, ignore_errors=True)


def recv_all(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise ConnectionError("the A1200 closed the link")
        b += c
    return b


def recv_u32(s):
    return struct.unpack(">I", recv_all(s, 4))[0]


def recv_str(s):
    n = recv_u32(s)
    return None if n == NULL else recv_all(s, n)


def serve(host, port, token, comp):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"V3H1" + string(token.encode()))
    if recv_all(s, 4) != b"V3OK":
        raise ConnectionError("the A1200 refused the token")
    s.settimeout(None)  # requests come when a program links
    print("%s connected to %s:%d" % (time.strftime("%H:%M:%S"), host, port), flush=True)
    n = 0
    while True:
        if recv_all(s, 4) != b"V3R1":
            raise ConnectionError("an unreadable request")
        op, crc = recv_u32(s), recv_u32(s)
        vs, fs, bindings, keys, name, xfile = (recv_str(s) for _ in range(6))
        if op == V3DGLSL_QUIT:  # a library expunge: a compiler started anew, as the A1200's would be
            comp.restart()
            s.sendall(b"V3S1" + u32(0) + string(b"") + u32(0))
            continue
        t0 = time.time()
        result, log, files = serve_request(comp, op, crc, vs, fs, bindings, keys,
                                           name.decode("latin-1") if name else None, xfile)
        out = b"V3S1" + u32(result) + string(log) + u32(len(files))
        for f, b in files:
            out += string(f.encode()) + string(b)
        s.sendall(out)
        n += 1
        if n % 50 == 1 or result:
            print("%s request %d: op %d -> %d, %d file(s), %.0f ms" %
                  (time.strftime("%H:%M:%S"), n, op, result, len(files), (time.time() - t0) * 1000), flush=True)


def check(comp):
    """The test shaders through serve_request against the command line's files,
    each in a compiler of its own: a program's default variants can depend on
    the one compiled before it in the same process (es3uni after es3tex;
    docs/GLES3_PLAN.md), as in the A1200's server."""
    sh = os.path.join(os.path.dirname(HERE), "v3dglslc", "test", "shaders")
    ref = tempfile.mkdtemp(prefix="v3dref")
    bad = n = 0
    try:
        for f in sorted(os.listdir(sh)):
            if not f.endswith(".vert") or not os.path.exists(os.path.join(sh, f[:-5] + ".frag")):
                continue
            v, fr = os.path.join(sh, f), os.path.join(sh, f[:-5] + ".frag")
            subprocess.run([comp.path, "-o", ref, v, fr], capture_output=True)
            want = os.path.join(ref, "%s-%s.v3dp" % (f[:-5], f[:-5]))
            vs, fs = open(v, "rb").read(), open(fr, "rb").read()
            one = Compiler(comp.path)
            result, log, files = serve_request(one, V3DGLSL_LINK, 0, vs, fs, None, None, "p.v3dp", None)
            one.p.stdin.close()
            one.p.wait()
            got = dict(files).get("p.v3dp")
            same = os.path.exists(want) and got == open(want, "rb").read()
            n += 1
            if not same:
                bad += 1
                print("DIFFERS: %s (result %d, %s)" % (f[:-5], result, log.decode("latin-1").strip()[:200]))
    finally:
        shutil.rmtree(ref, ignore_errors=True)
    print("%d of %d programs identical to the command line's" % (n - bad, n))
    return 1 if bad or not n else 0


def main(argv):
    port, path = 6802, default_compiler()
    if "--port" in argv:
        port = int(argv[argv.index("--port") + 1])
    if "--compiler" in argv:
        path = argv[argv.index("--compiler") + 1]
    comp = Compiler(path)
    if "--check" in argv:
        return check(comp)
    cfg = amictl.config()
    while True:
        try:
            serve(cfg["host"], port, cfg["token"], comp)
        except KeyboardInterrupt:
            return 0
        except (OSError, ConnectionError) as e:
            print("%s %s; again in 5 s" % (time.strftime("%H:%M:%S"), e), flush=True)
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
