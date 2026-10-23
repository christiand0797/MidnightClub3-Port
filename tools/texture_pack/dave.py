#!/usr/bin/env python3
"""Reads Midnight Club 3's "Dave" archives (assets_x.dat, assets_x.dat_1.._5, banks.dat).

Layout, little-endian:
  0x00  magic "Dave" (names packed) or "DAVE" (names plain), file count, directory size, name table size
  0x800 directory: per file {name offset, data offset, size, packed size}; packed size != size: raw deflate
  0x800 + directory size: the name table

Packed names are 6-bit codes, low bits first, over " #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~" (0 ends a name).
A code c >= 56 starts a name: the next code x gives a prefix length (c - 56) + (x - 32) * 8 copied from the name of
the entry before (names are decoded in entry order).

  py -3.12 tools/texture_pack/dave.py list assets_x.dat [pattern]
  py -3.12 tools/texture_pack/dave.py extract assets_x.dat outdir [pattern]
"""
import fnmatch
import struct
import sys
import zlib
from pathlib import Path

CHARS = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~@"  # 48 is unknown (3 cutscene names), shown as @


class Entry:
    __slots__ = ("name", "offset", "size", "packed")

    def __init__(self, name, offset, size, packed):
        self.name, self.offset, self.size, self.packed = name, offset, size, packed


def _codes(table, off):
    v = int.from_bytes(table[off:off + 192], "little")
    i = 0
    while True:
        yield (v >> (6 * i)) & 63
        i += 1


def _name(table, off, prev):
    g = _codes(table, off)
    c = next(g)
    out = ""
    if c >= 56:
        n = (c - 56) + (next(g) - 32) * 8
        out = prev[:n]
        c = next(g)
    while c:
        out += CHARS[c]
        c = next(g)
    return out


class Archive:
    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            magic, n, dsz, nsz = struct.unpack("<4sIII", f.read(16))
            if magic not in (b"Dave", b"DAVE"):
                raise ValueError(f"{path}: not a Dave archive")
            f.seek(0x800)
            d = f.read(n * 16)
            f.seek(0x800 + dsz)
            table = f.read(nsz)
        self.entries = []
        prev = ""
        for i in range(n):
            noff, off, size, packed = struct.unpack_from("<IIII", d, i * 16)
            if magic == b"Dave":
                name = _name(table, noff, prev)
            else:
                name = table[noff:table.index(b"\0", noff)].decode("latin-1")
            prev = name
            self.entries.append(Entry(name, off, size, packed))

    def read(self, e):
        with open(self.path, "rb") as f:
            f.seek(e.offset)
            raw = f.read(e.packed)
        return zlib.decompress(raw, -15) if e.packed != e.size else raw

    def files(self, pattern="*"):
        return [e for e in self.entries if e.size and not e.name.endswith("/") and fnmatch.fnmatch(e.name, pattern)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    a = Archive(path)
    if cmd == "list":
        pat = sys.argv[3] if len(sys.argv) > 3 else "*"
        for e in a.files(pat):
            print(f"{e.size:10d} {e.packed:10d}  {e.name}")
    elif cmd == "extract":
        out = Path(sys.argv[3])
        pat = sys.argv[4] if len(sys.argv) > 4 else "*"
        for e in a.files(pat):
            p = out / e.name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(a.read(e))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
