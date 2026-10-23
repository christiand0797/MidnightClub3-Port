#!/usr/bin/env python3
"""Xbox textures inside MC3's files, named as the runtime's texture pack names them.

The runtime (runtime/gpu/texture_pack.cpp ContentHash) keys a texture by Hash(format, width, height, depth, levels,
cube) then Hash(every byte of its mip chain) then, for P8, Hash(its palette's hash). Hash is runtime/gpu/shaders.cpp's;
hash() below is the same function. A texture loaded unchanged from a file therefore has the same name offline.

.xbck bundles are memory images: a 0x80 byte header {base virtual address, version, count, size} and the image from
0x80. D3D texture objects in it are {Common 0x0004xxxx, Data (physical address), Lock 0, Format, Size}; their pixels
sit at Data - (base & 0x0FFFFFFF) + 0x80. D3DPalette objects are {Common 0x0003xxxx (size code in bits 30-31), Data,
Lock}. Details and verification: docs/notes/texture_extraction.md.
"""
import struct

import numpy as np

K0, K1 = 0x9E3779B97F4A7C15, 0xE7037ED1A0B428DB
M64 = (1 << 64) - 1
FNV = 1469598103934665603


def _mix(a, b):
    r = a * b
    return (r & M64) ^ (r >> 64)


def hash_bytes(b, h=FNV):
    n = len(b)
    seed = h ^ K0
    full = n - n % 16
    if full:
        words = struct.unpack_from(f"<{full // 8}Q", b, 0)
        mix = _mix
        for j in range(0, len(words), 2):
            seed = mix(words[j] ^ K1, words[j + 1] ^ seed)
    r = n - full
    if r:
        if r >= 8:
            x = struct.unpack_from("<Q", b, full)[0]
            y = struct.unpack_from("<Q", b, n - 8)[0]
        else:
            x = int.from_bytes(b[full:n], "little")
            y = 0
        seed = _mix(x ^ K1, y ^ seed ^ r)
    return _mix(seed ^ K0, n ^ K1)


# Xbox D3DFORMAT: name, bits per texel (bytes per 4x4 block for DXT), swizzled, compressed
FORMATS = {
    0x00: ("L8", 8, True, False), 0x01: ("AL8", 8, True, False), 0x02: ("A1R5G5B5", 16, True, False),
    0x03: ("X1R5G5B5", 16, True, False), 0x04: ("A4R4G4B4", 16, True, False), 0x05: ("R5G6B5", 16, True, False),
    0x06: ("A8R8G8B8", 32, True, False), 0x07: ("X8R8G8B8", 32, True, False), 0x0B: ("P8", 8, True, False),
    0x0C: ("DXT1", 8, False, True), 0x0E: ("DXT3", 16, False, True), 0x0F: ("DXT5", 16, False, True),
    0x10: ("LIN_A1R5G5B5", 16, False, False), 0x11: ("LIN_R5G6B5", 16, False, False),
    0x12: ("LIN_A8R8G8B8", 32, False, False), 0x13: ("LIN_L8", 8, False, False),
    0x19: ("A8", 8, True, False), 0x1A: ("A8L8", 16, True, False), 0x1B: ("LIN_AL8", 8, False, False),
    0x1C: ("LIN_X1R5G5B5", 16, False, False), 0x1D: ("LIN_A4R4G4B4", 16, False, False),
    0x1E: ("LIN_X8R8G8B8", 32, False, False), 0x1F: ("LIN_A8", 8, False, False), 0x20: ("LIN_A8L8", 16, False, False),
}
P8 = 0x0B
# linear format -> the swizzled format with the same texel layout (for decoding)
LINEAR_OF = {0x10: 0x02, 0x11: 0x05, 0x12: 0x06, 0x13: 0x00, 0x1B: 0x01, 0x1C: 0x03, 0x1D: 0x04, 0x1E: 0x07,
             0x1F: 0x19, 0x20: 0x1A}


class Tex:
    __slots__ = ("fmt", "width", "height", "levels", "cube", "offset", "size", "name", "pitch", "header", "palettes")

    def __init__(self, fmt, width, height, levels, cube, offset, size, pitch=0):
        self.fmt, self.width, self.height, self.levels, self.cube = fmt, width, height, levels, cube
        self.offset, self.size, self.name, self.pitch = offset, size, 0, pitch
        self.header, self.palettes = 0, []

    @property
    def linear(self):
        return self.fmt in LINEAR_OF


def level_size(fmt, w, h, level):
    _, bits, _, comp = FORMATS[fmt]
    w, h = max(w >> level, 1), max(h >> level, 1)
    if comp:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bits
    return w * h * bits // 8


def total_size(fmt, w, h, levels, cube):
    s = sum(level_size(fmt, w, h, l) for l in range(levels))
    return ((s + 127) & ~127) * 6 if cube else s


