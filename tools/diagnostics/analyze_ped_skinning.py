"""Read-only indexed-skin capture analysis. Geometry/root proximity is evidence, not a guest entity identity."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re

DRAW = re.compile(r"^(\d+) draw .*? count (\d+) vs ([0-9A-F]+)")
CONSTANT = re.compile(r"^\s+c(-?\d+) ([^ ]+) ([^ ]+) ([^ ]+) ([^ ]+)$")


def read_capture(path: Path) -> list[dict]:
    frame = path / "frame.txt" if path.is_dir() else path
    blocks, current = [], None
    for line in frame.read_text(encoding="utf-8", errors="replace").splitlines():
        match = DRAW.match(line)
        if match:
            current = {"draw": int(match[1]), "count": int(match[2]), "vs": match[3], "lines": []}
            blocks.append(current)
        elif current is not None:
            current["lines"].append(line)
    shader_handles = set()
    for block in blocks:
        for line in block["lines"]:
            if line.strip().startswith("vs_"):
                shader = frame.parent / line.strip()
                if shader.is_file():
                    source = shader.read_text(encoding="utf-8", errors="replace")
                    if "c[clamp(a0" in source and "((i.v12)" in source and "c[105]" in source:
                        shader_handles.add(block["vs"])
    constants = [[0.0] * 4 for _ in range(192)]
    result = []
    for block in blocks:
        lines = block.pop("lines")
        # 'verts' marks a complete constant snapshot containing only nonzero registers; larger draws use deltas.
        if any(line.strip().startswith("verts") for line in lines):
            constants = [[0.0] * 4 for _ in range(192)]
        for line in lines:
            match = CONSTANT.match(line)
            if match and 0 <= (index := int(match[1]) + 96) < 192:
                constants[index] = [float(match[i]) for i in range(2, 6)]
        explicit = any(line.strip().startswith("skin scale") for line in lines)
        if not explicit and block["vs"] not in shader_handles:
            continue
        block["constants"] = [row[:] for row in constants]
        block["scale"] = constants[105][3]
        block["root"] = [constants[i][3] for i in range(3)]
        block["bone_rows"] = []
        block["vdata"] = ""
        block["layout"] = ""
        block["vertex_fingerprint"] = ""
        for line in lines:
            text = line.strip()
            if text.startswith("skin scale"):
                block["scale"] = float(text.split()[2])
                block["bone_rows"] = [int(v) for v in text.partition("bone_rows")[2].split()]
            elif text.startswith("skin vdata"):
                block["vdata"] = text.removeprefix("skin vdata").strip()
            elif text.startswith("skin layout"):
                block["layout"] = text.removeprefix("skin layout").strip()
            elif text.startswith("v0 ") or text.startswith("vb0 stride"):
                block["vertex_fingerprint"] = text
        block["geometry"] = [block["vdata"] or block["vertex_fingerprint"], block["count"], block["layout"]]
        block["issues"] = []
        if block["scale"] != constants[105][3]:
            block["issues"].append("diagnostic scale disagrees with c9 constant snapshot")
        if block["scale"] != 256:
            block["issues"].append("skin index scale differs from original256")
        if explicit and not block["bone_rows"]:
            block["issues"].append("no classified bone indices; inspect declaration/vertex data")
        if not all(math.isfinite(v) for row in constants for v in row):
            block["issues"].append("non-finite vertex constants")
        result.append(block)
    return result


def inverse(matrix: list[list[float]]) -> list[list[float]] | None:
    a, b, c = matrix
    cof = [[b[1]*c[2]-b[2]*c[1], a[2]*c[1]-a[1]*c[2], a[1]*b[2]-a[2]*b[1]],
           [b[2]*c[0]-b[0]*c[2], a[0]*c[2]-a[2]*c[0], a[2]*b[0]-a[0]*b[2]],
           [b[0]*c[1]-b[1]*c[0], a[1]*c[0]-a[0]*c[1], a[0]*b[1]-a[1]*b[0]]]
    det = sum(a[i] * cof[i][0] for i in range(3))
    if not math.isfinite(det) or abs(det) < 1e-8:
        return None
    return [[v/det for v in row] for row in cof]


def relative_bone(draw: dict, row: int) -> list[float] | None:
    constants = draw["constants"]
    inv = inverse([constants[i][:3] for i in range(3)])
    if inv is None or row < 0 or row+2 >= 192:
        return None
    # inv(root) * bone cancels whole-character translation and rotation, unlike raw palette comparison.
    r = [constants[row+i][:3] for i in range(3)]
    t = [constants[row+i][3]-constants[i][3] for i in range(3)]
    return [sum(inv[i][k]*r[k][j] for k in range(3)) for i in range(3) for j in range(3)] + [
        sum(inv[i][k]*t[k] for k in range(3)) for i in range(3)]


def compare(before: list[dict], after: list[dict], max_distance: float = 2.0) -> list[dict]:
    pairs, claimed = [], set()
    for a in before:
        if not all(math.isfinite(v) for v in a["root"]):
            continue
        candidates = [(math.dist(a["root"], b["root"]), index, b) for index, b in enumerate(after)
                      if index not in claimed and a["geometry"] == b["geometry"] and
                      all(math.isfinite(v) for v in b["root"])]
        candidates.sort(key=lambda item: item[:2])
        if not candidates or candidates[0][0] > max_distance:
            continue
        if len(candidates)>1 and candidates[1][0]-candidates[0][0] < .05:
            continue  # Shared geometry with indistinguishable nearby roots is not an identity match.
        distance, index, b = candidates[0]
        claimed.add(index)
        rows = sorted(set(a["bone_rows"]) & set(b["bone_rows"]) - {0})
        changes = []
        for row in rows:
            x, y = relative_bone(a, row), relative_bone(b, row)
            if x is not None and y is not None and all(math.isfinite(v) for v in x+y):
                changes.append({"row": row, "max_relative_delta": max(abs(v-w) for v,w in zip(x,y))})
        pairs.append({"before_draw": a["draw"], "after_draw": b["draw"], "vdata": a["vdata"],
                      "match": "same_geometry_and_nearest_root_inferred", "root_distance": distance,
                      "bone_changes": changes, "relative_pose_changed": any(c["max_relative_delta"]>1e-3 for c in changes),
                      "issues": a["issues"]+b["issues"], "observed_nonroot_bones": len(changes)})
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--max-root-distance", type=float, default=2.0)
    args = parser.parse_args()
    frames = [read_capture(path) for path in args.captures]
    report = {"schema": 1, "captures": [{"path": str(path.resolve()), "skin_draws": len(draws),
              "issues": [{"draw": d["draw"], "issues": d["issues"]} for d in draws if d["issues"]]}
              for path,draws in zip(args.captures,frames)], "comparisons": [
              {"before": i, "after": i+1, "pairs": compare(frames[i], frames[i+1], args.max_root_distance)}
              for i in range(len(frames)-1)],
              "limitation": "Geometry/root matching is inferred. No observed pose change alone does not prove a frozen animation."}
    text = json.dumps(report, indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(text+"\n", encoding="utf-8")
    print(text)
    return 0 if all(frames) else 2


if __name__ == "__main__":
    raise SystemExit(main())
