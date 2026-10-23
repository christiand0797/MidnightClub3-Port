#!/usr/bin/env python3
"""Every texture on the disc, saved as textures/source/<hash>.png under the name the runtime's texture pack uses.

Sources (format details and verification in docs/notes/texture_extraction.md):
  .xbck memory images (Dave archives assets_x.dat* and loose): swizzled, DXT, linear and P8 (paired with the
      bundle's D3DPalette) texture objects (xbtex.py)
  .tex images in the archives: P8/P4/DXT as the title's loader turns them into D3D textures (texfile.py); both the
      full mip chain and level 0 alone, as the title loads them either way
  resources/**/*.xbpf paged pools: baked mip records and gfxImage (vinyl) records (xbpf.py)
  media/*.xpr XPR0 bundles, read from the disc image when present (xiso.py)
Each texture's top mip is decoded and written once per hash; existing PNGs are kept. upscale.py then turns
textures/source and textures/dump into textures/replace.

  py -3.12 tools/texture_pack/extract_all.py [--assets DIR] [--textures DIR] [--iso DISC.iso]
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import dave  # noqa: E402
import texfile  # noqa: E402
import xbpf  # noqa: E402
import xbtex  # noqa: E402
import xiso  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def decode_dxt(data, w, h, fmt):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bsize = 8 if fmt == 0x0C else 16
    blk = np.frombuffer(data[:bw * bh * bsize], np.uint8).reshape(bw * bh, bsize)
    col = blk[:, -8:]
    c0 = col[:, 0].astype(np.int32) | col[:, 1].astype(np.int32) << 8
    c1 = col[:, 2].astype(np.int32) | col[:, 3].astype(np.int32) << 8
    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1) | (fmt != 0x0C)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # n,4,3
    idx = (col[:, 4:8].astype(np.uint32) @ np.array([1, 256, 65536, 16777216], np.uint32).reshape(4, 1))[:, 0]
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3  # n,16
    rgb = np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(3, 2), 1)  # n,16,3
    if fmt == 0x0C:
        a = np.where((~four[:, None]) & (sel == 3), 0, 255)
    elif fmt == 0x0E:
        ab = blk[:, :8]
        nib = np.stack([ab & 15, ab >> 4], -1).reshape(-1, 16)
        a = nib.astype(np.int32) * 17
    else:
        a0, a1 = blk[:, 0].astype(np.int32), blk[:, 1].astype(np.int32)
        bits = np.zeros(len(blk), np.uint64)
        for i in range(6):
            bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        ai = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int32)
        eight = a0 > a1
        tab = np.zeros((len(blk), 8), np.int32)
        tab[:, 0], tab[:, 1] = a0, a1
        for i in range(1, 7):
            tab[:, i + 1] = np.where(eight, ((7 - i) * a0 + i * a1) // 7, 0)
        for i in range(1, 5):
            tab[:, i + 1] = np.where(eight, tab[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
        tab[:, 6] = np.where(eight, tab[:, 6], 0)
        tab[:, 7] = np.where(eight, tab[:, 7], 255)
        a = np.take_along_axis(tab, ai, 1)
    px = np.concatenate([rgb, a[:, :, None]], 2).astype(np.uint8).reshape(bh, bw, 4, 4, 4)
    img = px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def swizzle_index(w, h):
    """Morton offsets of each texel, as the console stores swizzled textures (runtime SwizzleMasks/Deposit)."""
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

    xs = deposit(np.arange(w, dtype=np.int64), mx)
    ys = deposit(np.arange(h, dtype=np.int64), my)
    return ys[:, None] | xs[None, :]


def _texels(t, data, bpp):
    """Texel values of the top level in row order: swizzled (Morton) or linear rows of t.pitch bytes."""
    w, h = t.width, t.height
    if t.linear:
        rows = np.frombuffer(data[:t.pitch * h], np.uint8).reshape(h, t.pitch)[:, :w * bpp]
        raw = np.ascontiguousarray(rows).reshape(-1)
        return raw.view({1: np.uint8, 2: np.uint16, 4: np.uint32}[bpp]).reshape(h, w)
    raw = np.frombuffer(data[:w * h * bpp], np.uint8)
    return raw.view({1: np.uint8, 2: np.uint16, 4: np.uint32}[bpp])[swizzle_index(w, h)]


def decode(t, data, palette=None):
    """RGBA uint8 HxWx4 of the top mip, None for formats not handled. palette: a P8 texture's D3D colours (B G R A)."""
    if t.fmt in (0x0C, 0x0E, 0x0F):
        return decode_dxt(data, t.width, t.height, t.fmt)
    fmt = xbtex.LINEAR_OF.get(t.fmt, t.fmt)
    bpp = xbtex.FORMATS[t.fmt][1] // 8
    v = _texels(t, data, bpp).astype(np.int64)
    if fmt == xbtex.P8:
        if palette is None:
            return None
        pal = np.frombuffer(bytes(palette) + bytes(1024 - len(palette)), np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]]
        return pal[v]
    if bpp == 4:
        a = v >> 24 & 255 if fmt == 0x06 else 255 + 0 * v
        r, g, b = v >> 16 & 255, v >> 8 & 255, v & 255
    elif bpp == 2:
        if fmt == 0x05:
            r, g, b, a = (v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255 + 0 * v
        elif fmt == 0x04:
            a, r, g, b = (v >> 12 & 15) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17
        elif fmt in (0x02, 0x03):
            a = np.where(v >> 15, 255, 0) if fmt == 0x02 else 255 + 0 * v
            r, g, b = (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31
        elif fmt == 0x1A:
            a, r = v >> 8, v & 255
            g = b = r
        else:
            return None
    elif fmt == 0x00:
        r = g = b = v
        a = 255 + 0 * v
    elif fmt == 0x19:
        r = g = b = 255 + 0 * v
        a = v
    else:
        return None
    return np.stack([r, g, b, a], -1).astype(np.uint8)


def xpr_textures(b):
    """Textures of an XPR0 resource bundle {'XPR0', total size, header size, resource headers.., 0xFFFFFFFF}: Data is
    an offset into the data block at header size, loaded as is (XDK XPR layout)."""
    if b[:4] != b"XPR0":
        return []
    total, hs = struct.unpack_from("<II", b, 4)
    out, o = [], 12
    while o + 20 <= hs:
        common, data, lock, fmt, size = struct.unpack_from("<5I", b, o)
        if common == 0xFFFFFFFF:
            break
        kind = common & 0x00070000
        if kind == 0x00040000:
            o += 20
            f = (fmt >> 8) & 0xFF
            if f not in xbtex.FORMATS or ((fmt >> 4) & 0xF) != 2 or size:
                continue
            levels = max((fmt >> 16) & 0xF, 1)
            w, h, cube = 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF), bool(fmt & 4)
            n = xbtex.total_size(f, w, h, levels, cube)
            off = hs + data
            if f == xbtex.P8 or off + n > len(b):
                continue  # no P8 occurs in the shipped XPRs
            t = xbtex.Tex(f, w, h, levels, cube, off, n)
            t.name = xbtex.content_hash(t, b[off:off + n])
            out.append(t)
        elif common == 0:
            break
        else:
            o += 12 if kind == 0x00030000 else 20
    return out


def jobs(assets, iso):
    """(where, Tex, guest bytes, palette or None, rgba or None) for every texture variant of every source."""
    # 1. .xbck memory images in the archives and loose
    def xbck(where, b):
        for t in xbtex.xbck_textures(b):
            data = b[t.offset:t.offset + t.size]
            if t.fmt == xbtex.P8:
                for pal in t.palettes:
                    yield where, t, data, pal, None
            else:
                yield where, t, data, None, None

    archives = [dave.Archive(p) for p in sorted(assets.glob("assets_x.dat*"))]
    for a in archives:
        for e in a.files("*.xbck"):
            yield from xbck(f"{a.path.name}:{e.name}", a.read(e))
    for p in sorted(assets.rglob("*.xbck")):
        yield from xbck(str(p.relative_to(assets)), p.read_bytes())
    # 2. .tex images (texfile.py: the D3D texture the title's loader makes of them)
    for a in archives:
        for e in a.files("*.tex"):
            try:
                ti = texfile.TexImage(a.read(e))
            except ValueError:
                continue
            for n, t, data in ti.variants():
                yield f"{a.path.name}:{e.name}#L{n}", t, data, ti.palette, ti.rgba() if ti.fmt == xbtex.P8 else None
    # 3. paged pools
    for p in sorted(assets.rglob("*.xbpf")):
        pool = xbpf.Pool(p)
        for name, t, data, pal, rgba in xbpf.textures(pool):
            yield f"{p.relative_to(assets)}:{name}", t, data, pal, rgba
    # 4. XPR bundles, from the disc when it is there (loose copies may have been rewritten by old repack scripts)
    for p in sorted(assets.rglob("*.xpr")):
        rel = p.relative_to(assets).as_posix()
        b = iso.read(rel) if iso else None
        where = f"disc:{rel}" if b else rel
        b = b or p.read_bytes()
        for t in xpr_textures(b):
            yield where, t, b[t.offset:t.offset + t.size], None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--assets", type=Path, default=ROOT / "templates" / "game" / "assets")
    ap.add_argument("--textures", type=Path, default=ROOT / "templates" / "game" / "out" / "native" / "textures")
    ap.add_argument("--iso", type=Path, default=xiso.default_iso(), help="disc image for pristine XPRs")
    args = ap.parse_args()
    out = args.textures / "source"
    out.mkdir(parents=True, exist_ok=True)
    iso = xiso.XIso(args.iso) if args.iso and Path(args.iso).exists() else None
    index_path = args.textures / "source_index.json"
    index = {}  # hash -> where it was found, for finding a texture's file later
    found = written = skipped = 0
    kinds = {}
    for where, t, data, pal, rgba in jobs(args.assets, iso):
        found += 1
        ph = xbtex.palette_hash(pal) if pal is not None else 0
        h = t.name if (t.name and pal is None) else xbtex.content_hash(t, data, ph)
        name = f"{h:016X}"
        if name in index:
            continue
        kind = xbtex.FORMATS[t.fmt][0]
        index[name] = f"{where}@{t.offset:#x} {kind} {t.width}x{t.height} L{t.levels}"
        dst = out / f"{name}.png"
        if dst.exists():
            continue
        img = rgba if rgba is not None else decode(t, data, pal)
        if img is None:
            skipped += 1
            continue
        tmp = dst.with_name(dst.name + ".tmp")
        Image.fromarray(np.ascontiguousarray(img, np.uint8), "RGBA").save(tmp, format="PNG")
        tmp.replace(dst)
        written += 1
        kinds[kind] = kinds.get(kind, 0) + 1
        if written % 500 == 0:
            print(f"  {found} textures, {len(index)} unique, {written} written", flush=True)
    if index_path.exists():  # keep earlier entries (other runs, other sources)
        try:
            old = json.loads(index_path.read_text())
            old.update(index)
            index = old
        except ValueError:
            pass
    tmp = index_path.with_suffix(".tmp")
    tmp.write_text(json.dumps(index, indent=0))
    tmp.replace(index_path)
    print(f"done: {found} texture variants, {len(index)} unique in index, {written} new PNGs {kinds}, "
          f"{skipped} undecodable")


if __name__ == "__main__":
    main()
