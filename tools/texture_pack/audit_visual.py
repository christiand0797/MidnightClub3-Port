#!/usr/bin/env python3
"""CPU-only pack-wide visual triage and review sheets; never edits active textures.

Metrics flag review candidates, not defects. Thumbnail inspection cannot prove every
pixel, UV seam, material role, or in-game appearance. Sources, replacements, and
production completion records are read only. Review outputs go to a separate folder.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import upscale


ROOT = Path(__file__).resolve().parents[2]


def category(origin):
    value = origin.lower().replace("\\", "/")
    if any(word in value for word in ("loadscreen", "loading_", "loadmovie")):
        return "01_loading"
    if any(word in value for word in ("/ped", "/rider", "cutscene", "/char")):
        return "02_people"
    if "flash/" in value or "/font" in value:
        return "03_ui"
    if any(word in value for word in ("/vehicle", "/car", "/rim", "/tire", "/wheel")):
        return "04_vehicle"
    return "05_world"


def edge_jump(rgb, axis):
    a = np.asarray(rgb, dtype=np.float32)
    if axis == 0:
        return float(np.abs(a[0] - a[-1]).mean())
    return float(np.abs(a[:, 0] - a[:, -1]).mean())


def inspect_pair(textures, name, origin):
    source_path = textures / "source" / name
    if not source_path.exists():
        source_path = textures / "dump" / name
    result = {"name": name, "origin": origin, "category": category(origin)}
    try:
        with Image.open(source_path) as opened:
            source = opened.convert("RGBA")
        with Image.open(textures / "replace" / name) as opened:
            opened.verify()
        with Image.open(textures / "replace" / name) as opened:
            output = opened.convert("RGBA")
        s = np.asarray(source, dtype=np.float32)
        expected_alpha = source.getchannel("A").resize(output.size, Image.Resampling.LANCZOS)
        alpha_delta = np.abs(np.asarray(output.getchannel("A"), dtype=np.int16) - np.asarray(expected_alpha, dtype=np.int16))
        # RGB is compared after downsampling to avoid treating legitimate added detail
        # as a whole-image layout/color error. Hide fully transparent source pixels.
        back = np.asarray(output.convert("RGB").resize(source.size, Image.Resampling.LANCZOS), dtype=np.float32)
        valid = s[..., 3] >= 128
        delta = back - s[..., :3]
        if not valid.any():
            valid = s[..., 3] >= 1
        if not valid.any():
            valid = np.ones(s.shape[:2], dtype=bool)
        per_pixel = np.sqrt(np.mean(delta * delta, axis=2))
        baseline = source.convert("RGB").resize(output.size, Image.Resampling.LANCZOS)
        result.update({"source_size": list(source.size), "output_size": list(output.size),
            "alpha_max_error": int(alpha_delta.max()), "alpha_changed_pixels": int(np.count_nonzero(alpha_delta)),
            "rgb_roundtrip_rmse": round(float(np.sqrt(np.mean(delta[valid] ** 2))), 4),
            "rgb_roundtrip_p95": round(float(np.percentile(per_pixel[valid], 95)), 4),
            "rgb_roundtrip_bias": [round(float(x), 4) for x in delta[valid].mean(axis=0)],
            "output_edge_excess_x": round(edge_jump(output.convert("RGB"), 1) - edge_jump(baseline, 1), 4),
            "output_edge_excess_y": round(edge_jump(output.convert("RGB"), 0) - edge_jump(baseline, 0), 4),
            "visible_fraction": round(float(valid.mean()), 4)})
    except (OSError, ValueError, SyntaxError) as error:
        result["error"] = str(error)
    return result


def checker(size):
    y, x = np.indices((size[1], size[0]))
    c = (92 + 32 * ((x // 12 + y // 12) % 2)).astype(np.uint8)
    return Image.fromarray(np.dstack((c, c, c)), "RGB").convert("RGBA")


def thumbnail(path, size, enlarge=False):
    with Image.open(path) as opened:
        thumb = opened.convert("RGBA")
    if enlarge:
        scale = min(size[0] / thumb.width, size[1] / thumb.height)
        thumb = thumb.resize((max(1, round(thumb.width * scale)), max(1, round(thumb.height * scale))), Image.Resampling.LANCZOS)
    else:
        thumb.thumbnail(size, Image.Resampling.LANCZOS)
    tile = checker(size)
    tile.alpha_composite(thumb, ((size[0] - thumb.width) // 2, (size[1] - thumb.height) // 2))
    return tile.convert("RGB")


def font(size):
    try:
        return ImageFont.truetype("C:/Windows/Fonts/arial.ttf", size)
    except OSError:
        return ImageFont.load_default()


def make_sheets(textures, records, destination, columns=10, rows=10, source_mode=False):
    destination.mkdir(parents=True, exist_ok=True)
    cell_w, cell_h, top = 164, 190, 46
    manifest = []
    for first in range(0, len(records), columns * rows):
        subset = records[first:first + columns * rows]
        sheet = Image.new("RGB", (columns * cell_w, rows * cell_h + top), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        number = len(manifest) + 1
        title = "UNREPLACED ORIGINALS" if source_mode else "ACTIVE HD TEXTURES"
        draw.text((6, 5), f"{title} | sheet {number:03} | items {first+1}-{first+len(subset)} / {len(records)}", fill="white", font=font(19))
        draw.text((6, 28), "Thumbnail triage only; checker = alpha. Labels use exact hash + source dimensions.", fill=(190, 190, 190), font=font(12))
        names = []
        for offset, record in enumerate(subset):
            x = offset % columns * cell_w
            y = top + offset // columns * cell_h
            image_path = Path(record["source_path"]) if source_mode else textures / "replace" / record["name"]
            sheet.paste(thumbnail(image_path, (160, 154)), (x + 2, y + 2))
            draw.text((x + 2, y + 156), record["name"][:-4], fill="white", font=font(11))
            w, h = record.get("source_size", [0, 0])
            draw.text((x + 2, y + 171), f"{first+offset+1} | {w}x{h} | {record['category'][3:]}", fill=(190, 190, 190), font=font(11))
            names.append(record["name"])
        path = destination / f"pack_{number:03}.jpg"
        sheet.save(path, quality=92)
        manifest.append({"sheet": path.name, "first": first + 1, "last": first + len(subset), "names": names})
        print(f"sheet {number}: {len(subset)} textures", flush=True)
    return manifest


def inspect_original(name, source, origin, excluded):
    record = {"name": name, "origin": origin, "source_path": str(source.resolve())}
    try:
        with Image.open(source) as opened:
            opened.verify()
        with Image.open(source) as opened:
            rgba = opened.convert("RGBA")
        a = np.asarray(rgba)
        _job, _pixels, reason = upscale.inspect_source(source)
        record.update({"source_size": list(rgba.size), "alpha_min": int(a[..., 3].min()),
                       "alpha_max": int(a[..., 3].max()), "skip_reason": reason})
        if upscale.texture_name(name) in excluded:
            record["category"] = "01_mask"
            record["skip_reason"] = "categorical_vinyl_mask"
        else:
            record["category"] = "02_" + (reason or "unexplained")
    except (OSError, ValueError, SyntaxError) as error:
        record.update({"error": str(error), "category": "00_error"})
    return record


def make_comparison_sheets(textures, records, destination, threshold=20):
    """Enlarge color/layout outliers beside their originals; no automated verdict."""
    destination.mkdir(parents=True, exist_ok=True)
    candidates = sorted((record for record in records
                         if record.get("rgb_roundtrip_rmse", 0) > threshold),
                        key=lambda record: record["rgb_roundtrip_rmse"], reverse=True)
    manifest = []
    for first in range(0, len(candidates), 8):
        subset = candidates[first:first + 8]
        canvas = Image.new("RGB", (840, 1000), (24, 24, 24))
        draw = ImageDraw.Draw(canvas)
        draw.text((5, 5), "RGB OUTLIER TRIAGE | source left / active HD right", fill="white", font=font(18))
        draw.text((5, 30), "High RMSE is not a defect verdict. Both views are fit into 200px and composited over checker.", fill="white", font=font(11))
        for offset, record in enumerate(subset):
            x = offset % 2 * 420
            y = 52 + offset // 2 * 235
            name = record["name"]
            source = textures / "source" / name
            if not source.exists():
                source = textures / "dump" / name
            canvas.paste(thumbnail(source, (200, 200), enlarge=True), (x + 4, y))
            canvas.paste(thumbnail(textures / "replace" / name, (200, 200), enlarge=True), (x + 208, y))
            draw.text((x + 4, y + 201), f"{name[:-4]} | RMSE {record['rgb_roundtrip_rmse']:.2f}", fill="white", font=font(12))
            draw.text((x + 4, y + 217), record["origin"].replace("\\", "/")[-57:], fill=(190, 190, 190), font=font(10))
        path = destination / f"outliers_{len(manifest)+1:03}.jpg"
        canvas.save(path, quality=94)
        manifest.append({"sheet": path.name, "names": [record["name"] for record in subset]})
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--textures", type=Path, default=ROOT / "templates/game/out/native/textures")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--sheets-only", action="store_true")
    parser.add_argument("--comparisons-only", action="store_true")
    parser.add_argument("--outlier-threshold", type=float, default=20)
    parser.add_argument("--inactive", action="store_true", help="separate original source/dump union without an active replacement")
    args = parser.parse_args()
    if args.output is None:
        args.output = ROOT / ".local/texture-visual-review-20261024"
        if args.inactive:
            args.output /= "inactive"
    if args.output.resolve().is_relative_to(args.textures.resolve()):
        parser.error("review output must be outside the read-only production texture folder")
    args.output.mkdir(parents=True, exist_ok=True)
    report_path = args.output / "metrics.json"
    if args.sheets_only or args.comparisons_only:
        report = json.loads(report_path.read_text())
        if bool(report.get("source_mode", False)) != args.inactive:
            parser.error("saved report mode differs; select the matching --inactive mode/output")
    else:
        index = json.loads((args.textures / "source_index.json").read_text())
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            if args.inactive:
                found = upscale.source_paths(args.textures)
                excluded = upscale.excluded_masks(args.textures)
                names = sorted(name for name in found if not (args.textures / "replace" / name).exists())
                results = list(pool.map(lambda name: inspect_original(name, found[name], index.get(name[:-4], ""), excluded), names))
            else:
                names = sorted(path.name for path in (args.textures / "replace").glob("*.png"))
                results = list(pool.map(lambda name: inspect_pair(args.textures, name, index.get(name[:-4], "")), names))
        records = sorted(results, key=lambda result: (result["category"], result["origin"], result["name"]))
        report = {"format": "mc3-texture-visual-triage-v1", "checked_at": datetime.now(timezone.utc).isoformat(),
            "count": len(records), "source_mode": args.inactive, "records": records,
            "limitations": "Read-only metrics and thumbnail triage. Large metrics are candidates, not proof of a defect; no in-game or full-pixel certification."}
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    if not args.comparisons_only:
        manifest = make_sheets(args.textures, report["records"], args.output / "sheets", source_mode=report.get("source_mode", False))
        (args.output / "sheets.json").write_text(json.dumps(manifest, indent=2) + "\n")
    comparisons = make_comparison_sheets(args.textures, report["records"], args.output / "outliers", args.outlier_threshold)
    (args.output / "outliers.json").write_text(json.dumps(comparisons, indent=2) + "\n")
    print(json.dumps({"textures": report["count"], "outlier_sheets": len(comparisons), "output": str(args.output)}))


if __name__ == "__main__":
    main()
