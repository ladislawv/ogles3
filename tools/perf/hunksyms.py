"""Code-hunk symbols of an AmigaOS object file, hunk library (a HUNK_UNIT an object, as
the m68k-amigaos toolchain writes .a files) or ar archive:
    {member: [[(offset, name), ...], ...]}
one list per HUNK_CODE, in order, from HUNK_SYMBOL and HUNK_EXT definitions (bebbo's
GCC writes a static function as an EXT entry of type 33). objdump and nm flatten an
object's code hunks into one, so a second .text section's symbols land at the wrong
place: this reads the hunks themselves (tools/perf/libprof.py).
  python3 hunksyms.py FILE    lists each member's code hunks and their first symbols
"""
import struct

NUL = b"\x00"


def _parse(b, units=None):
    """units: filled per HUNK_UNIT (a hunk library's objects, by the unit name's last part)."""
    p = 0
    secs = []
    cur = None

    def L():
        nonlocal p
        v = struct.unpack(">I", b[p:p + 4])[0]
        p += 4
        return v

    while p + 4 <= len(b):
        t = L() & 0x3FFFFFFF
        if t == 0x3E7:  # UNIT: a new object
            n = L()
            name = b[p:p + 4 * n].rstrip(NUL).decode("latin1")
            p += 4 * n
            if units is not None:
                secs = units.setdefault(name.rsplit("/", 1)[-1], [])
            cur = None
        elif t == 0x3E8 or t == 0x3F1:  # NAME, DEBUG
            n = L()
            p += 4 * n
        elif t in (0x3E9, 0x3EA):  # CODE, DATA
            n = L() & 0x3FFFFFFF
            p += 4 * n
            cur = [] if t == 0x3E9 else None
            if t == 0x3E9:
                secs.append(cur)
        elif t == 0x3EB:  # BSS
            L()
            cur = None
        elif t in (0x3EC, 0x3ED, 0x3EE, 0x3F7, 0x3F8, 0x3F9, 0x3FD):  # RELOC32/16/8, DREL32/16/8, RELRELOC32
            while True:
                n = L()
                if not n:
                    break
                L()
                p += 4 * n
        elif t == 0x3FC:  # RELOC32SHORT: words
            while True:
                n = struct.unpack(">H", b[p:p + 2])[0]
                p += 2
                if not n:
                    break
                p += 2 + 2 * n
            if p & 2:
                p += 2
        elif t == 0x3EF or t == 0x3F0:  # EXT, SYMBOL
            while True:
                w = L()
                if not w:
                    break
                typ, n = w >> 24, w & 0xFFFFFF
                name = b[p:p + 4 * n].rstrip(NUL).decode("latin1")
                p += 4 * n
                if t == 0x3F0 or typ < 128:  # definitions (bebbo: 33 a local one)
                    v = L()
                    if cur is not None and typ != 2:
                        cur.append((v, name.lstrip("_")))
                elif typ == 130 or typ == 137:  # COMMON: size, then references
                    L()
                    c = L()
                    p += 4 * c
                else:
                    c = L()
                    p += 4 * c
        elif t == 0x3F2:  # END
            cur = None
        else:
            break
    return secs


def symbols(path):
    data = open(path, "rb").read()
    if not data.startswith(b"!<arch>\n"):
        units = {}
        secs = _parse(data, units)
        if len(units) > 1:
            return units
        return {None: list(units.values())[0] if units else secs}
    out = {}
    p = 8
    names = b""
    while p + 60 <= len(data):
        h = data[p:p + 60]
        name = h[:16].decode().strip()
        size = int(h[48:58])
        body = data[p + 60:p + 60 + size]
        if name == "//":
            names = body
        elif name.startswith("/") and name[1:].isdigit():
            o = int(name[1:])
            name = names[o:names.index(b"/", o)].decode()
        if name not in ("/", "//", "__.SYMDEF", "__.SYMDEF SORTED"):
            out[name.rstrip("/")] = _parse(body)
        p += 60 + size + (size & 1)
    return out


if __name__ == "__main__":
    import sys
    for m, secs in symbols(sys.argv[1]).items():
        for i, s in enumerate(secs):
            print(m, i, len(s), s[:3])
