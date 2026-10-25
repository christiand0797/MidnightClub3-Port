#!/usr/bin/env python3
"""Read-only original/loading replacement audit and candidate-pass manifest preparation.

All loading bundles are inventoried directly, rather than trusting the first-origin source index: common UI
textures also occur in other movies. This never overwrites originals, active replacements or their pass ledgers.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image

import extract_all
import upscale
import xbtex
import xiso

ROOT = Path(__file__).resolve().parents[2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def loading_bundle(path):
    name = Path(path).name.lower()
    return name.endswith(".xbck") and name.startswith(("loading_", "loadscreen_", "loadmovie"))


def independent_dxt(data, width, height, fmt):
    """Pillow's independent BCn/DDS decoder; this is not the project's extraction implementation."""
    fourcc = {0x0C: b"DXT1", 0x0E: b"DXT3", 0x0F: b"DXT5"}.get(fmt)
    if fourcc is None:
        return None
    blocks = max(1, (width+3)//4)*max(1, (height+3)//4)
    size = blocks*(8 if fmt == 0x0C else 16)
    if len(data) < size:
        raise ValueError("truncated compressed top mip")
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x81007, height, width, size, 0, 1)
    struct.pack_into("<II4s", header, 76, 32, 4, fourcc)
    struct.pack_into("<I", header, 108, 0x1000)
    with Image.open(io.BytesIO(bytes(header)+data[:size])) as image:
        return np.asarray(image.convert("RGBA"))


def pixels(path):
    with Image.open(path) as image:
        if image.format != "PNG":
            raise ValueError("not a PNG")
        image.verify()
    with Image.open(path) as image:
        return np.asarray(image.convert("RGBA"))


def classify(rgba, textures, name):
    job, _rgb, reason = upscale.inspect_source(textures/"source"/name)
    if name in upscale.excluded_masks(textures):
        return None, "excluded_categorical_mask"
    if reason:
        return None, reason
    # UI glyphs and alpha masks have a spatially constant color: their exact alpha carries the structure.
    # Keep the verified existing replacement instead of asking a color network to invent new mask edges.
    color_spread = np.std(rgba[..., :3].astype(np.float64), axis=(0, 1)).max()
    if color_spread < 2.0 and np.std(rgba[..., 3]) >= 2.0:
        return job, "structural_alpha_mask"
    return job, None


def audit(assets, textures, iso_path=None):
    assets, textures = Path(assets), Path(textures)
    bundles = sorted(p for p in (assets/"flash").glob("*.xbck") if loading_bundle(p))
    if not bundles:
        raise ValueError("no loading bundles found")
    iso = xiso.XIso(iso_path) if iso_path else None
    unique, source_files, issues = {}, [], {"disc_mismatch": [], "disc_inventory_mismatch": [],
        "source_missing": [], "source_pixel_mismatch": [],
        "source_decode_error": [], "independent_decode_error": [], "replacement_missing_invalid": [],
        "replacement_alpha_mismatch": [], "base_named_pass_mismatch": [], "base_context_mismatch": []}
    passes_path = textures/"upscale_passes.json"
    passes = json.loads(passes_path.read_text()) if passes_path.exists() else {}
    base_pass = passes.get("passes", {}).get("2026-10-22-originals-pass", {})
    base = base_pass.get("completed", {})
    if base_pass.get("context") != upscale.pass_context("ultrasharp-4x"):
        issues["base_context_mismatch"].append("2026-10-22-originals-pass")
    if iso:
        original_names = {path.lower() for path, _size in iso.walk("flash") if loading_bundle(path)}
        loose_names = {path.relative_to(assets).as_posix().lower() for path in bundles}
        issues["disc_inventory_mismatch"] = sorted(original_names ^ loose_names)
    for number, path in enumerate(bundles, 1):
        raw = path.read_bytes()
        relative = path.relative_to(assets).as_posix()
        source_files.append({"path": relative, "bytes": len(raw), "sha256": digest(raw)})
        if iso:
            original = iso.read(relative)
            if original != raw:
                issues["disc_mismatch"].append(relative)
        for texture in xbtex.xbck_textures(raw):
            data = raw[texture.offset:texture.offset+texture.size]
            for palette in texture.palettes if texture.fmt == xbtex.P8 else (None,):
                ph = xbtex.palette_hash(palette) if palette is not None else 0
                name = f"{xbtex.content_hash(texture, data, ph):016X}.png"
                origin = {"bundle": relative, "header": texture.header, "data": texture.offset,
                          "format": xbtex.FORMATS[texture.fmt][0], "palette_sha256": digest(palette) if palette else None}
                if name in unique:
                    unique[name]["origins"].append(origin)
                    continue
                item = {"name": name, "size": [texture.width, texture.height], "origins": [origin],
                        "encoded_texture_sha256": digest(data), "source_format": origin["format"]}
                unique[name] = item
                try:
                    rgba = extract_all.decode(texture, data, palette)
                    if rgba is None:
                        raise ValueError("unhandled original texture format")
                    item["decoded_rgba_sha256"] = digest(rgba.tobytes())
                    independent = independent_dxt(data, texture.width, texture.height, texture.fmt)
                    if independent is not None:
                        difference = np.abs(rgba.astype(np.int16)-independent.astype(np.int16))
                        item["independent_dxt_max_error_rgba"] = difference.max(axis=(0, 1)).tolist()
                        # BCn implementations differ in 565 expansion/interpolation rounding by small units.
                        if difference[..., :3].max() > 3 or difference[..., 3].max() > 1:
                            issues["independent_decode_error"].append(name)
                    source = textures/"source"/name
                    if not source.exists():
                        issues["source_missing"].append(name)
                        continue
                    source_rgba = pixels(source)
                    item["source_sha256"] = upscale.file_digest(source)
                    if source_rgba.shape != rgba.shape or not np.array_equal(source_rgba, rgba):
                        issues["source_pixel_mismatch"].append(name)
                        continue
                    job, skip = classify(rgba, textures, name)
                    item["skip"] = skip
                    if job:
                        item["candidate_scale"] = job["scale"]
                        item["old_heuristic_wrap"] = [job["wrap_x"], job["wrap_y"]]
                    replacement = textures/"replace"/name
                    item["base_replacement_sha256"] = upscale.file_digest(replacement) if replacement.exists() else None
                    if job and not upscale.replacement_valid(replacement, source):
                        issues["replacement_missing_invalid"].append(name)
                        continue
                    if job:
                        actual_alpha = pixels(replacement)[..., 3]
                        expected_alpha = np.asarray(Image.fromarray(rgba[..., 3]).resize(
                            (texture.width*job["scale"], texture.height*job["scale"]), Image.Resampling.LANCZOS))
                        item["replacement_alpha_sha256"] = digest(actual_alpha.tobytes())
                        if not np.array_equal(actual_alpha, expected_alpha):
                            issues["replacement_alpha_mismatch"].append(name)
                        if not upscale.pass_complete(base.get(name), source, replacement):
                            issues["base_named_pass_mismatch"].append(name)
                except (OSError, ValueError, SyntaxError) as error:
                    item["error"] = str(error)
                    issues["source_decode_error"].append(name)
        if number % 30 == 0:
            print(f"Audited {number}/{len(bundles)} loading bundles", flush=True)
    if iso:
        iso.f.close()
    counts = Counter(bundles=len(bundles), unique_textures=len(unique))
    for item in unique.values():
        counts["format_"+item["source_format"]] += 1
        counts["skip_"+item["skip"] if item.get("skip") else "candidate_images"] += 1
        if item.get("old_heuristic_wrap") and any(item["old_heuristic_wrap"]):
            counts["old_wrap_context"] += 1
    return {"format": "mc3-loading-art-audit-v1", "checked_at": datetime.now(timezone.utc).isoformat(),
            "audit_tool_sha256": upscale.file_digest(Path(__file__)), "base_pass_context": base_pass.get("context"),
            "original_disc_compared": bool(iso_path), "complete": not any(issues.values()),
            "counts": dict(counts), "issues": issues, "bundles": source_files, "textures": unique,
            "limits": "Clean source/replacement pixels do not establish correct native Flash sampling or prove the reported visible glitch's cause."}


def manifest(report, assets, textures):
    if report.get("complete") is not True:
        raise ValueError("loading audit has unresolved source/replacement issues")
    jobs = {}
    for name, item in sorted(report["textures"].items()):
        if item.get("skip"):
            continue
        jobs[name] = {"source": str((Path(textures)/"source"/name).resolve()), "source_sha256": item["source_sha256"],
                      "size": item["size"], "scale": item["candidate_scale"],
                      "base_replacement_sha256": item["base_replacement_sha256"],
                      "role": "garage_photo_panel" if item["size"] == [256, 256] and any(
                          Path(origin["bundle"]).name.lower().startswith("loadscreen_garage_") for origin in item["origins"])
                          else "loading_art", "old_heuristic_wrap": item["old_heuristic_wrap"]}
    return {"format": "mc3-loading-art-candidate-manifest-v1", "recipe": "loading-reflect-both-v1",
            "boundary_policy": "reflect-both-axes; no wrap; individual original textures; no unproved stitching",
            "audit_sha256": digest(json.dumps(report, sort_keys=True).encode()), "jobs": jobs,
            "skipped": {name: item["skip"] for name, item in report["textures"].items() if item.get("skip")},
            "active_replacements_modified": False, "base_named_pass_records_modified": False}


def write_new(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ROOT/"templates/game/assets")
    parser.add_argument("--textures", type=Path, default=upscale.TEXTURES)
    parser.add_argument("--iso", type=Path, default=xiso.default_iso())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    if args.output.exists() or (args.manifest and args.manifest.exists()):
        parser.error("report/manifest already exists; no file overwritten")
    report = audit(args.assets, args.textures, args.iso)
    write_new(args.output, report)
    if args.manifest and report["complete"]:
        write_new(args.manifest, manifest(report, args.assets, args.textures))
    print(json.dumps({"complete": report["complete"], "counts": report["counts"],
                      "issues": {key: len(value) for key, value in report["issues"].items()}}, indent=2))
    return 0 if report["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
