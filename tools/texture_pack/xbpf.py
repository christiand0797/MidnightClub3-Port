#!/usr/bin/env python3
"""MC3's paged resource pools (resources/**/*.xbpf + .xbpmap), and the textures the game makes of their records.

.xbpf: 'pf05', u32 page count, u32 page slot size, u32 table[count]; page n at (table[n] & 0xFFFFF) * 2048 (first page
0x10000), stored extent (table[n] >> 20) * 2048. A page holds records back to back; .xbpmap (CSV "pageno,offset,name,
type,size") names them, and every record starts with a 0x20 byte header {u32 baked address of the record's payload
(record + 0x20), 0, 0, u32 record size, ...}. Pointers inside a record are relative to that baked address.

  mip0/mip1..  a texture baked by the title's pool-texture builder (sub_000E46F0): D3D texture object at +0x20, its
               pixels at +0x80, exactly as uploaded (DXT1/DXT5 and a few P8). Loaded pages are used in place.
  gfxImage     a baked gfxImage {u16 w, h, stride, u8 type, u8, u32 flags, data*, palette*, refs, next*}: at runtime
               the "gfxImage" resource factory (sub_000E57C0 -> sub_000E5680 -> sub_000E46F0) turns it into a pool
               texture: type 5 (P8) -> D3DFMT_P8, indices swizzled (XGSwizzleRect), 1 level (sub_00210700 is 0 in
               this build), palette = the image's 256 colours with bytes 0 and 2 swapped (D3DPalette, 256 entries).
  P8 mip0      its palette lives in the model's drwShaderModel record: a struct {.., colours*, name} followed by the
               D3DPalette object {0x00030001, Data}; paired with the texture by name.

  py -3.12 tools/texture_pack/xbpf.py <file.xbpf>     list its texture records
"""
import csv
import struct
import sys
from pathlib import Path

import numpy as np

import texfile
import xbtex


class Pool:
    def __init__(self, path):
        self.path = Path(path)
        self.b = self.path.read_bytes()
        magic, n, self.slot = struct.unpack_from("<4sII", self.b, 0)
        if magic != b"pf05":
            raise ValueError(f"{path}: not a pf05 pool")
        self.table = struct.unpack_from(f"<{n}I", self.b, 12)

    def page(self, n):
        t = self.table[n]
        return (t & 0xFFFFF) * 2048, (t >> 20) * 2048

    def records(self):
        """(name, type, file offset, size) of every record: from the .xbpmap, else by walking each page."""
        m = self.path.with_suffix(".xbpmap")
        if m.exists():
            for row in csv.reader(l for l in m.read_text().splitlines() if l.strip() and not l.startswith("#")):
                page, off, name, typ, size = int(row[0]), int(row[1]), row[2], row[3], int(row[4])
                yield name, typ, self.page(page)[0] + off, size
            return
        for n in range(len(self.table)):
            start, extent = self.page(n)
            o = 0
            while o + 0x80 <= extent:
                size = struct.unpack_from("<I", self.b, start + o + 0xC)[0]
                if size < 0x80 or o + size > extent:
                    break
                c = struct.unpack_from("<I", self.b, start + o + 0x20)[0]
                yield f"page{n}_{o:x}", ("mip0" if c & 0xFFFF0000 == 0x00040000 else "?"), start + o, size
                o += size

    def to_file(self, rec, va):
        """File offset of a baked address inside record rec."""
        base = struct.unpack_from("<I", self.b, rec)[0]
        return rec + 0x20 + (va - base)


