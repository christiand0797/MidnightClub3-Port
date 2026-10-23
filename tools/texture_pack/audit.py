#!/usr/bin/env python3
"""CPU-only coverage and integrity audit of the original-derived HD texture pack.

  py -3.12 tools/texture_pack/audit.py --output .local/texture-pass-audit.json

No model is executed and no replacement or PBR material is written. Source names shared by source/ and dump/
use source/ just like upscale.py. Saved vinyl-mask hashes remain excluded even without source_index.json.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

import upscale


def audit(textures, model, pass_id=None):
    found = upscale.source_paths(textures)
    excluded = upscale.excluded_masks(textures)
    replacements = textures / "replace"
    state_file = textures / "upscale_state.json"
    state = json.loads(state_file.read_text(encoding="utf-8")) if state_file.exists() else {}
    counts = Counter(known=len(found), excluded_masks=len(excluded))
    issues = {"source_errors": {}, "missing_or_invalid": [], "different_or_unrecorded_model": [],
              "active_masks": [], "orphan_replacements": [], "pass_context_mismatch": [],
              "missing_or_changed_pass_record": []}
    selected_pass = None
    if pass_id:
        pass_file = textures / "upscale_passes.json"
        passes = json.loads(pass_file.read_text(encoding="utf-8")) if pass_file.exists() else {}
        selected_pass = passes.get("passes", {}).get(pass_id)
        if not selected_pass or selected_pass.get("context") != upscale.pass_context(model):
            issues["pass_context_mismatch"].append(pass_id)
    widths = Counter()
    padding = Counter()
    for number, (name, source) in enumerate(sorted(found.items()), 1):
        if upscale.texture_name(name) in excluded:
            if (replacements / name).exists():
                issues["active_masks"].append(name)
            continue
        try:
            job, _pixels, reason = upscale.inspect_source(source)
            if job is None:
                counts["skip_" + reason] += 1
                continue
            counts["eligible"] += 1
            widths[f"{job['size'][0]}x{job['size'][1]} -> {job['size'][0] * job['scale']}x{job['size'][1] * job['scale']}"] += 1
            padding[f"x={'wrap' if job['wrap_x'] else 'reflect'},y={'wrap' if job['wrap_y'] else 'reflect'}"] += 1
            valid = upscale.replacement_valid(replacements / name, source)
            if not valid:
                issues["missing_or_invalid"].append(name)
            if state.get(name) != model:
                issues["different_or_unrecorded_model"].append(name)
            if valid and state.get(name) == model:
                counts["complete_selected_model"] += 1
            if pass_id:
                record = selected_pass.get("completed", {}).get(name) if selected_pass else None
                if upscale.pass_complete(record, source, replacements / name):
                    counts["complete_named_pass"] += 1
                else:
                    issues["missing_or_changed_pass_record"].append(name)
        except (OSError, ValueError, SyntaxError) as error:
            issues["source_errors"][name] = str(error)
        if number % 1000 == 0:
            print(f"audited {number}/{len(found)} originals", flush=True)
    for path in replacements.glob("*.png"):
        if path.name not in found:
            issues["orphan_replacements"].append(path.name)
    model_files = {}
    for suffix in (".param", ".bin"):
        path = upscale.MODELS[model] / (model + suffix)
        model_files[path.name] = {"bytes": path.stat().st_size, "sha256": upscale.file_digest(path)} if path.exists() else {"missing": True}
    complete = (counts["eligible"] == counts["complete_selected_model"] and not any(issues.values()))
    return {"version": 1, "model": model, "pass_id": pass_id, "model_files": model_files, "complete": complete,
            "counts": dict(counts), "padding": dict(padding), "dimensions": dict(widths), "issues": issues,
            "provenance_limit": "Existing state records model names, not historical input or model-file checksums. Model digests here identify current installed weights."}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--textures", type=Path, default=upscale.TEXTURES)
    parser.add_argument("--model", choices=sorted(upscale.MODELS), default=upscale.DEFAULT_MODEL)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--pass-id", help="also verify exact source/output/model/pipeline checksums for this fresh pass")
    args = parser.parse_args()
    result = audit(args.textures, args.model, args.pass_id)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        upscale.publish_json(result, args.output)
    print(json.dumps({key: result[key] for key in ("model", "complete", "counts", "padding")}, indent=2))
    for category, items in result["issues"].items():
        print(f"{category}: {len(items)}")
    return 0 if result["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
