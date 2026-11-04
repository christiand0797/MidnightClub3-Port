#!/usr/bin/env python3
"""Build the vehicle_pack.py gate receipt (mc3-offline-pbr-gate-v1) only from real audit and backup files.

  py -3.12 tools/pbr/gate_receipt.py audit     # strict movie audit (decodes all 106 published movies)
  py -3.12 tools/pbr/gate_receipt.py backup    # full project copy + every-file SHA-256 verification
  py -3.12 tools/pbr/gate_receipt.py receipt   # compose the receipt from those two results, print its SHA

There is no way to enter counts or digests by hand: `receipt` reads the audit report written by
tools/video/upscale_movies.py --audit-only and the verified-backup marker written by `backup`, re-validates
both, and refuses if the media changed in between. Review the receipt before passing its SHA to vehicle_pack.py.

The movie model is Remacri (commit 369e907 made it the default; x4plus looked plastic). The older
tools/maintenance/overnight_pipeline.py --run still hardcodes x4plus and then runs generate.py --force on the
starter set, which would overwrite textures/pbr/manifest.json, so it is not used here. Its inventory, copy and
verification helpers are reused unchanged.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "maintenance"))
import overnight_pipeline as op  # noqa: E402

MODEL = "remacri-4x"
EXPECTED = op.EXPECTED_MOVIES
STATE = ROOT / ".local" / "pbr_gate"
SCHEMA = "mc3-offline-pbr-gate-v1"


class GateError(RuntimeError):
    pass


def require(ok, message):
    if not ok:
        raise GateError(message)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def validate_strict_movies(report, model=MODEL, expected=EXPECTED):
    """The video half of overnight_pipeline.validate_media_reports, for the current movie model."""
    rows = report.get("details", [])
    require(report.get("mode") == "audit", "not an --audit-only report")
    require(report.get("requested_model") == model, f"audit was not for {model}")
    require(report.get("require_original_provenance") is True, "audit did not require original provenance")
    require(report.get("movies") == expected and len(rows) == expected, f"audit does not cover {expected} movies")
    require(len({row.get("movie") for row in rows}) == expected, "duplicate movie rows")
    require(report.get("covers_all_sources") is True, "audit does not cover every original movie")
    require(report.get("failed") == 0, f"{report.get('failed')} movies failed the audit")
    for key in ("all_requested_model_validated", "all_source_movies_validated",
                "all_original_source_and_weights_verified"):
        require(report.get(key) is True, f"audit flag {key} is not true")
    bad = [row.get("movie") for row in rows
           if not (row.get("status") == "verified" and row.get("output_validated") is True
                   and row.get("recorded_model") == model and row.get("origin") == "original-bink"
                   and row.get("historical_source_and_weights_verified") is True)]
    require(not bad, f"{len(bad)} movies are not strictly verified, e.g. {bad[:3]}")
    return {"completed": len(rows), "total": expected, "failed": 0}


def compose_receipt(report_bytes, marker_bytes, model=MODEL, expected=EXPECTED):
    """Receipt bytes from the exact audit-report and backup-marker bytes."""
    counts = validate_strict_movies(json.loads(report_bytes), model, expected)
    marker = json.loads(marker_bytes)
    require(marker.get("status") == "verified", "backup marker is not verified")
    manifest_sha = marker.get("manifest_sha256")
    require(isinstance(manifest_sha, str) and op.SHA256.fullmatch(manifest_sha), "backup marker lacks a manifest SHA")
    require(marker.get("media_gate", {}).get("audit_report_sha256") == sha256_bytes(report_bytes),
            "backup was not made after this audit report")
    receipt = {"schema": SCHEMA, "offline_pbr_adoption_allowed": True,
               "strict_movies": dict(counts, final_audit_sha256=sha256_bytes(report_bytes), model=model),
               "full_project_backup": {"all_files_verified": True, "manifest_sha256": manifest_sha,
                                       "verification_sha256": sha256_bytes(marker_bytes),
                                       "files": marker.get("files"), "bytes": marker.get("bytes"),
                                       "destination": marker.get("destination")}}
    return (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode("utf-8")


def load_audit(state):
    record = json.loads((state / "audit.json").read_text(encoding="utf-8"))
    report = Path(record["report"])
    data = report.read_bytes()
    require(sha256_bytes(data) == record["report_sha256"], "audit report changed after the audit")
    return record, data


def cmd_audit(args):
    args.state.mkdir(parents=True, exist_ok=True)
    report = args.state / "movie_audit.json"
    before = op.media_signature(ROOT)
    code = subprocess.run([sys.executable, "tools/video/upscale_movies.py", "--audit-only", "--model", MODEL,
                           "--require-original-provenance", "--report", str(report)], cwd=ROOT).returncode
    data = report.read_bytes()
    counts = validate_strict_movies(json.loads(data))  # explains the failure before the exit code does
    require(code == 0, f"upscale_movies.py --audit-only exited {code}")
    require(op.media_signature(ROOT) == before, "media changed during the audit")
    op.atomic_json(args.state / "audit.json", {"report": str(report), "report_sha256": sha256_bytes(data),
                                               "media_signature": before, "model": MODEL, "verified_at": op.utc(),
                                               **counts})
    print(f"strict movies {counts['completed']}/{counts['total']}, report sha256 {sha256_bytes(data)}")
    return 0


def cmd_backup(args):
    record, data = load_audit(args.state)
    signature = op.media_signature(ROOT)
    require(signature == record["media_signature"], "media changed since the audit; run audit again")
    require(args.backup_root.is_dir(), f"backup root is unavailable: {args.backup_root}")
    destination = args.backup_root / ("MidnightClub3-Port_PrePBR_" + datetime.now().strftime("%Y-%m-%d_%H%M%S"))
    op.safe_backup_destination(ROOT, destination)
    snapshot = op.inventory(ROOT)
    required = sum(v["bytes"] for v in snapshot["files"].values())
    require(shutil.disk_usage(args.backup_root).free > required + 1024 ** 3,
            "not enough free space for the project plus 1 GiB")
    print(f"copying {len(snapshot['files'])} files ({required / 1024 ** 3:.1f} GiB) to {destination}", flush=True)
    code = subprocess.run(op.robocopy_command(ROOT, destination, snapshot)).returncode
    require(code < 8, f"robocopy failed with exit code {code}")
    manifest_path = args.backup_root / "verification" / (destination.name + ".manifest.json")
    manifest = op.verify_backup(ROOT, destination, snapshot, manifest_path,
                                lambda n, total: print(f"  verified {n}/{total}", flush=True) if n % 4096 == 0 else None)
    require(op.media_signature(ROOT) == signature, "media changed during the backup")
    marker_path = manifest_path.with_name(destination.name + ".verified.json")
    marker = {"version": 1, "status": "verified", "source": str(ROOT.resolve()), "destination": str(destination),
              "manifest": str(manifest_path), "manifest_sha256": op.sha256(manifest_path),
              "media_signature": signature,
              "media_gate": {"provenance_policy": op.MEDIA_PROVENANCE_POLICY, "model": MODEL,
                             "audit_report_sha256": record["report_sha256"]},
              "files": manifest["file_count"], "bytes": manifest["byte_count"], "verified_at": op.utc()}
    op.atomic_json(marker_path, marker)
    op.verify_gate(marker_path, ROOT, signature)
    op.atomic_json(args.state / "backup.json", {"marker": str(marker_path)})
    print(f"verified backup: {manifest['file_count']} files, marker {marker_path}")
    return 0


def cmd_receipt(args):
    record, report_bytes = load_audit(args.state)
    marker_path = Path(json.loads((args.state / "backup.json").read_text(encoding="utf-8"))["marker"])
    signature = op.media_signature(ROOT)
    require(signature == record["media_signature"], "media changed since the audit")
    # The project keeps changing after the backup (other work goes on); the backup itself must be intact.
    op.verify_gate(marker_path, ROOT, signature, require_source_snapshot=False)
    data = compose_receipt(report_bytes, marker_path.read_bytes())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)  # bytes as hashed (write_text would turn newlines into CRLF)
    print(data.decode("utf-8"))
    print(f"gate receipt {args.out}\ngate sha256 = {sha256_bytes(data)}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("audit", "backup", "receipt"))
    ap.add_argument("--state", type=Path, default=STATE)
    ap.add_argument("--backup-root", type=Path, default=Path(r"G:\MC3_Backups"))
    ap.add_argument("--out", type=Path, default=STATE / "gate_receipt.json")
    args = ap.parse_args(argv)
    try:
        return {"audit": cmd_audit, "backup": cmd_backup, "receipt": cmd_receipt}[args.step](args)
    except (GateError, RuntimeError, OSError, KeyError, ValueError) as error:
        print(f"gate not issued: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
