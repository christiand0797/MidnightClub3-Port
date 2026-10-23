#!/usr/bin/env python3
"""Read files straight from the game's XISO disc image (read only), for when a loose copy may have been rewritten.

  py -3.12 tools/texture_pack/xiso.py <iso> <path\\in\\disc>     print the size, or '*' as the last part to list
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def default_iso():
    note = ROOT / ".local" / "source-disc.txt"
    if note.exists():
        p = Path(note.read_text().strip())
        if p.exists():
            return p
    hits = sorted(ROOT.glob("*.iso"))
    return hits[0] if hits else None


class XIso:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.base = None
        for b in (0, 0x18300000, 0xFD90000, 0x2080000):
            self.f.seek(b + 0x10000)
            if self.f.read(20) == b"MICROSOFT*XBOX*MEDIA":
                self.base = b
                break
        if self.base is None:
            raise ValueError(f"{path}: not an XISO image")
        self.f.seek(self.base + 0x10000 + 20)
        self.root = struct.unpack("<II", self.f.read(8))

    def entries(self, sec, size):
        self.f.seek(self.base + sec * 2048)
        data = self.f.read(size)
        out = []
        stack = [0]
        while stack:
            o = stack.pop()
            if o * 4 + 14 > len(data):
                continue
            l, r, s, sz, attr, nl = struct.unpack_from("<HHIIBB", data, o * 4)
            if l == 0xFFFF:
                continue
            out.append((data[o * 4 + 14:o * 4 + 14 + nl].decode("latin1"), s, sz, attr))
            if l:
                stack.append(l)
            if r:
                stack.append(r)
        return out

    def find(self, path):
        sec, size = self.root
        attr = 0x10
        for part in str(path).replace("\\", "/").strip("/").split("/"):
            m = [e for e in self.entries(sec, size) if e[0].lower() == part.lower()]
            if not m:
                return None
            _, sec, size, attr = m[0]
        return sec, size, attr

    def read(self, path):
        e = self.find(path)
        if e is None or e[2] & 0x10:
            return None
        self.f.seek(self.base + e[0] * 2048)
        return self.f.read(e[1])

    def walk(self, path=""):
        """Every file below path, as (relative path, size)."""
        e = self.find(path) if path else (*self.root, 0x10)
        stack = [(path.strip("/"), e[0], e[1])]
        while stack:
            pre, sec, size = stack.pop()
            for n, s, sz, a in self.entries(sec, size):
                p = f"{pre}/{n}" if pre else n
                if a & 0x10:
                    stack.append((p, s, sz))
                else:
                    yield p, sz


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    iso = XIso(sys.argv[1])
    want = sys.argv[2]
    if want.endswith("*"):
        for p, sz in sorted(iso.walk(want[:-1].rstrip("\\/"))):
            print(f"{sz:12d}  {p}")
    else:
        e = iso.find(want)
        print("NOT FOUND" if e is None else f"{e[1]} bytes")
