#!/usr/bin/env python3
"""Every texture on the disc against the HD pack and the installed PBR pack, by area.

Joins textures/source_index.json (every disc texture extract_all.py found, by content hash), textures/source and
textures/replace (HD), the vinyl masks upscale.py deliberately skips, the installed PBR pack named in settings.toml
[pbr] (materials-v1.toml), textures/pbr (generated maps) and logs/texture_coverage.tsv (what a drive actually drew,
MC3_TEXTURE_COVERAGE=1). Prints a table per area and writes logs/texture_audit.tsv with one row per texture and the
reason for every gap.

  py -3.12 -I tools/texture_pack/audit_coverage.py [--native DIR]
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import upscale  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CITIES = ("sd", "atlanta", "detroit", "tokyo")
CITY = re.compile(r"resources/(city|prop)/(%s)" % "|".join(CITIES))


def area_of(where):
    w = where.replace("\\", "/").lower()
    m = CITY.search(w)
    if m:
        return f"{m.group(2)} {m.group(1)}"
    for key, name in (("vehicle/va_tra", "trains"), ("vehicle/va_", "traffic cars"), ("resources/vehicle", "player cars"),
                      ("vehicle/", "car parts"), ("fonts/", "fonts/UI"), ("fx/", "effects"), ("ped", "pedestrians"),
                      ("cutscene", "cutscenes"), ("media/", "menus/UI"), (".xbpf", "car paint pools"),
                      ("texture_x/", "UI/HUD textures")):
        if key in w:
            return name
    return "other"


def pack_materials(native):
    settings = native / "settings.toml"
    text = settings.read_text(encoding="utf-8") if settings.exists() else ""
    m = re.search(r"(?ms)^\[pbr\]\s*$(.*?)(?=^\[|\Z)", text)
    p = re.search(r"(?m)^pack_path\s*=\s*'([^']*)'", m.group(1)) if m else None
    pack = native / (p.group(1) if p else "pbr-cities")
    manifest = pack / "materials-v1.toml"
    if not manifest.exists():
        return pack, set()
    return pack, set(re.findall(r'(?m)^texture = "([0-9A-F]{16})"', manifest.read_text(encoding="utf-8")))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--native", type=Path, default=Path(os.environ.get("MC3_NATIVE") or ROOT / "templates" / "game" /
                                                      "out" / "native"))
    a = ap.parse_args(argv)
    textures = a.native / "textures"
    index = json.loads((textures / "source_index.json").read_text(encoding="utf-8"))
    masks = upscale.excluded_masks(textures)
    replacements = upscale.index_replacements(textures) if hasattr(upscale, "index_replacements") else {
        p.stem[:16].upper(): p for p in (textures / "replace").rglob("*.png") if len(p.stem) >= 16}
    pack, materials = pack_materials(a.native)
    seen, pbr_drawn = {}, {}
    cov = a.native / "logs" / "texture_coverage.tsv"
    if cov.exists():
        for line in cov.read_text().splitlines():
            parts = line.split("\t")
            if len(parts) == 3 and re.fullmatch(r"[0-9A-Fa-f]{16}", parts[0]):
                seen[parts[0].upper()] = int(parts[1]); pbr_drawn[parts[0].upper()] = int(parts[2])
    rows, table = [], defaultdict(Counter)
    for h, where in index.items():
        h = h.upper()
        area = area_of(where)
        src = (textures / "source" / f"{h}.png").exists()
        hd = h in replacements
        city = area.endswith(" city") or area.endswith(" prop")
        maps = all((textures / "pbr" / f"{h}_{k}.png").exists() for k in ("n", "r")) if city else False
        mat = h in materials
        hd_why = "" if hd else ("vinyl mask (tinted at run time, kept console)" if f"{h}.png" in masks else
                                "no source PNG" if not src else
                                {"tiny": "too small to gain detail (kept console)", "flat": "one flat colour (kept console)",
                                 "size_cap": "already at the size cap"}.get(
                                    upscale.inspect_source(textures / "source" / f"{h}.png")[2], "not upscaled"))
        pbr_why = "" if mat else ("outside the PBR pack's scope (city scenery and props only)" if not city else
                                  "no generated maps" if not maps else "not in the installed pack")
        t = table[area]
        t["textures"] += 1; t["hd"] += hd; t["pbr"] += mat; t["seen"] += h in seen
        t["seen_no_hd"] += (h in seen and not hd); t["seen_pbr"] += pbr_drawn.get(h, 0) > 0
        t["city_no_pbr"] += city and not mat
        rows.append((h, area, where, int(hd), hd_why, int(mat), pbr_why, seen.get(h, 0), pbr_drawn.get(h, 0)))
    unknown_seen = [h for h in seen if h not in {k.upper() for k in index}]
    out = a.native / "logs" / "texture_audit.tsv"
    out.parent.mkdir(exist_ok=True)
    with out.open("w", encoding="utf-8") as f:
        f.write("hash\tarea\twhere\thd\thd_gap\tpbr\tpbr_gap\tdraws\tpbr_draws\n")
        for r in sorted(rows, key=lambda r: (r[1], r[0])):
            f.write("\t".join(map(str, r)) + "\n")
    print(f"PBR pack: {pack.name} ({len(materials)} materials); census: {len(seen)} textures seen")
    print(f"{'area':16} {'textures':>8} {'HD':>6} {'PBR':>6} {'seen':>6} {'seen,noHD':>9} {'seenPBR':>8} {'city,noPBR':>10}")
    total = Counter()
    for area in sorted(table):
        t = table[area]; total.update(t)
        print(f"{area:16} {t['textures']:8} {t['hd']:6} {t['pbr']:6} {t['seen']:6} {t['seen_no_hd']:9} {t['seen_pbr']:8} "
              f"{t['city_no_pbr']:10}")
    print(f"{'total':16} {total['textures']:8} {total['hd']:6} {total['pbr']:6} {total['seen']:6} {total['seen_no_hd']:9} "
          f"{total['seen_pbr']:8} {total['city_no_pbr']:10}")
    gaps = Counter(r[4] for r in rows if r[4]) + Counter(r[6] for r in rows if r[6])
    for why, n in gaps.most_common():
        print(f"  {n:6}  {why}")
    print(f"  {len(unknown_seen):6}  drawn textures not in source_index (render targets, movies, generated)")
    print(f"rows: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
