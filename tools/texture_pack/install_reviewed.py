#!/usr/bin/env python3
"""Build/check a reversible San Diego-only texture install plan; never install anything.

Future canonical PNG/archive/ledger paths are verified through explicit read-only projections into immutable
candidate files and NEW independent archive copies. There is deliberately no apply/install operation.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from PIL import Image

import reviewed_fixes as fixes
import upscale

ROOT = Path(__file__).resolve().parents[2]
FORMAT = "mc3-reviewed-texture-install-plan-v1"
CITY = "s"


def within(root, path):
    root, path = Path(root).resolve(), Path(path).resolve()
    if root not in path.parents:
        raise ValueError("path escapes the project root")
    return path


def rel(root, path):
    return within(root, path).relative_to(Path(root).resolve()).as_posix()


def new_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def independent_copy(source, destination, expected):
    """Read a possibly hardlinked old PNG; create a new inode without opening any existing target for writes."""
    if upscale.file_digest(source) != expected:
        raise ValueError("archive source differs from its exact old anchor")
    destination = Path(destination)
    with Path(source).open("rb") as incoming, destination.open("xb") as outgoing:
        shutil.copyfileobj(incoming, outgoing)
        outgoing.flush()
        os.fsync(outgoing.fileno())
    if (upscale.file_digest(source) != expected or upscale.file_digest(destination) != expected
            or destination.stat().st_nlink != 1 or os.path.samefile(source, destination)):
        raise ValueError("independent base archive copy failed verification")


def installation_workers():
    """Installer readiness is stricter than GPU scheduling: a live coordinator also blocks installation."""
    if os.name != "nt":
        raise RuntimeError("installation readiness requires the Windows process inventory")
    query = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -like '*python*' -or $_.Name -in "
             "@('mc3.exe','FortniteClient-Win64-Shipping.exe','BlackOps3.exe','realesrgan-ncnn-vulkan.exe') } | "
             "Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress")
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", query],
                            capture_output=True, text=True, check=True)
    rows = json.loads(result.stdout) if result.stdout.strip() else []
    rows = rows if isinstance(rows, list) else [rows]
    terms = ("overnight_pipeline.py", "upscale_movies.py", "tools/texture_pack/upscale.py",
             "loading_art_worker.py", "loading_garage_join.py", "cpu_x4plus.py")
    return [row for row in rows if row["ProcessId"] != os.getpid() and
            (row["Name"].lower() in {"mc3.exe", "fortniteclient-win64-shipping.exe", "blackops3.exe",
                                   "realesrgan-ncnn-vulkan.exe"}
             or any(term in (row.get("CommandLine") or "").lower().replace("\\", "/") for term in terms))]


class ProjectedFiles(fixes.Files):
    """Resolve only explicitly named future current/archive paths; every other dependency remains real."""
    def __init__(self, root, dependencies, bindings):
        self.bindings = bindings
        super().__init__(root, dependencies)

    def checked(self, value, expected=None):
        if value not in self.bindings:
            return super().checked(value, expected)
        self.path(value)  # Check the future path for link escapes as well as the staged source.
        actual = self.path(self.bindings[value])
        pinned = self.dependencies.get(value)
        if (not isinstance(pinned, str) or not fixes.SHA.fullmatch(pinned)
                or self.dependencies.get(self.bindings[value]) != pinned
                or (expected is not None and expected != pinned)
                or not actual.is_file() or upscale.file_digest(actual) != pinned):
            raise ValueError("projected future dependency does not match its pinned staged bytes")
        return actual


def native_review(files, proof, names, entries):
    if proof.get("decision") != "accepted":
        return False, "native before/after capture evidence is pending"
    native_path = proof.get("native_review_proof")
    if not native_path:
        raise ValueError("accepted review has no pinned native review proof")
    native = files.json(native_path)
    if (native.get("format") != "mc3-native-texture-review-v1" or native.get("decision") != "accepted"
            or not isinstance(native.get("reviewer"), str) or not native["reviewer"].strip()
            or set(native.get("entries", {})) != set(names)):
        raise ValueError("native review must explicitly accept exactly the two San Diego panels")
    before, after = native.get("before_capture"), native.get("after_capture")
    if not before or not after or before == after:
        raise ValueError("native review needs distinct before and after capture artifacts")
    for capture in (before, after):
        path = files.checked(capture)
        with Image.open(path) as image:
            image.load()
            if min(image.size) < 32:
                raise ValueError("native capture is not a readable game image")
    for name in names:
        if any(native["entries"][name].get(key) != entries[name][key]
               for key in ("before_sha256", "after_sha256", "source_sha256")):
            raise ValueError("native review does not pin the exact source/before/after panel bytes")
    return True, None


def validate_plan(plan_path, *, root=ROOT, workers=None):
    """Read-only readiness check. A passing result is a plan review, not installation or a live-pack audit."""
    root = Path(root).resolve()
    plan_path = within(root, plan_path)
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    if (plan.get("format") != FORMAT or plan.get("city") != CITY or plan.get("operation") != "dry-run-only"
            or plan.get("canonical_files_modified") is not False
            or plan.get("planner_sha256") != upscale.file_digest(Path(__file__))):
        raise ValueError("unsupported/changed install planner or operation")
    ledger_path = within(root, root/fixes.relative_path(plan["proposed_ledger"]))
    if upscale.file_digest(ledger_path) != plan["proposed_ledger_sha256"]:
        raise ValueError("proposed ledger changed after planning")
    ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
    textures = within(root, root/fixes.relative_path(plan["textures"]))
    if (textures/"reviewed_fixes.json").exists():
        raise ValueError("a canonical reviewed ledger appeared; the first-adoption plan is stale")
    entries, bindings = ledger["entries"], plan["future_bindings"]
    if len(entries) != 2 or len(bindings) != 4 or len(plan["operations"]) != 2:
        raise ValueError("plan must contain exactly two PNGs and two independent old archives")
    expected_bindings = {}
    for operation in plan["operations"]:
        name = operation["name"]
        if name not in entries or not fixes.NAME.fullmatch(name):
            raise ValueError("unknown planned PNG")
        entry = entries[name]
        current = plan["textures"]+"/replace/"+name
        archive = entry["base_archive"]
        if (operation["target"] != current or operation["archive_target"] != archive
                or operation["expected_before_sha256"] != entry["before_sha256"]
                or operation["expected_after_sha256"] != entry["after_sha256"]):
            raise ValueError("install/rollback anchors disagree with proposed ledger")
        current_path = within(root, root/fixes.relative_path(current))
        if upscale.file_digest(current_path) != entry["before_sha256"]:
            raise ValueError("active PNG changed; preserve it and build a fresh plan")
        archive_path = within(root, root/fixes.relative_path(archive))
        if archive_path.exists():
            raise ValueError("future archive path already exists; never overwrite another archive")
        staged_archive = within(root, root/fixes.relative_path(operation["archive_source"]))
        if staged_archive.stat().st_nlink != 1 or os.path.samefile(current_path, staged_archive):
            raise ValueError("old archive must be an independent copy, not a live/QA hardlink")
        expected_bindings[current] = operation["candidate_source"]
        expected_bindings[archive] = operation["archive_source"]
    if bindings != expected_bindings:
        raise ValueError("future projection contains unexpected path bindings")
    files = ProjectedFiles(root, ledger["dependencies"], bindings)
    passes = files.json(plan["textures"]+"/upscale_passes.json")
    selected = passes["passes"][ledger["base_pass_id"]]
    context = upscale.pass_context(ledger["base_model"])
    if (ledger.get("format") != fixes.FORMAT or selected.get("context") != context
            or ledger.get("base_context") != context):
        raise ValueError("historical source pass/model context changed")
    found, excluded, cache = upscale.source_paths(textures), upscale.excluded_masks(textures), {}
    names = set(entries)
    for name, entry in entries.items():
        candidate = fixes.garage_candidate(files, entry["candidate"]["stage"], name, plan["textures"], cache)
        if candidate["record"]["city"] != CITY:
            raise ValueError("only San Diego has native adoption scope in this first plan")
        source = files.checked(plan["textures"]+"/source/"+name, entry["source_sha256"])
        archive = files.checked(entry["base_archive"], entry["before_sha256"])
        if not upscale.pass_complete(selected["completed"].get(name), source, archive):
            raise ValueError("independent archive no longer matches the original named pass")
    proofs = {entry["review_proof"] for entry in entries.values()}
    if len(proofs) != 1:
        raise ValueError("San Diego pair must have one explicit paired review")
    proof = files.json(next(iter(proofs)))
    reviewed, pending = native_review(files, proof, names, entries)
    if reviewed:
        for name, entry in entries.items():
            fixes.verify_entry(files, textures, ledger["base_model"], selected, name, entry, excluded, found, cache)
    files.verify_all()
    base = fixes.original_audit.audit(textures, ledger["base_model"], ledger["base_pass_id"])
    if base.get("complete") is not True:
        raise ValueError("the current complete original pack must pass before a first correction can be planned")
    live = installation_workers() if workers is None else workers
    return {"format": "mc3-reviewed-texture-install-readiness-v1", "candidate_chain_complete": True,
            "base_pack_complete": True, "base_pack_counts": base["counts"],
            "native_review_accepted": reviewed, "native_review_pending_reason": pending,
            "blocking_workers": live, "install_ready": reviewed and not live,
            "canonical_files_modified": False, "installed": False,
            "limit": "Read-only projected validation; no atomic multi-file transaction has been performed or proven."}


def build_plan(staging, candidate_stage, base_source_commit, *, native_proof=None, root=ROOT,
               textures=None, model="ultrasharp-4x", pass_id="2026-10-22-originals-pass", workers=None):
    root = Path(root).resolve()
    textures = within(root, textures or upscale.TEXTURES)
    staging, candidate_stage = within(root, staging), within(root, candidate_stage)
    local = root/".local"
    if (local not in staging.parents or staging.exists() or local not in candidate_stage.parents
            or textures in staging.parents or staging in textures.parents):
        raise ValueError("proposal must use a NEW isolated .local directory")
    if not isinstance(base_source_commit, str) or not re.fullmatch(r"[0-9a-f]{40}", base_source_commit):
        raise ValueError("base integration source commit must be a complete Git object hash")
    if (textures/"reviewed_fixes.json").exists():
        raise ValueError("only a first-adoption plan with no canonical reviewed ledger is supported")
    plan_source = json.loads((candidate_stage/"joined-plan.json").read_text())
    names = [plan_source["jobs"][CITY]["panels"][side]["name"] for side in ("left", "right")]
    selected = json.loads((textures/"upscale_passes.json").read_text())["passes"][pass_id]
    context = upscale.pass_context(model)
    if selected.get("context") != context:
        raise ValueError("original named-pass context differs from current model/pipeline files")
    staging.mkdir()
    (staging/"base-archive").mkdir()
    dependencies, bindings, entries, operations = {}, {}, {}, []

    def pin(path):
        path = within(root, path)
        dependencies[rel(root, path)] = upscale.file_digest(path)
        return rel(root, path)

    textures_rel, stage_rel = rel(root, textures), rel(root, candidate_stage)
    for filename in ("upscale_passes.json", "upscale_state.json", "source_index.json", "vinyl_hashes.txt"):
        if (textures/filename).exists(): pin(textures/filename)
    for path in (Path(upscale.__file__), Path(fixes.__file__), Path(__file__), Path(fixes.cpu_x4plus.__file__), fixes.cpu_x4plus.DEFAULT_WEIGHTS):
        pin(path)
    for suffix in context["models"]: pin(upscale.MODELS[model]/(model+suffix))
    pin(root/"tools/texture_pack/loading_garage_join.py")
    for filename in ("joined-plan.json", "inference-record.json", "neural-candidate-records.json", "candidate-integrity.json"):
        if (candidate_stage/filename).exists(): pin(candidate_stage/filename)
    # Pin the complete existing four-image inference graph, although only the San Diego pair is proposed.
    for city, job in plan_source["jobs"].items():
        pin(Path(job["bundle"]))
        for side in ("left", "right"):
            name = job["panels"][side]["name"]
            pin(textures/"source"/name)
            pin(candidate_stage/"neural-candidates"/name)
        for folder in ("network-input", "network-output", "joined-source"):
            pin(candidate_stage/folder/f"garage_{city}.png")
    comparison = pin(candidate_stage/"boundary-review/s/source_active_candidate.png")
    for filename in ("source_lanczos.png", "active_hd.png", "candidate_cpu.png", "tile_positions.png", "boundary-statistics.json"):
        path = candidate_stage/"boundary-review/s"/filename
        if path.exists(): pin(path)
    note = root/"docs/notes/loading_art_audit.md"
    if note.exists():
        independent_copy(note, staging/"loading-review-note.md", upscale.file_digest(note))
        pin(staging/"loading-review-note.md")
    for side, name in zip(("left", "right"), names):
        panel = plan_source["jobs"][CITY]["panels"][side]
        before, after = panel["base_replacement_sha256"], upscale.file_digest(candidate_stage/"neural-candidates"/name)
        current = textures/"replace"/name
        if not upscale.pass_complete(selected["completed"].get(name), textures/"source"/name, current):
            raise ValueError("active old PNG/source does not match its original named-pass anchor")
        independent_copy(current, staging/"base-archive"/name, before)
        archive_source = pin(staging/"base-archive"/name)
        current_rel = textures_rel+"/replace/"+name
        archive_target = textures_rel+"/reviewed_archive/"+staging.name+"/"+name
        dependencies[current_rel], dependencies[archive_target] = after, before
        candidate_source = stage_rel+"/neural-candidates/"+name
        bindings[current_rel], bindings[archive_target] = candidate_source, archive_source
        entries[name] = {"source_sha256": panel["source_sha256"], "before_sha256": before, "after_sha256": after,
                         "base_archive": archive_target, "review_proof": rel(root, staging/"review.json"),
                         "candidate": {"kind": fixes.RECIPE, "stage": stage_rel}}
        operations.append({"name": name, "target": current_rel, "candidate_source": candidate_source,
                           "archive_target": archive_target, "archive_source": archive_source,
                           "expected_before_sha256": before, "expected_after_sha256": after,
                           "rollback_condition": "Restore archived old bytes only if target still equals expected_after_sha256; preserve any unrelated replacement."})
    review = {"format": "mc3-texture-review-v1", "decision": "pending", "reviewer": "pending authenticated parent native evidence",
              "comparison": comparison, "entries": {name: {key: entry[key] for key in
                  ("before_sha256", "after_sha256", "source_sha256")} for name, entry in entries.items()},
              "limit": "Observed native review in chat is documented; persisted accepted before/after capture evidence is not fabricated."}
    if native_proof:
        native_path = pin(native_proof)
        native = json.loads(Path(native_proof).read_text())
        for key in ("before_capture", "after_capture"): pin(root/fixes.relative_path(native[key]))
        review.update(decision=native.get("decision"), reviewer=native.get("reviewer"), native_review_proof=native_path)
    new_json(staging/"review.json", review)
    pin(staging/"review.json")
    ledger = {"format": fixes.FORMAT, "base_model": model, "base_pass_id": pass_id, "base_context": context,
              "dependencies": dependencies, "entries": entries}
    new_json(staging/"proposed-reviewed-fixes.json", ledger)
    plan = {"format": FORMAT, "operation": "dry-run-only", "city": CITY, "base_source_commit": base_source_commit,
            "planner_sha256": upscale.file_digest(Path(__file__)), "textures": textures_rel,
            "proposed_ledger": rel(root, staging/"proposed-reviewed-fixes.json"),
            "proposed_ledger_sha256": upscale.file_digest(staging/"proposed-reviewed-fixes.json"),
            "future_bindings": bindings, "operations": operations, "canonical_files_modified": False,
            "requires_before_any_install": ["No game, texture, movie, inference or coordinator worker is live; acquire coordinator and install exclusion locks.",
                "Revalidate all dependency hashes and exact old anchors immediately before mutation; every archive destination must remain absent.",
                "Create independent verified base archives and same-directory candidate copies, then replace directory entries atomically; never open a live/hardlinked PNG for writes.",
                "Publish the exact reviewed ledger last, audit the full mixed pack, and rollback only unchanged newly installed targets on failure.",
                "Preserve unexpected external changes and report incomplete rollback instead of overwriting them; keep all proofs and archived old bytes."],
            "limit": "This tool deliberately has no install operation; multi-file transaction/recovery must be implemented and independently tested before applying this plan."}
    new_json(staging/"install-plan.json", plan)
    readiness = validate_plan(staging/"install-plan.json", root=root, workers=workers)
    new_json(staging/"readiness.json", readiness)
    return plan, readiness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staging", type=Path)
    parser.add_argument("--candidate-stage", type=Path)
    parser.add_argument("--base-source-commit")
    parser.add_argument("--native-proof", type=Path)
    parser.add_argument("--check", type=Path, help="Read-only revalidation of an existing install-plan.json")
    args = parser.parse_args()
    if args.check:
        result = validate_plan(args.check)
    else:
        if not all((args.staging, args.candidate_stage, args.base_source_commit)):
            parser.error("building requires NEW staging, existing candidate-stage, and full base-source-commit")
        _plan, result = build_plan(args.staging, args.candidate_stage, args.base_source_commit, native_proof=args.native_proof)
    print(json.dumps(result, indent=2, allow_nan=False))
    return 2 if args.check and not result["install_ready"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
