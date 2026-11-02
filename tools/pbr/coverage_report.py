#!/usr/bin/env python3
"""Per-city texture coverage from a live drive: which textures the game actually drew, and how many went through PBR.

The renderer writes logs/texture_coverage.tsv beside mc3.exe when started with MC3_TEXTURE_COVERAGE=1 (d3d12.cpp,
WriteCoverage): content hash, draws into the frame, draws through the dedicated PBR path; runs add up. This joins it
with textures/source_index.json (where each texture lives on the disc) and the installed pack's materials.

  py -3.12 -I tools/pbr/coverage_report.py [--native DIR] [--list]

Per area (each city's scenery and props, cars, everything else) it prints textures seen, seen with a pack material,
seen drawn through PBR, and seen city textures that have no material: those are the conversions still missing.
"""
import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CITIES = ("sd", "atlanta", "detroit", "tokyo")
AREA = re.compile(r"resources/(city|prop)/(%s)" % "|".join(CITIES))


def area_of(where):
    m = AREA.search(where.replace("\\", "/").lower())
    if m:
        return f"{m.group(2)} {m.group(1)}"
    w = where.replace("\\", "/").lower()
    if "vehicle" in w:
        return "cars"
    return "other"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--native", type=Path, default=ROOT / "templates" / "game" / "out" / "native")
    ap.add_argument("--list", action="store_true", help="list the seen city textures without a material")
    a = ap.parse_args()
    cov_path = a.native / "logs" / "texture_coverage.tsv"
    if not cov_path.exists():
        sys.exit(f"{cov_path}: none yet; drive with MC3_TEXTURE_COVERAGE=1")
    index = json.load(open(a.native / "textures" / "source_index.json", encoding="utf-8"))
    pack = set()
    for path in a.native.glob("pbr-*/materials-v1.toml"):
        pack |= set(re.findall(r'^texture = "([0-9A-F]{16})"', path.read_text(encoding="utf-8"), re.M))
    rows = defaultdict(lambda: {"seen": 0, "in_pack": 0, "pbr": 0, "missing": []})
    total = defaultdict(int)
    for h, where in index.items():
        total[area_of(where)] += 1
    for line in cov_path.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        h, draws, pbr = parts[0].upper(), int(parts[1]), int(parts[2])
        r = rows[area_of(index.get(h, ""))]
        r["seen"] += 1
        if h in pack:
            r["in_pack"] += 1
        elif AREA.search(index.get(h, "").replace("\\", "/").lower()):
            r["missing"].append(h)
        if pbr:
            r["pbr"] += 1
    print(f"{'area':16s} {'on disc':>8s} {'seen':>6s} {'in pack':>8s} {'drawn PBR':>10s} {'no material':>12s}")
    for area in sorted(rows, key=lambda k: (k not in [f"{c} {t}" for c in CITIES for t in ("city", "prop")], k)):
        r = rows[area]
        print(f"{area:16s} {total.get(area, 0):8d} {r['seen']:6d} {r['in_pack']:8d} {r['pbr']:10d} {len(r['missing']):12d}")
    if a.list:
        for area, r in sorted(rows.items()):
            for h in r["missing"]:
                print(f"{area}\t{h}\t{index.get(h, '')}")


if __name__ == "__main__":
    main()
