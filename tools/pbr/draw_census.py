"""Read-only census of captured frames (XBE_CAPTURE_DIR frame.txt + vs_*.hlsl) against the dedicated PBR
draw gates in D3D12Backend::TryPbrDraw. It reports which original draw facts block the opt-in path, per
vertex shader class and as a funnel. Capture text only: no game, GPU, settings or asset access.

    python tools/pbr/draw_census.py CAPTURE_DIR [CAPTURE_DIR ...] [--json] [--skip GATE,GATE]

--skip drops gates from the funnel, to size what lifting them (e.g. triangle_list,ff_lighting) would reach.
Since 2026-10-29 TryPbrDraw expands strips, derives face normals and has a LIGHTING-off baked mode, so the
current path is `--skip triangle_list,authored_normal,ff_lighting`; the full funnel shows the morning baseline.

The gates are the capture-visible subset, checked in TryPbrDraw's order: fixed transform (the translated
HLSL is the fixed-function `mul(ip, wvp)` path, not a program or pretransformed quad), a triangle list
(prim 5), an authored normal (register 2 read from the vertex stream), fixed-function lighting enabled,
one texture stage and opaque blending. Alpha test, fog, cutouts and material approval are not in
frame.txt, so a draw passing every gate here is a candidate, not an eligible draw."""
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

DRAW = re.compile(r"^(\d{4}) draw .* prim (\d+) count (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+) blend (\d+) (\d+) (\d+) "
                  r"zen (\d+) \d+ zwrite (\d+)")
GATES = ["fixed_transform", "triangle_list", "authored_normal", "ff_lighting", "single_stage", "opaque"]


def shader_facts(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    return {"fixed": "mul(ip, wvp)" in text, "normal": bool(re.search(r"\bv2 : ATTR2;", text))}


def draws(capture):
    """(draw facts, vs hlsl name) for every draw in one capture. A vs_*.hlsl line follows the first draw that
    compiled that translation; later draws with the same handle and stream strides reuse it."""
    lines = (capture / "frame.txt").read_text(encoding="utf-8", errors="replace").splitlines()
    out, cur, seen = [], None, {}
    for line in lines:
        m = DRAW.match(line)
        if m:
            idx, prim, count, vs, ps, ben, src, dst, zen, zwrite = m.groups()
            cur = {"capture": capture.name, "draw": idx, "prim": int(prim), "count": int(count), "vs": vs, "ps": ps,
                   "blend": ben == "1" and not (src == "1" and dst == "0"), "zwrite": zwrite == "1",
                   "stages": line.count("| t"), "lighting": False, "strides": "", "hlsl": None}
            out.append(cur)
            continue
        if re.match(r"^\d{4} ", line):
            cur = None
            continue
        if cur is None:
            continue
        s = line.strip()
        if s.startswith("lighting "):
            cur["lighting"] = True
        elif re.match(r"vb\d stride", s):
            cur["strides"] += s.split(" v0 ")[0] + ";"
        elif s.startswith("vs_") and s.endswith(".hlsl"):
            cur["hlsl"] = s
    for d in out:
        key = (d["vs"], d["strides"])
        if d["hlsl"]:
            seen[key] = d["hlsl"]
        else:
            d["hlsl"] = seen.get(key)
    return out


def gate_results(d, facts):
    handle = int(d["vs"], 16)
    pretransformed = handle < 0x10000 and bool(handle & 0x4)
    f = facts.get(d["hlsl"]) if d["hlsl"] else None
    return {
        "fixed_transform": bool(f and f["fixed"]) and not pretransformed,
        "triangle_list": d["prim"] == 5,
        "authored_normal": bool(f and f["normal"]) or (handle < 0x10000 and bool(handle & 0x10)),
        "ff_lighting": d["lighting"],
        "single_stage": d["stages"] == 1,
        "opaque": not d["blend"] and d["zwrite"],
    }


def census(captures, skip=()):
    gates = [g for g in GATES if g not in skip]
    facts, rows = {}, []
    for cap in captures:
        for p in cap.glob("vs_*.hlsl"):
            facts.setdefault(p.name, shader_facts(p))
        rows += draws(cap)
    funnel = Counter()
    first_block = Counter()
    classes = defaultdict(lambda: {"draws": 0, "first_block": Counter()})
    for d in rows:
        g = gate_results(d, facts)
        blocked = next((name for name in gates if not g[name]), None)
        alive = True
        for name in gates:
            alive = alive and g[name]
            if alive:
                funnel[name] += 1
        first_block[blocked or "candidate"] += 1
        c = classes[f'{d["vs"]} {d["hlsl"] or "?"}']
        c["draws"] += 1
        c["first_block"][blocked or "candidate"] += 1
    return {
        "captures": len(captures), "draws": len(rows),
        "skipped": list(skip),
        "funnel": {name: funnel[name] for name in gates},
        "first_block": dict(first_block.most_common()),
        "classes": {k: {"draws": v["draws"], "first_block": dict(v["first_block"].most_common())}
                    for k, v in sorted(classes.items(), key=lambda kv: -kv[1]["draws"])},
    }


def main(argv):
    as_json = "--json" in argv
    skip, paths = [], []
    it = iter(a for a in argv if a != "--json")
    for a in it:
        if a == "--skip":
            skip += next(it, "").split(",")
        else:
            paths.append(Path(a))
    if any(s not in GATES for s in skip):
        print(f"--skip takes gates from {', '.join(GATES)}")
        return 2
    caps = [c for c in paths if (c / "frame.txt").exists()]
    if not caps:
        print(__doc__)
        return 2
    r = census(caps, skip)
    if as_json:
        print(json.dumps(r, indent=1))
        return 0
    print(f'{r["draws"]} draws in {r["captures"]} captures')
    print("passing each gate in order:", ", ".join(f"{k} {v}" for k, v in r["funnel"].items()))
    print("first blocking gate:", ", ".join(f"{k} {v}" for k, v in r["first_block"].items()))
    print("largest vertex shader classes:")
    for k, v in list(r["classes"].items())[:15]:
        print(f'  {v["draws"]:7d}  {k}  {v["first_block"]}')
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
