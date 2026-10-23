#!/usr/bin/env python3
"""First-pass material class for every texture in source_index.json -> textures/pbr/classes.json.

  py -3.12 tools/pbr/classify.py            # all 12,714 sources (about a minute)
  py -3.12 tools/pbr/classify.py --stats    # print the class histogram of an existing classes.json

Where the texture came from decides most classes (rims, tyres, brakes, riders, shared car parts, car and traffic
atlases, UI/flash, vinyl decals). City bundles carry no texture names, so those are classified from image statistics
(alpha use, saturation, brightness, hue, tiling) and marked "confidence": "low"; docs/PBR_PLAN.md describes the runtime
material census that is meant to replace these guesses with what the draws actually do. A curated set file
(tools/pbr/sets/*.json) always overrides this file.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pbrlib as P  # noqa: E402

OUT = P.PBR / "classes.json"

SHARED_RULES = [
    (r"carbon", "carbon"), (r"grill|radiator", "grill"), (r"exh_", "exhaust"),
    (r"tl1elgte|signal_light|fog_light", "car_light"), (r"window", "car_glass"),
    (r"chrm|va_whl", "chrome"), (r"inside_tire", "tire"), (r"suspension|h2_step|kazada", "metal_painted"),
    (r"fx_|shadow|neon|checker", "skip"),
]


def by_path(loc: str):
    p = loc.lower()
    if p.startswith("flash/") or "/flash/" in p or p.startswith("disc:media") or re.search(r"(^|[:/])fonts?/", p):
        return "ui", "path"
    if "decal" in p and ".xbpf" in p:
        return "skip", "path"  # vinyls: palette masks the game tints, not surfaces
    for key, cls in (("rim.xbpf", "rim"), ("tire.xbpf", "tire"), ("brake.xbpf", "brake"), ("rider.xbpf", "rider")):
        if key in p:
            return cls, "path"
    if "shared_texture" in p or "shared_tex.xbpf" in p:
        for pat, cls in SHARED_RULES:
            if re.search(pat, p):
                return cls, "path"
        return "car_atlas", "path"
    if "resources/vehicle/vp_" in p:
        return "car_atlas", "path"
    if "vehicle/va_" in p:
        return "traffic_atlas", "path"
    if "cutscene/" in p or "_peds" in p:
        return "rider", "path"
    if p.startswith("assets_x.dat:fx/") or "/fx/" in p or "particle" in p:
        return "skip", "path"
    return None, None


def features(path: Path) -> dict:
    a = P.load_rgba(path)
    rgb, al = a[..., :3], a[..., 3]
    lum = P.luminance(rgb)
    sat = P.saturation(rgb)
    # tiling: how well the right edge continues into the left one, against neighbouring columns' own difference
    edge = float(np.mean(np.abs(lum[:, 0] - lum[:, -1])))
    inner = float(np.mean(np.abs(lum[:, 1:] - lum[:, :-1]))) + 1e-4
    g = rgb[..., 1] - np.maximum(rgb[..., 0], rgb[..., 2])
    return {
        "lum": float(lum.mean()), "lum_std": float(lum.std()), "sat": float(sat.mean()),
        "alpha_mean": float(al.mean()), "alpha_zero": float((al < 0.1).mean()), "alpha_full": float((al > 0.9).mean()),
        "green": float((g > 0.04).mean()), "tile": edge / inner,
        "bright_frac": float((lum > 0.6).mean()), "dark_frac": float((lum < 0.08).mean()),
    }


def by_image(f: dict, fmt: str, loc: str):
    cut = fmt == "DXT5" and f["alpha_zero"] > 0.15 and f["alpha_full"] > 0.15 and f["alpha_zero"] + f["alpha_full"] > 0.8
    if cut and f["green"] > 0.25:
        return "foliage"
    if f["dark_frac"] > 0.55 and f["bright_frac"] > 0.03:
        return "windows_night"
    speck = fmt == "DXT5" and f["alpha_mean"] < 0.3 and not cut
    if f["sat"] < 0.14 and f["tile"] < 1.6 and 0.12 < f["lum"] < 0.5 and f["lum_std"] < 0.12:
        return "road_asphalt" if speck else "concrete"
    if f["green"] > 0.35 and f["sat"] > 0.2:
        return "terrain"
    if f["sat"] > 0.35:
        return "signage"
    return "facade"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()
    if args.stats:
        d = json.load(open(OUT))
        c = collections.Counter((v["class"], v["confidence"]) for v in d["textures"].values())
        for (cls, conf), n in sorted(c.items(), key=lambda x: -x[1]):
            print(f"{n:6d}  {cls:16s} {conf}")
        return
    idx = P.load_index()
    out = {}
    for i, (h, v) in enumerate(idx.items()):
        e = P.parse_index_entry(v)
        cls, how = by_path(e["loc"])
        conf = "path"
        if cls is None:
            src = P.SOURCE / f"{h}.png"
            if not src.exists():
                continue
            if max(e["w"], e["h"]) < 16:
                cls, conf = "skip", "tiny"
            else:
                f = features(src)
                cls, conf = by_image(f, e["fmt"], e["loc"]), "low"
        out[h] = {"class": cls, "confidence": conf, "fmt": e["fmt"], "size": [e["w"], e["h"]]}
        if i % 1000 == 0:
            print(f"{i}/{len(idx)}", flush=True)
    P.PBR.mkdir(parents=True, exist_ok=True)
    tmp = OUT.with_suffix(".part")
    json.dump({"version": 1, "textures": out}, open(tmp, "w"), indent=0)
    tmp.replace(OUT)
    c = collections.Counter(v["class"] for v in out.values())
    print(json.dumps(dict(c.most_common()), indent=1))


if __name__ == "__main__":
    main()
