#!/usr/bin/env python3
"""Review sheets for generated PBR maps -> textures/pbr/_preview/<name>_<n>.png.

  py -3.12 tools/pbr/contact_sheet.py --set tools/pbr/sets/starter50.json
  py -3.12 tools/pbr/contact_sheet.py --hash D4B3A26FD275FFE4 4FF7C1B479C897F0 --name check

One row per texture: albedo (as the pack shows it) | normal | height | roughness | metalness | AO | shaded with the
maps under a light from the upper left | the same albedo shaded flat (no maps), so the effect of the maps is visible
at a glance | shaded under a grazing light from the right. Shading is a simple GGX + ambient preview, not the game.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pbrlib as P  # noqa: E402

CELL = 160
COLS = ["albedo", "normal", "height", "rough", "metal", "AO", "lit (maps)", "lit (flat)", "grazing"]
ROWS_PER_SHEET = 14


def load_grey(path, size):
    if not path.exists():
        return None
    with Image.open(path) as im:
        a = np.asarray(im, dtype=np.float32)
    a = a / (65535.0 if a.max() > 255 else 255.0)
    return P.resize(a, *size)


def crop_view(a, cell):
    """A centre crop at the map's own resolution (up to 2x the cell) scaled to the cell, so the detail is visible."""
    H, W = a.shape[:2]
    c = min(H, W, cell * 2)
    y, x = (H - c) // 2, (W - c) // 2
    a = a[y:y + c, x:x + c]
    return P.resize(a, cell, cell)


def to_img(a):
    a = np.clip(a, 0, 1)
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, -1)
    return Image.fromarray((a * 255 + 0.5).astype(np.uint8), "RGB")


def row_images(h, info):
    W, H = info["size"]
    path, _ = P.albedo_path(h, prefer_hd=info.get("input") == "replace")
    rgba = np.clip(P.resize(P.load_rgba(path), W, H), 0, 1)
    with Image.open(P.PBR / f"{h}_n.png") as im:
        n = np.asarray(im.convert("RGB"), dtype=np.float32) / 255 * 2 - 1
    n = P.normalize(n)
    hgt = load_grey(P.PBR / f"{h}_h.png", (W, H))
    r = load_grey(P.PBR / f"{h}_r.png", (W, H))
    m = load_grey(P.PBR / f"{h}_m.png", (W, H))
    ao = load_grey(P.PBR / f"{h}_ao.png", (W, H))
    rgb = rgba[..., :3]
    flat = np.zeros_like(n); flat[..., 2] = 1
    lit = P.shade_preview(rgb, n, r, m, ao)
    lit_flat = P.shade_preview(rgb, flat, np.full_like(r, 0.6), None, np.ones_like(r))
    graze = P.shade_preview(rgb, n, r, m, ao, light=(0.95, 0.0, 0.3))
    if P.MATERIALS.get(info["class"], P.MATERIALS["generic"]).alpha == "cutout":
        alb = rgb * rgba[..., 3:4] + np.array([1, 0, 1]) * (1 - rgba[..., 3:4]) * 0.35
    else:
        alb = rgb  # alpha is a mask (reflection, emissive) or unused, not coverage
    cells = [alb, n * 0.5 + 0.5, hgt, r, m if m is not None else np.zeros_like(r), ao, lit, lit_flat, graze]
    return [to_img(crop_view(c, CELL)) for c in cells]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set")
    ap.add_argument("--hash", nargs="*")
    ap.add_argument("--name")
    args = ap.parse_args()
    man = json.load(open(P.PBR / "manifest.json"))["textures"]
    items = []
    if args.set:
        s = json.load(open(args.set))
        items += [(t["hash"].upper(), t.get("note", "")) for t in s["textures"]]
    if args.hash:
        items += [(h.upper(), "") for h in args.hash]
    items = [(h, note) for h, note in items if h in man]
    name = args.name or (Path(args.set).stem if args.set else "sheet")
    out_dir = P.PBR / "_preview"
    out_dir.mkdir(parents=True, exist_ok=True)
    head = 18
    label = 14
    for s0 in range(0, len(items), ROWS_PER_SHEET):
        chunk = items[s0:s0 + ROWS_PER_SHEET]
        sheet = Image.new("RGB", (CELL * len(COLS), head + len(chunk) * (CELL + label)), (32, 32, 32))
        d = ImageDraw.Draw(sheet)
        for c, t in enumerate(COLS):
            d.text((c * CELL + 4, 3), t, fill=(220, 220, 220))
        for r, (h, note) in enumerate(chunk):
            info = man[h]
            y = head + r * (CELL + label)
            for c, im in enumerate(row_images(h, info)):
                sheet.paste(im, (c * CELL, y))
            st = info["stats"]
            d.text((4, y + CELL), f"{h} {info['class']} {info['input']} {info['size'][0]}x{info['size'][1]} "
                                  f"r~{st['rough_mean']:.2f} {note}", fill=(255, 230, 90))
        out = out_dir / f"{name}_{s0 // ROWS_PER_SHEET + 1}.png"
        sheet.save(out)
        print(out)


if __name__ == "__main__":
    main()
