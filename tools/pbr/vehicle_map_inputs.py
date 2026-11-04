#!/usr/bin/env python3
"""Write the explicit maps inventory vehicle_pack.py takes (schema mc3-vehicle-map-inputs-v1).

For every generated vehicle texture (or a set file's textures) whose maps are all in
textures/pbr (generate.py: <hash>_n, _ao, _r and, for metal classes, _m) at one size, it records each map's file
and SHA-256, relative to --maps-root. Textures with a map missing or a size mismatch are listed and left out. This
only describes files: it selects no capture, approves nothing and installs nothing.

  py -3.12 -I tools/pbr/vehicle_map_inputs.py [--set SET] [--maps-root DIR] [--out FILE]

Prints the inventory's SHA-256 for vehicle_pack.py --maps-sha256.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
NATIVE = Path(os.environ.get("MC3_NATIVE") or ROOT / "templates" / "game" / "out" / "native")
ROLES = (("normal", "n"), ("ao", "ao"), ("roughness", "r"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", type=Path, help="only this set's textures (default: every generated texture whose disc "
                                                "location is a vehicle folder, from textures/pbr/manifest.json)")
    ap.add_argument("--maps-root", type=Path, default=NATIVE / "textures" / "pbr")
    ap.add_argument("--out", type=Path, default=NATIVE / "textures" / "vehicle_map_inputs.json")
    a = ap.parse_args(argv)
    if a.set:
        hashes = [t["hash"].upper() for t in json.loads(a.set.read_text(encoding="utf-8"))["textures"]]
    else:
        generated = json.loads((a.maps_root / "manifest.json").read_text(encoding="utf-8"))
        print(f"generator normal convention: {generated.get('normal_convention')}")
        hashes = sorted(h.upper() for h, t in generated["textures"].items()
                        if "vehicle" in t.get("from", "").replace("\\", "/").lower())
    textures, skipped = {}, []
    for h in hashes:
        files = {role: a.maps_root / f"{h}_{suffix}.png" for role, suffix in ROLES}
        metal = a.maps_root / f"{h}_m.png"
        if metal.exists():
            files["metallic"] = metal
        if not all(p.exists() for p in files.values()):
            skipped.append((h, "a map is missing"))
            continue
        sizes = set()
        for p in files.values():
            with Image.open(p) as im:
                sizes.add(im.size)
        if len(sizes) != 1:
            skipped.append((h, f"map sizes differ {sorted(sizes)}"))
            continue
        textures[h] = {role: {"file": p.relative_to(a.maps_root).as_posix(), "sha256": sha256(p)}
                       for role, p in files.items()}
    data = (json.dumps({"schema": "mc3-vehicle-map-inputs-v1", "textures": textures}, indent=1, sort_keys=True)
            + "\n").encode("utf-8")
    a.out.write_bytes(data)  # bytes as hashed: write_text would turn the newlines into CRLF on Windows
    for h, why in skipped:
        print(f"  skipped {h}: {why}")
    print(f"{len(textures)} of {len(hashes)} textures in {a.out}")
    print(f"maps sha256 = {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
