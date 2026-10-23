#!/usr/bin/env python3
"""Side-by-side of the normal-map methods for a few textures -> textures/pbr/_preview/compare_<name>.png.
Writes no maps; for choosing the default method and tuning the class priors.

  py -3.12 tools/pbr/compare_methods.py --set tools/pbr/sets/starter50.json --pick 0 12 20 38 48
  py -3.12 tools/pbr/compare_methods.py --hash D4B3A26FD275FFE4 --class brick

Columns: albedo, then for each method its normal map and a grazing-light render (same roughness for all, so only the
normals differ). Also prints per method the run time and how strongly it agrees with the classical estimate.
"""
import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pbrlib as P  # noqa: E402
from contact_sheet import crop_view, to_img  # noqa: E402

CELL = 192


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set")
    ap.add_argument("--pick", nargs="*", type=int, help="indices into the set")
    ap.add_argument("--hash", nargs="*")
    ap.add_argument("--class", dest="cls", default="generic")
    ap.add_argument("--methods", nargs="*", default=["classic", "deep", "pbrify", "ensemble"])
    ap.add_argument("--max-size", type=int, default=1024)
    ap.add_argument("--input", choices=["source", "replace"], default="source")
    ap.add_argument("--name", default="methods")
    args = ap.parse_args()
    items = []
    if args.set:
        t = json.load(open(args.set))["textures"]
        for i in (args.pick if args.pick else range(len(t))):
            items.append((t[i]["hash"], t[i]["class"]))
    for h in args.hash or []:
        items.append((h.upper(), args.cls))
    index = P.load_index()
    models = {"deep": P.DeepBump("cpu") if P.DeepBump.available() else None,
              "pbrify": P.PBRify(6) if P.PBRify.available() else None}
    cols = 1 + 2 * len(args.methods)
    sheet = Image.new("RGB", (cols * CELL, 18 + len(items) * (CELL + 14)), (32, 32, 32))
    d = ImageDraw.Draw(sheet)
    d.text((4, 3), "albedo", fill=(220, 220, 220))
    for j, mth in enumerate(args.methods):
        d.text(((1 + 2 * j) * CELL + 4, 3), f"{mth} normal", fill=(220, 220, 220))
        d.text(((2 + 2 * j) * CELL + 4, 3), f"{mth} grazing", fill=(220, 220, 220))
    for r, (h, cls) in enumerate(items):
        e = P.parse_index_entry(index[h])
        rgba, src, scale = P.load_working(h, e, args.input, 4.0, args.max_size)
        y = 18 + r * (CELL + 14)
        sheet.paste(to_img(crop_view(rgba[..., :3], CELL)), (0, y))
        ref = None
        line = [h, cls]
        for j, mth in enumerate(args.methods):
            t = time.time()
            res = P.generate(rgba, cls, scale, models, mth)
            dt = time.time() - t
            if ref is None:
                ref = res.normal
            agree = np.corrcoef(ref[..., :2].ravel(), res.normal[..., :2].ravel())[0, 1]
            line.append(f"{res.info['method']} {dt:.1f}s r={agree:.2f}")
            lit = P.shade_preview(rgba[..., :3], res.normal, np.full_like(res.rough, 0.7), None, np.ones_like(res.rough),
                                  light=(0.95, 0.0, 0.3))
            sheet.paste(to_img(crop_view(res.normal * 0.5 + 0.5, CELL)), ((1 + 2 * j) * CELL, y))
            sheet.paste(to_img(crop_view(lit, CELL)), ((2 + 2 * j) * CELL, y))
        d.text((4, y + CELL), f"{h} {cls}", fill=(255, 230, 90))
        print(" | ".join(line), flush=True)
    out = P.PBR / "_preview" / f"compare_{args.name}.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
