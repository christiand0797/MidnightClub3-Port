#!/usr/bin/env python3
"""MC3's .tex image files, turned into the D3D texture the game makes of them (format, bytes, palette).

Traced in default.xbe (details and addresses in docs/notes/texture_extraction.md):
  .tex loader sub_002D7710: 14 byte header {u16 width, u16 height, u16 type, u16 levels, u16 flags_lo, u16 flags_hi,
  u16 ?}, then for palettized types the palette (4 bytes a colour, B G R A), then each mip level's pixels.
  type 0x01 / 0x0E  P8, 256 colours   -> D3DFMT_P8, indices swizzled, palette = the file's 256 colours
  type 0x0F / 0x10  P4, 16 colours    -> D3DFMT_P8, each byte two indices (low nibble first), palette = the file's
                                          16 colours then 240 opaque black (0xFF000000)
  type 0x16 DXT1, 0x18 DXT3, 0x1A DXT5 -> the same D3D format, bytes copied unchanged
  The image chain (sub_002D8FA0) only goes on while both sides are > 4, so levels stop at the first level with a
  side of 4. A texture loaded without mips (half the title's load calls) keeps level 0 only, so both variants are
  produced for multi-level files.
"""
import functools
import struct

import numpy as np

import xbtex

P8, DXT1, DXT3, DXT5 = 0x0B, 0x0C, 0x0E, 0x0F
TYPES = {0x01: P8, 0x0E: P8, 0x0F: "P4", 0x10: "P4", 0x16: DXT1, 0x18: DXT3, 0x1A: DXT5}
BPP = {P8: 8, "P4": 8, DXT1: 4, DXT3: 8, DXT5: 8}  # the image's bits per pixel (table 0x4B5850)
OPAQUE_BLACK = struct.pack("<I", 0xFF000000)


@functools.lru_cache(maxsize=None)
def morton(w, h):
    """Swizzled offset of each texel (h x w), as XGSwizzleRect lays them out and the runtime's Deposit reads them."""
    mx = my = 0
    bit, i = 1, 0
    while (1 << i) < w or (1 << i) < h:
        if (1 << i) < w:
            mx |= bit
            bit <<= 1
        if (1 << i) < h:
            my |= bit
            bit <<= 1
        i += 1

    def deposit(v, m):
        out = np.zeros_like(v)
        b = 0
        for k in range(32):
            if m >> k & 1:
                out |= ((v >> b) & 1) << k
                b += 1
        return out

    return deposit(np.arange(h, dtype=np.int64), my)[:, None] | deposit(np.arange(w, dtype=np.int64), mx)[None, :]


class TexImage:
    """One .tex file as the game's D3D texture: fmt, width, height, levels, data (guest bytes), palette (1024 bytes
    as the D3DPalette holds them, or None), levels_in_file."""

    def __init__(self, b):
        if len(b) < 14:
            raise ValueError("short .tex")
        w, h, typ, levels = struct.unpack_from("<4H", b, 0)
        flags = struct.unpack_from("<H", b, 0xC)[0] << 16 | struct.unpack_from("<H", b, 0xA)[0]
        if w == 0 and h == 0:
            raise ValueError("'tex2' chunked .tex not handled")
        if typ not in TYPES:
            raise ValueError(f"type {typ:#x} not handled")
        if flags & 0x400000:
            raise ValueError("linear .tex not handled")
        kind = TYPES[typ]
        self.width, self.height, self.type, self.flags = w, h, typ, flags
        o = 14
        self.palette = None
        if kind == P8:
            self.palette = bytes(b[o:o + 1024])
            o += 1024
        elif kind == "P4":
            self.palette = bytes(b[o:o + 64]) + OPAQUE_BLACK * 240
            o += 64
        # the image chain: level n exists while both sides of level n-1 were > 4
        dims = [(w, h)]
        while len(dims) < max(levels, 1) and dims[-1][0] > 4 and dims[-1][1] > 4:
            dims.append((dims[-1][0] >> 1, dims[-1][1] >> 1))
        self.fmt = P8 if kind == "P4" else kind
        self.mips = []
        for lw, lh in dims:
            stride = BPP[kind] * lw >> 3
            n = stride * lh if kind != "P4" else lw * lh // 2
            raw = b[o:o + n]
            if len(raw) < n:
                break
            o += n
            if kind == "P4":
                v = np.frombuffer(raw, np.uint8)
                raw = np.stack([v & 15, v >> 4], -1).reshape(-1).tobytes()
            if self.fmt == P8:
                lin = np.frombuffer(raw, np.uint8).reshape(lh, lw)
                sw = np.empty(lw * lh, np.uint8)
                sw[morton(lw, lh)] = lin
                self.mips.append((lw, lh, sw.tobytes(), lin))
            else:
                self.mips.append((lw, lh, bytes(raw), None))
        if not self.mips:
            raise ValueError("truncated .tex")

    def variants(self):
        """(levels, Tex, guest bytes) for the mip counts the game may create: all loaded levels and level 0 only."""
        counts = sorted({len(self.mips), 1}, reverse=True)
        for n in counts:
            data = b"".join(m[2] for m in self.mips[:n])
            t = xbtex.Tex(self.fmt, self.width, self.height, n, False, 0, len(data))
            yield n, t, data

    def palette_hash(self):
        return xbtex.hash_bytes(self.palette) if self.palette else 0

    def rgba(self):
        """Top level as RGBA (palettized only; DXT is decoded by extract_all.decode)."""
        lin = self.mips[0][3]
        pal = np.frombuffer(self.palette, np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]]
        return pal[lin]
