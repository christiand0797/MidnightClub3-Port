"""Read-only frozen-content validation; default is a plan, --execute writes only a NEW external report directory.

No inference, adoption, retiming, metadata repair, process control or live media access occurs. Exact source timing
and recorded model provenance are separate from successful software decoding of historical movies.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from fractions import Fraction
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile

from PIL import Image

import freeze_content as freeze
import package_reviewed_proof as proof_api

ROOT = Path(__file__).absolute().parents[2]
PROOF_SHA = "143b02627bd57b5b81e63f3b1c7d00b5d39651771e336e644368e5efef1ae75e"
ARCHIVE_SHA = "21105deacc97ad2eb49b8d165c43c20d3b3a689df053a9c9a6591361bbf919bc"
LEDGER_SHA = "f8ed40b44fbdeabfb4586e630e795a6481fa63e28905293b7d51e6fcce08d9cc"
PASS_ID = "2026-10-22-originals-pass"
TEXTURES = "templates/game/out/native/textures/"
CLASSES = {"strict-original-record", "historical-marker-only", "historical-unrecorded", "historical-json-unverified"}


class ValidationError(RuntimeError):
    pass


def need(condition, message):
    if not condition:
        raise ValidationError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, "Duplicate JSON key")
        result[key] = value
    return result


def document(data):
    return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_object)


def joined(root, relative):
    return root / freeze.safe_relative(relative)


def digest(path):
    path = freeze.absolute(path)
    freeze.plain(path, directory=False)
    before = path.stat()
    need(before.st_nlink == 1, "Validation input must be an independent regular file")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        need((opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns) ==
             (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns), "Input changed while opening")
        value = hashlib.file_digest(stream, "sha256").hexdigest()
        after_fd = os.fstat(stream.fileno())
    after = path.stat()
    need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_nlink) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_nlink) and
         (opened.st_size, opened.st_mtime_ns) == (after_fd.st_size, after_fd.st_mtime_ns), "Input changed during validation")
    freeze.plain(path, directory=False)
    return value


def read_json(path, expected=None):
    first = digest(path)
    need(expected is None or first == expected, "JSON trust-anchor mismatch: " + str(path))
    data = path.read_bytes()
    need(hashlib.sha256(data).hexdigest() == first, "JSON changed while reading")
    return document(data)


def png_info(source, label):
    """Verify PNG chunks, then reopen and fully decode pixels; never save or normalize input bytes."""
    def opened():
        return Image.open(io.BytesIO(source)) if isinstance(source, bytes) else Image.open(source)
    try:
        with opened() as image:
            need(image.format == "PNG" and 0 < image.width <= 2048 and 0 < image.height <= 2048,
                 "PNG format/dimensions outside original-pass bounds: " + label)
            size, mode = image.size, image.mode
            image.verify()
        with opened() as image:
            need(image.format == "PNG" and image.size == size and image.mode == mode, "PNG header changed: " + label)
            image.load()
        return {"width": size[0], "height": size[1], "mode": mode}
    except (OSError, SyntaxError, ValueError, Image.DecompressionBombError) as error:
        raise ValidationError("PNG integrity failure: " + label + ": " + str(error)) from error


def video_api():
    """Load a private helper module; replacing its subprocess facade never changes a live worker/module."""
    path = ROOT / "tools/video/upscale_movies.py"
    spec = importlib.util.spec_from_file_location("frozen_video_readonly_api", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    probe_path = Path(module.binary(module.ffmpeg_bin(), "ffprobe")).resolve(strict=True)
    ff, probe = probe_path.parent, str(probe_path)

    class SoftwareProbe:
        @staticmethod
        def run(command, **kwargs):
            need(command[0] == probe and "-show_entries" in command, "Only software ffprobe queries are allowed")
            return subprocess.run([command[0], "-threads", "2", *command[1:]], **kwargs)

    module.subprocess = SoftwareProbe
    return module, ff, {"ffprobe": str(probe), "ffprobe_sha256": digest(Path(probe)), "decoder_threads": 2}


@dataclass
class Config:
    snapshot: Path
    snapshot_sha256: str | None
    proof_manifest: Path
    report: Path
    proof_manifest_sha256: str = PROOF_SHA
    proof_archive_sha256: str = ARCHIVE_SHA
    ledger_sha256: str = LEDGER_SHA
    textures: int = 7081
    movies: int = 106
    reviewed: int = 2
    iso_sha256: str = freeze.ISO_SHA256
    external_g: bool = True  # Tests use tiny private fixtures; CLI has no policy override.


def check_inventory(config, manifest, event):
    need(manifest.get("format") == "mc3-frozen-content-cohort-v1" and manifest.get("status") == "complete", "Snapshot is not complete")
    need(manifest.get("payload_files") == 1 + config.textures + config.movies and
         manifest.get("texture_count") == config.textures and manifest.get("movie_count") == config.movies and
         manifest.get("source_bink_count") == config.movies, "Wrong frozen catalogue counts")
    inventory, payload, movies = (manifest[key] for key in ("inventory", "payload", "movies"))
    need(isinstance(inventory, dict) and isinstance(payload, dict) and isinstance(movies, dict), "Malformed frozen inventory")
    need(len({n.casefold() for n in inventory}) == len(inventory), "Case-colliding frozen inventory")
    movie_names = list(movies)
    need(len(movie_names) == config.movies and len({n.casefold() for n in movie_names}) == config.movies, "Ambiguous movie catalogue")
    for name in movie_names:
        freeze.safe_relative(name)
        need(name.endswith(".mp4") and name == name.lower() and not PurePosixPath(name).name.startswith("."), "Unexpected movie name")
    pngs = {p for p in payload if re.fullmatch(r"textures/replace/[0-9A-Fa-f]{16}\.png", p)}
    need(len(pngs) == config.textures and len({p.casefold() for p in pngs}) == config.textures, "Wrong/ambiguous PNG catalogue")
    need(set(payload) == {"game/game.iso"} | pngs | {"video_hd/" + n for n in movies}, "Unexpected payload names")
    need(payload["game/game.iso"]["sha256"] == config.iso_sha256, "Unsupported frozen ISO")
    actual = {p.relative_to(config.snapshot).as_posix() for p in freeze.walk_files(config.snapshot)}
    need(actual == set(inventory) | {"snapshot-manifest.json"}, "Unlisted/missing frozen evidence")
    for number, (relative, row) in enumerate(inventory.items(), 1):
        path = joined(config.snapshot, relative)
        need(path.stat().st_size == row["bytes"] and digest(path) == row["sha256"], "Frozen inventory mismatch: " + relative)
        if relative in payload:
            need(row["bytes"] == payload[relative]["bytes"] and row["sha256"] == payload[relative]["sha256"], "Payload disagrees with inventory")
        if number % 1000 == 0:
            event("inventory", files_checked=number)
    return pngs


def check_textures(config, manifest, pngs):
    package = read_json(config.proof_manifest, config.proof_manifest_sha256)
    need(package.get("format") == "mc3-reviewed-proof-package-v1" and package.get("complete") is True and
         package.get("ledger_sha256") == config.ledger_sha256, "Untrusted/incomplete proof package")
    archive = joined(config.proof_manifest.parent, package["archive"]["path"])
    need(package["archive"]["sha256"] == config.proof_archive_sha256 and
         digest(archive) == config.proof_archive_sha256 and archive.stat().st_size == package["archive"]["bytes"], "Proof archive trust-anchor mismatch")
    files = {r["path"]: r for r in package["files"]}
    need(len(files) == len(package["files"]), "Duplicate proof package members")
    proof_api.verify_archive(archive, files)
    modes, dimensions = Counter(), Counter()
    with zipfile.ZipFile(archive) as zipped:
        ledger_raw = zipped.read(package["ledger"])
        need(hashlib.sha256(ledger_raw).hexdigest() == config.ledger_sha256, "Proof ledger mismatch")
        ledger = document(ledger_raw)
        frozen_ledger = config.snapshot / "evidence/texture-records/reviewed_fixes.json"
        need(digest(frozen_ledger) == config.ledger_sha256 == manifest["ledger_sha256"] and
             frozen_ledger.read_bytes() == ledger_raw, "Frozen ledger differs from authenticated proof")
        audit = read_json(config.snapshot / "evidence/texture-records/texture-audit.json", manifest["texture_audit_sha256"])
        need(audit.get("format") == "mc3-reviewed-texture-audit-v1" and audit.get("complete") is True and audit.get("base_complete") is True and
             audit.get("base_model") == "ultrasharp-4x" and audit.get("base_pass_id") == PASS_ID and
             audit.get("current_model") == "mixed-reviewed" and audit.get("ledger_sha256") == config.ledger_sha256 and
             isinstance(audit.get("issues"), dict) and not any(audit["issues"].values()), "Frozen reviewed audit is incomplete")
        counts = audit["counts"]
        need(counts["eligible"] == counts["base_named_pass_verified"] == config.textures and
             counts["reviewed_fixes"] == counts["accounted_changed_pass_records"] == config.reviewed and
             counts["current_original_named_pass"] == config.textures-config.reviewed, "Wrong reviewed audit accounting")
        passes = document(zipped.read(TEXTURES + "upscale_passes.json"))
        named = passes["passes"][PASS_ID]
        entries = ledger["entries"]
        need(ledger.get("format") == "mc3-reviewed-texture-fixes-v1" and
             ledger["base_pass_id"] == PASS_ID and ledger["base_model"] == "ultrasharp-4x" and
             ledger["base_context"] == named["context"] and len(entries) == config.reviewed, "Wrong original-pass/reviewed context")
        names = {PurePosixPath(p).stem.upper() + ".png": p for p in pngs}
        need(set(names) == set(named["completed"]) and set(entries) <= set(names), "PNG catalogue differs from named pass")
        for name, relative in names.items():
            expected = named["completed"][name]["output_sha256"]
            if name in entries:
                entry = entries[name]
                need(entry["before_sha256"] == expected and
                     hashlib.sha256(zipped.read(entry["base_archive"])).hexdigest() == expected, "Reviewed archive differs from original pass")
                expected = entry["after_sha256"]
                corrected = zipped.read(TEXTURES + "replace/" + name)
                need(hashlib.sha256(corrected).hexdigest() == expected, "Proof correction differs from ledger")
                proof_size = png_info(corrected, "proof correction " + name)
                old_size = png_info(zipped.read(entry["base_archive"]), "proof original archive " + name)
                need((proof_size["width"], proof_size["height"]) == (old_size["width"], old_size["height"]), "Reviewed panel dimensions differ from original archive")
            need(manifest["inventory"][relative]["sha256"] == expected, "Frozen PNG differs from original-pass/reviewed hash: " + name)
            info = png_info(joined(config.snapshot, relative), relative)
            if name in entries:
                need((info["width"], info["height"]) == (proof_size["width"], proof_size["height"]), "Frozen reviewed-panel dimensions differ from proof")
            modes[info["mode"]] += 1
            dimensions[f"{info['width']}x{info['height']}"] += 1
    return {"png_hashes_verified": config.textures, "original_pass": config.textures-config.reviewed,
            "reviewed": config.reviewed, "png_chunks_verified_and_pixels_decoded": config.textures,
            "modes": dict(modes), "dimensions": dict(dimensions),
            "scope": "Exact frozen-byte parity and fresh PNG integrity decode; not a new neural, source-alpha or visual-quality audit."}


def movie_evidence(config, manifest, target, api, seen=None):
    seen = set() if seen is None else seen
    need(target not in seen and len(seen) < config.movies, "Circular identical-source alias chain")
    seen = seen | {target}
    row = manifest["movies"][target]
    need(row.get("classification") in CLASSES, "Unknown frozen movie classification")
    output = config.snapshot / "video_hd" / target
    source_rel = row["source_bink"]
    expected_source = "evidence/source-video/" + str(PurePosixPath(target).with_suffix(".bik"))
    need(isinstance(source_rel, str) and source_rel.casefold() == expected_source.casefold(), "Unexpected frozen source path")
    source = joined(config.snapshot, source_rel)
    original = manifest["all_copied_sources"][source_rel]
    header = api.bink_header(source)
    need(all(original[k] == manifest["inventory"][source_rel][k] for k in ("bytes", "sha256")) and
         header == original["header"] and all(row[k] == manifest["payload"]["video_hd/"+target][k] for k in ("bytes", "sha256")), "Frozen source/output anchors disagree")
    anchors = {}
    for key, suffix in (("json", ".upscale.json"), ("marker", ".model.txt")):
        anchor = row["anchors"][key]
        relative = "evidence/video-records/" + str(PurePosixPath(target).with_suffix(suffix))
        path = joined(config.snapshot, relative)
        if anchor["present"]:
            need(anchor["path"] == relative and digest(path) == anchor["sha256"], "Frozen sidecar anchor mismatch")
            anchors[key] = path.read_bytes()
        else:
            need(anchor["path"] is None and anchor["sha256"] is None and not os.path.lexists(path), "Frozen absence is not preserved")
            anchors[key] = None
    saved = document(anchors["json"]) if anchors["json"] is not None else None
    need(saved == row["recorded_metadata"], "Frozen JSON differs from recorded metadata")
    classified = freeze.classify_movie(saved, anchors["marker"], row, original, manifest["model_identity"])
    strict = row["classification"] == "strict-original-record"
    need(row["strict_recorded_provenance"] is strict, "Inconsistent strict classification")
    reuse = (saved or {}).get("identical_source_reuse")
    reuse_valid = True
    if reuse:
        other_target = str(PurePosixPath(freeze.safe_relative(reuse["original_movie"])).with_suffix(".mp4")).lower()
        other = manifest["movies"].get(other_target, {})
        reuse_valid = (reuse.get("method") == "byte-identical-original-and-exact-picture-recipe-v1" and
            reuse.get("original_output_sha256") == row["sha256"] == other.get("sha256") and
            reuse.get("original_source_sha256") == original["sha256"] ==
            manifest["all_copied_sources"].get(other.get("source_bink"), {}).get("sha256") and
            reuse.get("original_record_sha256") == other.get("anchors", {}).get("json", {}).get("sha256"))
    if strict:
        need(classified["strict_recorded_provenance"] is True, "Strict frozen provenance does not revalidate")
        need(reuse_valid, "Strict identical-source proof does not close in frozen cohort")
        if reuse:
            need(other_target != target and other.get("classification") == "strict-original-record" and
                 other.get("strict_recorded_provenance") is True, "Strict alias requires a different strict-origin parent")
            parent_row, _path, _header, parent_strict = movie_evidence(config, manifest, other_target, api, seen)
            need(parent_strict and parent_row["recorded_metadata"]["recipe"] == saved["recipe"], "Strict alias parent picture/model recipe differs")
    else:
        need(classified["classification"] == row["classification"] or
             (row["classification"] == "historical-json-unverified" and classified["strict_recorded_provenance"] and reuse and not reuse_valid),
             "Historical classification disagrees with frozen evidence")
    return row, output, header, strict


def inspect_movie(config, manifest, target, api, ff):
    row, output, header, strict = movie_evidence(config, manifest, target, api)
    info = api.probe(ff, output, count_frames=True)  # Full software decode; the private facade fixes threads=2.
    api.validate_info(info, dict(header, fps=info["fps"]), header["frames"])
    need(len(info["timeline"]["pts"]) == header["frames"] == len(info["timeline"]["durations"]), "Decoded frame list count differs from original")
    result = {"classification": row["classification"], "strict_recorded_provenance": strict,
              "decode_and_frame_checks": "passed", "source": header,
              "output": {k: v for k, v in info.items() if k != "timeline"}, "sha256": row["sha256"]}
    try:
        need(Fraction(info["fps"]) == Fraction(header["fps"]), "Output/source frame rates differ")
        result["exact_source_timing"] = api.validate_timeline(info, header["fps"], header["frames"])["timing"]
    except (ValueError, TypeError, KeyError, ZeroDivisionError, ValidationError) as error:
        result["exact_source_timing"] = False
        result["timing_difference"] = str(error)
        result["observed_timeline"] = {k: v for k, v in info["timeline"].items() if k not in ("pts", "durations")}
        timeline = info["timeline"]
        try:
            base, fps = Fraction(timeline["time_base"]), Fraction(header["fps"])
            errors = [Fraction(pts)*base-Fraction(i, 1)/fps for i, pts in enumerate(timeline["pts"])]
            first = next((i for i, value in enumerate(errors) if value), None)
            result["timing_metrics"] = {"source_fps": str(fps), "output_fps": info["fps"], "output_time_base": str(base),
                "source_total_seconds": str(Fraction(header["frames"], 1)/fps),
                "observed_total_seconds": str(Fraction(timeline["duration_ts"])*base),
                "max_absolute_pts_error_seconds": str(max(map(abs, errors), default=Fraction(0))),
                "first_mismatching_frame": first, "first_pts_error_seconds": str(errors[first]) if first is not None else None}
        except (ValueError, TypeError, KeyError, ZeroDivisionError):
            result["timing_metrics"] = {"error": "Timeline fields cannot form valid exact rational metrics"}
        result["compatibility"] = "failed-strict-timing" if strict else "legacy-review-pending"
    return result


def validate(config, *, execute=False):
    config.snapshot, config.proof_manifest, config.report = map(freeze.absolute, (config.snapshot, config.proof_manifest, config.report))
    plan = {"mode": "plan", "snapshot": str(config.snapshot), "report": str(config.report),
            "expected_payload_files": 1+config.textures+config.movies, "execute_requested": False,
            "no_writes_or_decode": True, "snapshot_manifest_sha256_required": True}
    if not execute:
        return plan
    need(isinstance(config.snapshot_sha256, str) and freeze.SHA.fullmatch(config.snapshot_sha256), "Explicit frozen manifest SHA-256 required")
    output = config.report.parent
    need(not output.exists() and output.parent.is_dir() and config.report.suffix == ".json", "Report needs a NEW directory below an existing parent")
    need(config.snapshot not in output.parents and output != config.snapshot and config.proof_manifest.parent not in output.parents and
         output != config.proof_manifest.parent, "Report directory overlaps frozen inputs")
    if config.external_g:
        need(output.drive.upper() == "G:" and ROOT not in output.parents and output != ROOT, "Report must be external on G:")
    freeze.plain(output.parent, directory=True)
    output.mkdir()
    result = {"format": "mc3-frozen-content-validation-v1", "status": "running", "complete": False,
              "started_at": datetime.now(timezone.utc).isoformat(), "snapshot_manifest_sha256": config.snapshot_sha256,
              "movies": {}, "errors": {}, "warnings": {}, "inputs_modified": None, "input_integrity": "not-yet-reverified",
              "inference_or_retime_or_adoption": False, "original_bink_decode": "Header only; full source decode not claimed."}
    with (output / "progress.jsonl").open("xb") as progress:
        def event(phase, **values):
            progress.write((json.dumps({"at": datetime.now(timezone.utc).isoformat(), "phase": phase, **values})+"\n").encode())
            progress.flush(); os.fsync(progress.fileno())
        event("started")
        try:
            code = [Path(__file__), Path(freeze.__file__), Path(proof_api.__file__), ROOT / "tools/video/upscale_movies.py"]
            result["validation_code_sha256"] = {str(p): digest(p) for p in code}
            manifest_path = config.snapshot / "snapshot-manifest.json"
            manifest = read_json(manifest_path, config.snapshot_sha256)
            pngs = check_inventory(config, manifest, event)
            frozen_model = read_json(config.snapshot / "evidence/current-model-identity.json")
            need(frozen_model == manifest["model_identity"], "Frozen model identity differs from manifest")
            result["textures"] = check_textures(config, manifest, pngs)
            counts = Counter(row["classification"] for row in manifest["movies"].values())
            need(all(manifest["movie_cohort_counts"].get(k, 0) == counts[k] for k in CLASSES), "Frozen cohort counts disagree")
            api, ff, result["decoder"] = video_api()
            proof_archive = joined(config.proof_manifest.parent, read_json(config.proof_manifest)["archive"]["path"])
            for target in sorted(manifest["movies"]):
                try:
                    row = inspect_movie(config, manifest, target, api, ff)
                    result["movies"][target] = row
                    if row.get("compatibility") == "failed-strict-timing":
                        result["errors"][target] = row["timing_difference"]
                    elif row.get("compatibility") == "legacy-review-pending":
                        result["warnings"][target] = row["timing_difference"]
                except (OSError, RuntimeError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
                    result["errors"][target] = str(error)
                event("movie", movie=target, checked=len(result["movies"]), errors=len(result["errors"]), warnings=len(result["warnings"]))
            check_inventory(config, manifest, event)
            need(digest(manifest_path) == config.snapshot_sha256, "Frozen manifest changed during validation")
            need(digest(config.proof_manifest) == config.proof_manifest_sha256, "Proof manifest changed during validation")
            need(digest(proof_archive) == config.proof_archive_sha256, "Proof archive changed during validation")
            need(digest(Path(result["decoder"]["ffprobe"])) == result["decoder"]["ffprobe_sha256"], "Decoder changed during validation")
            need(all(digest(Path(p)) == h for p, h in result["validation_code_sha256"].items()), "Validation helper source changed during run")
            result["inputs_modified"] = False
            result["input_integrity"] = "all-recorded-input-hashes-reverified"
            result["cohort_counts"] = dict(counts)
            result["all_outputs_decoded_and_frame_checked"] = len(result["movies"]) == config.movies
            result["all_outputs_have_strict_recorded_provenance"] = (len(result["movies"]) == config.movies and
                all(row["strict_recorded_provenance"] for row in result["movies"].values()))
            result["checks_finished"] = True
            result["complete"] = not result["errors"] and not result["warnings"] and result["all_outputs_decoded_and_frame_checked"]
            result["status"] = "failed" if result["errors"] else "compatibility-pending" if result["warnings"] else "complete"
        except (OSError, RuntimeError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
            result["status"] = "failed"
            result["input_integrity"] = "failed-or-not-reverified"
            result["errors"]["global"] = str(error)
        except (KeyboardInterrupt, SystemExit) as error:
            result["status"] = "interrupted"
            result["input_integrity"] = "interrupted-before-final-reverification"
            result["errors"]["global"] = type(error).__name__
        result["finished_at"] = datetime.now(timezone.utc).isoformat()
        result["release_ready"] = False  # Native archive, clean install and final source/release review remain separate.
        event("finished", status=result["status"], complete=result["complete"])
    with config.report.open("xb") as report:
        report.write((json.dumps(result, indent=2, allow_nan=False)+"\n").encode())
        report.flush(); os.fsync(report.fileno())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", required=True, type=Path)
    parser.add_argument("--snapshot-manifest-sha256")
    parser.add_argument("--proof-manifest", required=True, type=Path)
    parser.add_argument("--trusted-proof-manifest-sha256", default=PROOF_SHA)
    parser.add_argument("--trusted-proof-archive-sha256", default=ARCHIVE_SHA)
    parser.add_argument("--trusted-ledger-sha256", default=LEDGER_SHA)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    config = Config(args.snapshot, args.snapshot_manifest_sha256, args.proof_manifest, args.report,
                    args.trusted_proof_manifest_sha256, args.trusted_proof_archive_sha256, args.trusted_ledger_sha256)
    try:
        result = validate(config, execute=args.execute)
        print(json.dumps({k: result[k] for k in ("mode", "status", "complete", "errors", "warnings") if k in result}, indent=2))
        return 0 if not args.execute or result["complete"] else 2 if result["status"] == "compatibility-pending" else 1
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print("Frozen validation refused: " + str(error))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