def palettes_by_name(pool, recs):
    """name -> set of 1024 byte palettes, from the D3DPalette objects in the pool's other records."""
    out = {}
    b = pool.b
    for name, typ, r, size in recs:
        if typ.startswith("mip") or typ == "gfxImage":
            continue
        base = struct.unpack_from("<I", b, r)[0]
        if not base:
            continue
        rec = b[r:r + size]
        for o in range(0x20, size - 12, 4):
            c, d, lock = struct.unpack_from("<3I", rec, o)
            if c & 0x3FFFFFFF != 0x00030001 or lock or not d or d >> 28:
                continue
            va = d | 0x80000000
            col = r + 0x20 + (va - base)
            if not (r <= col and col + 1024 <= r + size):
                continue
            # the owner struct: {.., colours pointer (== va), name} just before the object
            back = rec[max(0, o - 0x40):o]
            k = back.rfind(struct.pack("<I", va))
            if k < 0:
                continue
            s = back[k + 4:]
            nm = s[:s.index(b"\0")].decode("latin1") if b"\0" in s else s.decode("latin1")
            n = (256 >> (c >> 30)) * 4
            out.setdefault(nm, set()).add(bytes(b[col:col + n]) + bytes(1024 - n))
    return out


def gfx_image(pool, r, size):
    """(w, h, stride, type, pixels offset, palette offset or None) of a gfxImage record, None if not found."""
    b = pool.b
    base = struct.unpack_from("<I", b, r)[0]
    if not base:
        return None
    cands = [r + 0x20]
    p = struct.unpack_from("<I", b, r + 0x24)[0]
    if p >> 28 == 8:
        cands.insert(0, pool.to_file(r, p))
    for io in cands:
        if not (r <= io and io + 0x20 <= r + size):
            continue
        w, h, stride, typ = struct.unpack_from("<HHHB", b, io)
        data, pal = struct.unpack_from("<II", b, io + 0xC)
        if not w or not h or w & (w - 1) or h & (h - 1) or stride < w:
            continue
        do = pool.to_file(r, data)
        po = pool.to_file(r, pal) if pal else None
        if r <= do and do + stride * h <= r + size:
            return w, h, stride, typ, do, po
    return None


def textures(pool):
    """(name, Tex, guest bytes, palette 1024 bytes or None, rgba or None) for every texture record of the pool."""
    recs = list(pool.records())
    pals = None
    b = pool.b
    for name, typ, r, size in recs:
        if typ.startswith("mip"):
            common, data, lock, fmt, sz = struct.unpack_from("<5I", b, r + 0x20)
            f = (fmt >> 8) & 0xFF
            if common & 0xFFFF0000 != 0x00040000 or f not in xbtex.FORMATS or sz:
                continue
            levels = max((fmt >> 16) & 0xF, 1)
            w, h, cube = 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF), bool(fmt & 4)
            n = xbtex.total_size(f, w, h, levels, cube)
            if 0x80 + n > size:
                continue
            t = xbtex.Tex(f, w, h, levels, cube, r + 0x80, n)
            if f == 0x0B:
                if pals is None:
                    pals = palettes_by_name(pool, recs)
                for pal in sorted(pals.get(name, ())):
                    yield name, t, b[r + 0x80:r + 0x80 + n], pal, None
                continue
            yield name, t, b[r + 0x80:r + 0x80 + n], None, None
        elif typ == "gfxImage":
            g = gfx_image(pool, r, size)
            if not g:
                continue
            w, h, stride, itype, do, po = g
            if itype != 5 or po is None:  # only P8 images occur in the shipped pools
                continue
            lin = np.frombuffer(b, np.uint8, stride * h, do).reshape(h, stride)[:, :w]
            sw = np.empty(w * h, np.uint8)
            sw[texfile.morton(w, h)] = lin
            img_pal = np.frombuffer(b, np.uint8, 1024, po).reshape(256, 4)
            d3d_pal = img_pal[:, [2, 1, 0, 3]].tobytes()  # sub_000E46F0 swaps bytes 0 and 2
            t = xbtex.Tex(0x0B, w, h, 1, False, do, w * h)
            yield name, t, sw.tobytes(), d3d_pal, img_pal[lin]  # image palette bytes are R G B A


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for name, t, data, pal, _ in textures(Pool(sys.argv[1])):
        h = xbtex.content_hash(t, data, xbtex.hash_bytes(pal) if pal else 0)
        print(f"{h:016X}  {xbtex.FORMATS[t.fmt][0]:6s} {t.width}x{t.height} L{t.levels}  {name}")
