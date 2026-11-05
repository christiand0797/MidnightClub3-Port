#!/usr/bin/env python3
"""crisp_fonts.py: which sheets it rebuilds, and that a rebuilt glyph keeps its size with a smooth edge.

  py -3.12 -I tools/texture_pack/test_crisp_fonts.py
"""
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import crisp_fonts as cf  # noqa: E402


def sheet(alpha, ink=(255, 255, 255)):
    rgba = np.zeros(alpha.shape + (4,), np.uint8)
    rgba[..., :3] = ink
    rgba[..., 3] = alpha
    return rgba


def glyphs(on=255, edge=None):
    """A 128x128 atlas of 36 'L' shaped glyphs, optionally with an anti-aliased rim."""
    a = np.zeros((128, 128), np.uint8)
    for gy in range(6):
        for gx in range(6):
            y, x = 4 + gy * 20, 4 + gx * 20
            a[y:y + 14, x:x + 3] = on
            a[y + 11:y + 14, x:x + 10] = on
            if edge is not None:
                a[y:y + 14, x + 3] = edge
    return a


def main():
    ok = []

    def check(name, cond, detail=""):
        ok.append(bool(cond))
        print(f"  [{'PASS' if cond else 'FAIL'}] {name} {detail}")

    hard = glyphs()
    check("pure on/off glyphs count as hard-edged", cf.hard_edged(hard))
    check("glyphs with a thin anti-aliased rim count as hard-edged", cf.hard_edged(glyphs(edge=128)))
    soft = np.clip(np.asarray(Image.fromarray(hard).resize((32, 32), Image.BILINEAR).resize((128, 128), Image.BILINEAR)),
                   0, 200).astype(np.uint8)
    check("a soft shadow sheet is left alone", not cf.hard_edged(soft))

    check("a one-colour atlas of many glyphs is a glyph sheet", cf.glyph_sheet(sheet(hard)))
    noisy = sheet(hard)
    noisy[..., 0] = np.random.default_rng(1).integers(0, 255, (128, 128))
    check("a multi-coloured image is not a glyph sheet", not cf.glyph_sheet(noisy))
    block = np.zeros((128, 128), np.uint8)
    block[10:60, 10:60] = 255
    check("one big shape is not a glyph sheet", not cf.glyph_sheet(sheet(block)))

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "src.png"
        Image.fromarray(sheet(hard)).save(src)
        out = cf.rebuild(src, Path(tmp) / "missing.png")
        check("a hard sheet is rebuilt at 8x", out is not None and out.size == (1024, 1024), str(out and out.size))
        a = np.asarray(out)[..., 3].astype(np.float64) / 255
        ratio = a.sum() / (hard.astype(np.float64).sum() / 255 * 64)
        check("glyph coverage is kept within 25%", 0.75 <= ratio <= 1.25, f"{ratio:.3f}")
        partial = ((a > 0.05) & (a < 0.95)).sum() / max((a > 0.05).sum(), 1)
        check("edges are anti-aliased, not stair-stepped", 0.02 < partial < 0.3, f"{partial:.3f}")
        rgb = np.asarray(out)[..., :3][a > 0.5]
        check("ink colour carried to the new edge (no dark rim)", rgb.min() > 200, str(rgb.min()))
        Image.fromarray(sheet(soft)).save(src)
        check("a soft sheet is not rebuilt", cf.rebuild(src, Path(tmp) / "missing.png") is None)

    print(f"crisp_fonts: {sum(ok)}/{len(ok)}")
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
