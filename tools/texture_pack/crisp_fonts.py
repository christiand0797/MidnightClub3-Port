#!/usr/bin/env python3
"""Rebuild the HD font sheets with clean glyph edges.

The game's menu and HUD lettering (the NETWORKING header, menu items, button labels) comes from 128x128 palettised
font atlases: 164 in fonts/texture_x/*.tex and about 100 more embedded in the menu screens (flash/*.xbck, found here
by content: one ink colour, many separate glyphs; the menu headers' are pure on/off alpha). Each is an atlas of glyphs only ~10-40 texels tall. The AI upscale of such tiny
pixel glyphs keeps their staircase as wobbly, blobby edges, and the menus magnify the glyphs well past 4x, so the
letters still looked low resolution.

For every hard-edged sheet (glyph alpha mostly fully on or off) this rebuilds the alpha the way a distance-field font
renders: the console's own alpha is upsampled smoothly to 8x, its staircase rounded off, and re-thresholded with a
narrow anti-aliased ramp, so every letter gets a smooth outline at any size. Colour comes from the existing HD
upscale, spread under the new edge so no dark fringe shows. Soft sheets (drop shadows, glows: the "...s" sheets)
are left as they are. The PNG carries a text mark, so a second run skips sheets already done.

Writes textures/replace/<hash>.png (a new file, replacing any hard link rather than writing through it). Run after
upscale.py (it calls this).

  py -3.12 tools/texture_pack/crisp_fonts.py [--textures DIR] [--only HASH ...] [--force]
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, PngImagePlugin
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
MARK = "mc3-crisp-fonts-v1"
SCALE = 8          # output texels per console texel
MAX_SIDE = 2048
ROUND = 0.18       # staircase rounding, in console texels (gaussian sigma)
RAMP = 1.6         # anti-aliased edge width, in output texels


def is_font(where):
    return "fonts/" in where.replace("\\", "/").lower()


def is_ui(where):
    w = where.replace("\\", "/").lower()
    return not re.search(r"resources/(city|prop)/", w) and "vehicle" not in w


def glyph_sheet(rgba):
    """A font atlas the menus embed (flash/*.xbck): one ink colour, many separate glyphs, mostly empty."""
    cover = rgba[..., 3] > 128
    if cover.sum() < 64 or cover.mean() > 0.6:
        return False
    if rgba[..., :3][cover].astype(np.float32).std(axis=0).max() > 12:
        return False
    return ndimage.label(cover)[1] >= 20


def hard_edged(alpha):
    """True for glyphs with solid cores and thin anti-aliased edges; false for soft shadows/glows and for hairline
    fonts with no solid core (thresholding those would erode their strokes)."""
    part = (alpha > 8) & (alpha < 247)
    if (alpha > 8).sum() < 16:
        return False
    if not part.any():
        return True  # pure on/off glyphs (the menus' embedded fonts): the staircase case itself
    core = ndimage.maximum_filter(alpha >= 247, size=3)
    return (part & core).sum() / part.sum() >= 0.6


def crisp_alpha(alpha, scale):
    """Console alpha (uint8, HxW) -> smooth-edged alpha at scale x, float 0..1."""
    a = alpha.astype(np.float32) / 255.0
    up = np.clip(ndimage.zoom(a, scale, order=3), 0, 1)
    up = ndimage.gaussian_filter(up, ROUND * scale)
    # the smoothed edge spans about `scale` output texels per unit of alpha; a ramp RAMP texels wide around 0.5
    grad = np.hypot(*np.gradient(up))
    slope = np.median(grad[(up > 0.3) & (up < 0.7)]) if np.any((up > 0.3) & (up < 0.7)) else 1.0 / scale
    width = max(RAMP * slope, 1e-3)
    return np.clip((up - 0.5) / width + 0.5, 0, 1)


def spread_colour(rgb, alpha, sigma):
    """Colour carried under its own coverage outward (premultiplied blur), so a grown edge is not dark."""
    w = alpha[..., None]
    num = np.dstack([ndimage.gaussian_filter(rgb[..., c] * alpha, sigma) for c in range(3)])
    den = ndimage.gaussian_filter(alpha, sigma)[..., None]
    spread = np.where(den > 1e-4, num / np.maximum(den, 1e-4), rgb)
    return rgb * w + spread * (1 - w)


def rebuild(source_png, hd_png):
    src = np.asarray(Image.open(source_png).convert("RGBA"))
    if not hard_edged(src[..., 3]):
        return None
    scale = min(SCALE, MAX_SIDE // max(src.shape[:2]))
    alpha = crisp_alpha(src[..., 3], scale)
    h, w = alpha.shape
    before = src[..., 3].astype(np.float64).sum() / 255.0 * scale * scale
    if not 0.75 <= alpha.sum() / max(before, 1.0) <= 1.25:
        return None  # the glyphs would grow or thin visibly: keep the AI upscale
    base = Image.open(hd_png).convert("RGBA") if hd_png.exists() else Image.open(source_png).convert("RGBA")
    base = np.asarray(base.resize((w, h), Image.LANCZOS)).astype(np.float32) / 255.0
    # colour from the glyph interiors only: the AI upscale darkens its own edges, which would show as a rim
    core = ndimage.binary_erosion(base[..., 3] > 0.9, iterations=max(1, scale // 2)).astype(np.float32)
    if core.sum() < 16:
        core = base[..., 3]
    rgb = spread_colour(base[..., :3], core, sigma=scale * 0.75)
    rgb = spread_colour(rgb, np.maximum(core, ndimage.gaussian_filter(core, scale)), sigma=scale * 2)
    out = np.dstack([rgb, alpha])
    return Image.fromarray(np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA")


def marked(png):
    try:
        return Image.open(png).text.get("mc3") == MARK
    except Exception:
        return False


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--textures", type=Path, default=ROOT / "templates" / "game" / "out" / "native" / "textures")
    ap.add_argument("--only", nargs="*", help="these hashes only")
    ap.add_argument("--force", action="store_true", help="rebuild sheets already marked")
    a = ap.parse_args(argv)
    index_path = a.textures / "source_index.json"
    if not index_path.exists():
        print("crisp fonts: no source_index.json (run extract_all.py first); nothing to do")
        return 0
    index = json.loads(index_path.read_text())
    hashes = []
    for h, where in index.items():
        if is_font(where):
            hashes.append(h)
        elif is_ui(where) and (a.textures / "source" / f"{h}.png").exists():
            if glyph_sheet(np.asarray(Image.open(a.textures / "source" / f"{h}.png").convert("RGBA"))):
                hashes.append(h)
    if a.only:
        hashes = [h for h in hashes if h in {x.upper() for x in a.only}]
    done = soft = kept = missing = 0
    for h in sorted(hashes):
        src, out = a.textures / "source" / f"{h}.png", a.textures / "replace" / f"{h}.png"
        if not src.exists():
            missing += 1
            continue
        if out.exists() and marked(out) and not a.force:
            kept += 1
            continue
        img = rebuild(src, out)
        if img is None:
            soft += 1
            continue
        info = PngImagePlugin.PngInfo()
        info.add_text("mc3", MARK)
        tmp = out.with_suffix(".crisp.tmp")
        img.save(tmp, format="PNG", pnginfo=info)
        os.replace(tmp, out)  # a new file: never write through a hard link shared with an archive
        done += 1
    print(f"crisp fonts: {done} rebuilt, {kept} already done, {soft} soft sheets kept, {missing} without source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
