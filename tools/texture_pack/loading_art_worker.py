#!/usr/bin/env python3
"""Prepare or run an isolated non-tiling loading-art candidate pass.

Run only when the durable media coordinator has drained its existing GPU worker and no game is running.
This script never schedules itself, publishes active replacements, changes original pass records or enables PBR.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

import loading_art_audit
import upscale

ROOT = Path(__file__).resolve().parents[2]


def safe_staging(path, root=ROOT):
    path = Path(path).resolve()
    local = (Path(root)/".local").resolve()
    if path == local or local not in path.parents:
        raise ValueError("candidate staging must be a dedicated directory under this project's .local")
    for protected in (upscale.TEXTURES.resolve(), (Path(root)/"templates/game/assets").resolve()):
        if path == protected or protected in path.parents or path in protected.parents:
            raise ValueError("candidate staging overlaps active texture/original asset paths")
    return path


def prepare_source(source, expected_sha256, destination):
    if upscale.file_digest(source) != expected_sha256:
        raise ValueError(f"original PNG changed since audit: {source.name}")
    job, rgb, reason = upscale.inspect_source(source)
    if job is None:
        raise ValueError(f"audited candidate became ineligible: {reason}")
    job["wrap_x"] = job["wrap_y"] = False
    # Loading artwork is finite imagery. Opposite edges are not adjacent, even if the generic RGB-edge
    # heuristic happens to call two dark borders a seamless material.
    Image.fromarray(upscale.edge_pad(rgb, job["pad"], False, False)).save(destination)
    return job


def active_workers():
    sys.path.insert(0, str(ROOT/"tools/maintenance"))
    import overnight_pipeline
    return overnight_pipeline.active_workers()


@contextmanager
def exclusive_candidate_lock(path):
    # This worker's lock is shared across candidate staging directories; the coordinator still owns global
    # scheduling against texture/movie workers. A preflight inventory below also refuses an existing worker/game.
    import msvcrt
    with Path(path).open("a+b") as stream:
        stream.seek(0)
        if not stream.read(1):
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise RuntimeError("another loading-art candidate worker owns the lock") from error
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def context(manifest_path, model):
    return {"recipe": "loading-reflect-both-v1", "manifest_sha256": upscale.file_digest(manifest_path),
            "worker_sha256": upscale.file_digest(Path(__file__)), "network": upscale.pass_context(model),
            "active_replacements_modified": False, "base_named_pass_records_modified": False}


def stage(manifest_path, staging, model):
    staging = safe_staging(staging)
    manifest = json.loads(Path(manifest_path).read_text())
    if manifest.get("format") != "mc3-loading-art-candidate-manifest-v1" or manifest.get("recipe") != "loading-reflect-both-v1":
        raise ValueError("unsupported loading-art candidate manifest")
    jobs = manifest.get("jobs")
    if not isinstance(jobs, dict) or not jobs:
        raise ValueError("candidate manifest contains no jobs")
    for name, job in jobs.items():
        if (len(name) != 20 or not name.endswith(".png") or any(c not in "0123456789ABCDEF" for c in name[:16])
                or Path(job["source"]).resolve() != (upscale.TEXTURES/"source"/name).resolve()):
            raise ValueError("candidate source is not an audited original texture PNG")
    expected = context(manifest_path, model)
    staging.mkdir(parents=True, exist_ok=True)
    ledger_path = staging/"loading_candidate_pass.json"
    if ledger_path.exists():
        ledger = json.loads(ledger_path.read_text())
        if ledger.get("context") != expected:
            raise ValueError("candidate recipe/weights/manifest changed; preserve this stage and choose a new directory")
    else:
        ledger = {"format": "mc3-loading-art-candidate-pass-v1", "context": expected, "completed": {},
                  "phase": "prepared", "created_at": datetime.now(timezone.utc).isoformat()}
        upscale.publish_json(ledger, ledger_path)
    (staging/"replace").mkdir(exist_ok=True)
    return manifest, ledger, ledger_path


def stopped(stop_file):
    return any(path and Path(path).exists() for path in (stop_file, ROOT/".local/stop-overnight-pipeline",
               ROOT/".local/stop-texture-upscale", ROOT/".local/stop-loading-art"))


def run(manifest_path, staging, model, batch_size, stop_file=None):
    manifest, ledger, ledger_path = stage(manifest_path, staging, model)
    staging = safe_staging(staging)
    todo = []
    for name, job in sorted(manifest["jobs"].items()):
        source, destination = Path(job["source"]), staging/"replace"/name
        record = ledger["completed"].get(name)
        if not upscale.pass_complete(record, source, destination) or not upscale.replacement_valid(destination, source):
            todo.append((name, job))
    for offset in range(0, len(todo), batch_size):
        if stopped(stop_file):
            ledger["phase"] = "paused"
            upscale.publish_json(ledger, ledger_path)
            return 2
        blockers = active_workers()
        if blockers:
            raise RuntimeError("existing game or GPU/media worker; coordinator must drain it before candidate inference")
        batch = todo[offset:offset+batch_size]
        with tempfile.TemporaryDirectory(prefix="loading-batch-", dir=staging) as temporary:
            temporary = Path(temporary).resolve()
            if temporary.parent != staging or not temporary.name.startswith("loading-batch-"):
                raise ValueError("unexpected work directory")
            incoming, outgoing = temporary/"input", temporary/"output"
            incoming.mkdir(); outgoing.mkdir()
            prepared = {}
            for name, info in batch:
                prepared[name] = prepare_source(Path(info["source"]), info["source_sha256"], incoming/name)
                if list(prepared[name]["size"]) != info["size"] or prepared[name]["scale"] != info["scale"]:
                    raise ValueError("candidate dimensions changed since audit")
            command = [str(upscale.ESRGAN), "-i", str(incoming), "-o", str(outgoing), "-n", model,
                       "-s", "4", "-m", str(upscale.MODELS[model]), "-f", "png", "-j", "1:2:2"]
            ledger.update(phase="inference", command=command, batch=[name for name, _info in batch])
            upscale.publish_json(ledger, ledger_path)
            completed = subprocess.run(command, capture_output=True, text=True)
            if completed.returncode:
                raise RuntimeError("candidate network failed: "+completed.stderr[-1000:])
            for name, info in batch:
                destination = staging/"replace"/name
                upscale.finish(prepared[name], outgoing/name, destination)
                with Image.open(destination) as image:
                    alpha = np.asarray(image.convert("RGBA"))[..., 3]
                expected_alpha = np.asarray(Image.fromarray(prepared[name]["alpha"]).resize(
                    (info["size"][0]*info["scale"], info["size"][1]*info["scale"]), Image.Resampling.LANCZOS))
                if not np.array_equal(alpha, expected_alpha):
                    raise ValueError("candidate alpha differs from original-derived resize")
                ledger["completed"][name] = {"source_sha256": info["source_sha256"],
                    "output_sha256": upscale.file_digest(destination),
                    "base_replacement_sha256": info["base_replacement_sha256"],
                    "alpha_sha256": loading_art_audit.digest(alpha.tobytes()), "padding": "reflect-both"}
            ledger.update(phase="batch-complete", updated_at=datetime.now(timezone.utc).isoformat())
            upscale.publish_json(ledger, ledger_path)
        print(f"Staged loading candidates {min(offset+batch_size,len(todo))}/{len(todo)}", flush=True)
    ledger.update(phase="complete", completed_at=datetime.now(timezone.utc).isoformat())
    upscale.publish_json(ledger, ledger_path)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--staging", type=Path, required=True)
    parser.add_argument("--model", choices=sorted(upscale.MODELS), default=upscale.DEFAULT_MODEL)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--run", action="store_true", help="execute GPU inference; default prepares the isolated ledger only")
    args = parser.parse_args()
    if args.batch < 1:
        parser.error("batch must be positive")
    args.staging = safe_staging(args.staging)
    if not args.run:
        _manifest, ledger, path = stage(args.manifest, args.staging, args.model)
        print(json.dumps({"phase": ledger["phase"], "ledger": str(path), "gpu_started": False}))
        return 0
    if os.name != "nt":
        parser.error("GPU candidate execution requires the Windows media coordinator")
    with exclusive_candidate_lock(ROOT/".local/loading-art-candidate-worker.lock"):
        return run(args.manifest, args.staging, args.model, args.batch, args.stop_file)


if __name__ == "__main__":
    raise SystemExit(main())