def palette_hash(colours):
    """The runtime's StagePalette hash (d3d12.cpp ReadStagePalette): Hash of 256 colours, those past the palette's
    size (code in Common bits 30-31) left zero. colours: the D3D palette's bytes (B G R A each)."""
    return hash_bytes(bytes(colours) + bytes(1024 - len(colours)))


def content_hash(t, data, palette_hash=0):
    shape = struct.pack("<6I", t.fmt, t.width, t.height, 1, t.levels, 1 if t.cube else 0)
    h = hash_bytes(shape)
    h = hash_bytes(data, h)
    return hash_bytes(struct.pack("<Q", palette_hash), h) if palette_hash else h


def _palette_at(b, o, pb):
    """The colours (as many as the size code says) of a D3DPalette object at image offset o, None if it is not one."""
    if o < 0x80 or o + 12 > len(b):
        return None
    c, d, lock = struct.unpack_from("<3I", b, o)
    if (c & 0x00070000) != 0x00030000 or (c & 0x3FF80000) or lock:
        return None
    off = d - pb + 0x80
    n = (256 >> (c >> 30)) * 4
    if off < 0x80 or off + n > len(b):
        return None
    return bytes(b[off:off + n])


def xbck_textures(b):
    """Every 2D texture object in an .xbck image (swizzled, compressed and linear), with the runtime's content hash.

    A P8 texture's palette is found the way the bundles link them: the record pointing at the texture object holds,
    0x28 bytes on, a pointer to a {u32, D3DPalette} pair (all 6697 P8 textures of the flash bundles pair this way).
    Failing that, the bundle's two palette objects nearest the texture are tried. t.palettes lists each candidate's
    colours; t.name is the hash with the first one (0 when there is none)."""
    if len(b) < 0x80:
        return []
    vbase = struct.unpack_from("<I", b, 0)[0]
    pb = vbase & 0x0FFFFFFF
    out, seen, p8 = [], set(), []
    for o in range(0x80, len(b) - 20, 4):
        common, data, lock, fmt, size = struct.unpack_from("<5I", b, o)
        if (common & 0xFFFF0000) != 0x00040000 or lock or (fmt & 3) != 1 or ((fmt >> 4) & 0xF) != 2:
            continue
        f = (fmt >> 8) & 0xFF
        if f not in FORMATS:
            continue
        cube = bool(fmt & 4)
        if size:  # linear with an explicit size (texture.cpp Describe): one level of pitch * height bytes
            if f not in LINEAR_OF or cube:
                continue
            w, h, pitch = (size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1, ((size >> 24) + 1) * 64
            levels, n = 1, pitch * h
        else:
            levels = max((fmt >> 16) & 0xF, 1)
            lw, lh = (fmt >> 20) & 0xF, (fmt >> 24) & 0xF
            if lw > 11 or lh > 11 or (fmt >> 28):
                continue
            w, h = 1 << lw, 1 << lh
            if f in LINEAR_OF:  # texture.cpp LevelSize: pitch * height for every level of a linear texture
                if cube:
                    continue
                pitch = w * FORMATS[f][1] // 8
                n = pitch * h * levels
            else:
                pitch, n = 0, total_size(f, w, h, levels, cube)
        off = data - pb + 0x80
        if off < 0x80 or off + n > len(b) or off in seen:
            continue
        seen.add(off)
        t = Tex(f, w, h, levels, cube, off, n, pitch)
        t.header = o
        if f == P8:
            p8.append(t)
        else:
            t.name = content_hash(t, b[off:off + n])
        out.append(t)
    if p8:
        _pair_palettes(b, vbase, pb, p8)
    return out


def _pair_palettes(b, vbase, pb, p8):
    words = np.frombuffer(b, "<u4", (len(b) - 0x80) // 4, 0x80)
    pals = None
    for t in p8:
        va = vbase + t.header - 0x80
        found = []
        for i in np.nonzero(words == va)[0]:
            r = 0x80 + 4 * int(i)
            if r + 0x2C > len(b):
                continue
            wrapper = struct.unpack_from("<I", b, r + 0x28)[0]
            for o in (wrapper - vbase + 0x80 + 4, wrapper - vbase + 0x80):
                col = _palette_at(b, o, pb)
                if col is not None:
                    found.append(col)
                    break
        if not found:
            if pals is None:  # every palette object in the bundle, by offset
                pals = []
                for i in np.nonzero((words & 0x00070000) == 0x00030000)[0]:
                    o = 0x80 + 4 * int(i)
                    col = _palette_at(b, o, pb)
                    if col is not None:
                        pals.append((o, col))
            found = [c for _, c in sorted(pals, key=lambda p: abs(p[0] - t.header))[:2]]
        uniq = []
        for c in found:
            if c not in uniq:
                uniq.append(c)
        t.palettes = uniq
        if uniq:
            t.name = content_hash(t, b[t.offset:t.offset + t.size], palette_hash(uniq[0]))
