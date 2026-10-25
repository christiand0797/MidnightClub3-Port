#!/usr/bin/env python3
"""CPU-only, read-only pixel diagnostics for completed isolated joined-photo candidates.

Boundary statistics flag lines for visual review. They do not prove equivalence to whole-image RRDB inference.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import loading_art_audit as audit
import loading_art_worker as candidates
import loading_garage_join as joined
import upscale


def line_statistics(rgb, axis, position, margin=32, radius=32):
    values = np.asarray(rgb, np.float64)
    if values.ndim != 3 or values.shape[2] != 3 or axis not in (0, 1):
        raise ValueError("expected RGB image and horizontal/vertical axis")
    jumps = np.abs(np.diff(values, axis=axis)).mean(axis=2)
    if axis == 0:
        jumps = jumps[:, margin:-margin] if margin else jumps
        lines = jumps.mean(axis=1)
    else:
        jumps = jumps[margin:-margin, :] if margin else jumps
        lines = jumps.mean(axis=0)
    if not 1 <= position <= len(lines):
        raise ValueError("boundary outside image")
    index = position-1
    neighbours = [lines[i] for i in range(max(0, index-radius), min(len(lines), index+radius+1))
                  if abs(i-index) > 4]
    if not neighbours:
        raise ValueError("insufficient neighboring gradient samples")
    median, p95 = float(np.median(neighbours)), float(np.percentile(neighbours, 95))
    value = float(lines[index])
    return {"axis": "y" if axis == 0 else "x", "position": position, "mean_rgb_jump": value,
            "nearby_median_rgb_jump": median, "nearby_p95_rgb_jump": p95,
            "ratio_to_nearby_median": value/median if median else None,
            "excess_over_nearby_p95": max(0., value-p95)}


def review(staging, city):
    staging = candidates.safe_staging(staging)
    plan = json.loads((staging/"joined-plan.json").read_text())
    record = json.loads((staging/"inference-record.json").read_text())
    if record.get("plan_sha256") != upscale.file_digest(staging/"joined-plan.json"):
        raise ValueError("inference records do not pin this joined plan")
    name = f"garage_{city}.png"
    actual, job = record["records"][name], plan["jobs"][city]
    input_path, output_path = staging/"network-input"/name, staging/"network-output"/name
    if (upscale.file_digest(input_path) != job["network_input_sha256"]
            or actual["source_sha256"] != job["network_input_sha256"]
            or upscale.file_digest(output_path) != actual["output_sha256"]):
        raise ValueError("review inputs disagree with pinned source/output hashes")
    destination = staging/"boundary-review"/city
    if destination.exists():
        raise ValueError("review directory already exists; preserve the prior review")
    with Image.open(output_path) as image:
        scale, pad = job["scale"], job["pad"]
        expected = ((job["joined_size"][0]+2*pad)*scale, (job["joined_size"][1]+2*pad)*scale)
        if image.size != expected:
            raise ValueError("unexpected candidate dimensions")
        border = pad*scale
        candidate = image.convert("RGB").crop((border, border, border+job["joined_size"][0]*scale,
                                               border+job["joined_size"][1]*scale))
    with Image.open(staging/"joined-source"/name) as image:
        source = image.convert("RGB").resize(candidate.size, Image.Resampling.LANCZOS)
    panels = []
    for side in ("left", "right"):
        panel = job["panels"][side]
        path = upscale.TEXTURES/"replace"/panel["name"]
        if upscale.file_digest(path) != panel["base_replacement_sha256"]:
            raise ValueError("active baseline changed since preparation")
        panels.append(audit.pixels(path))
    base = Image.fromarray(joined.join_rgb(*panels, job["overlap_pixels"]*scale))
    tile = actual["tile"]
    xlines = [(value-pad)*scale for value in range(tile, expected[0]//scale, tile)
              if pad < value < pad+job["joined_size"][0]]
    ylines = [(value-pad)*scale for value in range(tile, expected[1]//scale, tile)
              if pad < value < pad+job["joined_size"][1]]
    result = {"format": "mc3-garage-joined-boundary-review-v1", "city": city,
              "plan_sha256": upscale.file_digest(staging/"joined-plan.json"),
              "actual_inference": actual, "tile_x_lines": xlines, "tile_y_lines": ylines,
              "metrics": {}, "active_replacements_modified": False,
              "limits": "Local gradient outliers guide visual review; natural scene edges can coincide with tile lines. No whole-image or cross-library bit-equivalence is established."}
    destination.mkdir(parents=True)
    for label, image in (("source_lanczos", source), ("active_hd", base), ("candidate_cpu", candidate)):
        image.save(destination/(label+".png"))
        result["metrics"][label] = [line_statistics(image, axis, line) for axis, lines in ((0, ylines), (1, xlines)) for line in lines]
    overlay = candidate.copy()
    draw = ImageDraw.Draw(overlay)
    for x in xlines:
        draw.line((x, 0, x, overlay.height-1), fill=(255, 40, 40), width=2)
    for y in ylines:
        draw.line((0, y, overlay.width-1, y), fill=(255, 40, 40), width=2)
    overlay.save(destination/"tile_positions.png")
    # Uniformly scaled comparison sheets keep the same actual six-column registration for all variants.
    sheet = Image.new("RGB", (candidate.width, candidate.height*3), "black")
    for row, image in enumerate((source, base, candidate)):
        sheet.paste(image, (0, row*candidate.height))
    sheet.save(destination/"source_active_candidate.png")
    audit.write_new(destination/"boundary-statistics.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staging", type=Path, required=True)
    parser.add_argument("--city", choices=list("adst"), required=True)
    args = parser.parse_args()
    result = review(args.staging, args.city)
    print(json.dumps({"city": args.city, "tile_x_lines": result["tile_x_lines"], "tile_y_lines": result["tile_y_lines"],
                      "candidate": result["metrics"]["candidate_cpu"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
