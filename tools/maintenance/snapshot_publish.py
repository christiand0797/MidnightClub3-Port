#!/usr/bin/env python3
"""Copy, verify, archive and privately publish a complete project checkpoint.

Default invocation is a read-only plan. Actual copying requires --execute and
--checkpoint-confirmed. Resume the same external --state file after interruption.
This is a checkpoint backup, not the final post-media/pre-PBR completion gate.
"""
import argparse
import contextlib
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REPO = "christiand0797/MidnightClub3-Port"
DEFAULT_BACKUPS = Path(r"G:\MC3_Backups")
PART_BYTES = 1900 * 1024 * 1024
VOLATILE_PREFIXES = (".local/", "templates/game/out/native/logs/")


def now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def atomic_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, filename = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    candidate = Path(filename)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(data, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(candidate, path)
    finally:
        candidate.unlink(missing_ok=True)


def inside(path, parent):
    try:
        Path(path).resolve().relative_to(Path(parent).resolve())
        return True
    except ValueError:
        return False


def validate_paths(source, backups, state_path=None):
    source, backups = Path(source).resolve(), Path(backups).resolve()
    if not source.is_dir() or not backups.is_dir():
        raise ValueError("source and backup root must already be existing directories")
    if inside(backups, source) or inside(source, backups):
        raise ValueError("source and backup root must be disjoint; recursive self-backups are forbidden")
    if state_path is not None and inside(state_path, source):
        raise ValueError("snapshot control/state/manifest files must be outside the source project")
    return source, backups


def inventory(root):
    root = Path(root).resolve()
    files, directories, reparse = {}, [], []
    for folder, dirs, names in os.walk(root, followlinks=False):
        parent = Path(folder)
        for name in list(dirs):
            path = parent / name
            info = path.lstat()
            relative = path.relative_to(root).as_posix()
            if path.is_symlink() or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                dirs.remove(name)
                reparse.append({"path": relative, "kind": "directory-reparse-point"})
            else:
                directories.append(relative)
        for name in names:
            path = parent / name
            info = path.lstat()
            relative = path.relative_to(root).as_posix()
            if path.is_symlink() or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                reparse.append({"path": relative, "kind": "file-reparse-point"})
                continue
            if not stat.S_ISREG(info.st_mode):
                raise ValueError(f"unsupported non-regular project item: {relative}")
            files[relative] = {"size": info.st_size, "mtime_ns": info.st_mtime_ns}
    return {"files": files, "directories": sorted(directories), "reparse_points": reparse}


def hash_stable(path):
    before = path.stat()
    digest = sha256(path)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
        raise RuntimeError(f"file changed while hashing: {path}")
    return digest


def hash_snapshot(snapshot):
    initial = inventory(snapshot)
    if initial["reparse_points"]:
        raise ValueError("copied snapshot unexpectedly contains reparse points")
    rows = []
    for index, (relative, info) in enumerate(sorted(initial["files"].items()), 1):
        rows.append({"path": relative, "size": info["size"], "sha256": hash_stable(snapshot / relative)})
        if index % 500 == 0:
            print(f"snapshot hash: {index}/{len(initial['files'])}", flush=True)
    final = inventory(snapshot)
    if final != initial:
        raise RuntimeError("snapshot inventory changed during hashing")
    return {"files": rows, "directories": initial["directories"],
            "file_count": len(rows), "total_bytes": sum(row["size"] for row in rows)}


def volatile(relative):
    return any(relative.startswith(prefix) for prefix in VOLATILE_PREFIXES)


def compare_source(source, snapshot_manifest):
    live = inventory(source)
    expected = {row["path"]: row for row in snapshot_manifest["files"]}
    differences = []
    for index, relative in enumerate(sorted(set(live["files"]) | set(expected)), 1):
        row = expected.get(relative)
        current = live["files"].get(relative)
        kind = None
        if row is None:
            kind = "new-in-live-source"
        elif current is None:
            kind = "missing-from-live-source"
        else:
            try:
                if current["size"] != row["size"] or hash_stable(source / relative) != row["sha256"]:
                    kind = "source-content-differs"
            except (OSError, RuntimeError) as error:
                kind = "source-changed-during-verification"
        if kind:
            differences.append({"path": relative, "kind": kind, "allowed_volatile": volatile(relative)})
        if index % 500 == 0:
            print(f"source comparison: {index}/{len(live['files'])}", flush=True)
    for relative in sorted(set(live["directories"]) ^ set(snapshot_manifest["directories"])):
        differences.append({"path": relative + "/", "kind": "directory-set-differs", "allowed_volatile": volatile(relative + "/")})
    after = inventory(source)
    # Metadata changes after a file was hashed must be reported too. We do not
    # claim a VSS-style, simultaneous live-filesystem snapshot.
    for relative in sorted(set(live["files"]) | set(after["files"])):
        if live["files"].get(relative) != after["files"].get(relative):
            differences.append({"path": relative, "kind": "source-metadata-changed-during-comparison", "allowed_volatile": volatile(relative)})
    for relative in sorted(set(live["directories"]) ^ set(after["directories"])):
        differences.append({"path": relative + "/", "kind": "source-directory-changed-during-comparison", "allowed_volatile": volatile(relative + "/")})
    for item in live["reparse_points"] + after["reparse_points"]:
        differences.append({"path": item["path"], "kind": "source-reparse-point-excluded", "allowed_volatile": False})
    return {"all_regular_files_match_at_checks": not differences,
            "stable_files_match": not any(not item["allowed_volatile"] for item in differences),
            "differences": differences, "source_reparse_points_excluded": live["reparse_points"],
            "note": "Individual copied files are hashed; live source comparison spans time and is not a simultaneous filesystem snapshot."}


def verify_snapshot(snapshot, manifest):
    actual = inventory(snapshot)
    expected = {row["path"]: row for row in manifest["files"]}
    if set(actual["files"]) != set(expected) or actual["directories"] != manifest["directories"]:
        raise ValueError("snapshot path inventory no longer matches its manifest")
    for relative, row in expected.items():
        if actual["files"][relative]["size"] != row["size"] or hash_stable(snapshot / relative) != row["sha256"]:
            raise ValueError(f"snapshot no longer matches SHA256 manifest: {relative}")


def checked_tool(name):
    found = shutil.which(name)
    if not found:
        raise RuntimeError(f"required tool is not on PATH: {name}")
    return found


def run_logged(command, log, cwd=None, allowed=(0,)):
    print("running: " + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    with Path(log).open("a", encoding="utf-8", errors="replace") as handle:
        result = subprocess.run([str(item) for item in command], cwd=cwd, stdout=handle, stderr=subprocess.STDOUT)
    if result.returncode not in allowed:
        raise RuntimeError(f"command exited {result.returncode}; see {log}")
    return result.returncode


def gh_json(arguments, missing_ok=False):
    result = subprocess.run([checked_tool("gh"), *arguments], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        if missing_ok and "HTTP 404" in result.stderr:
            return None
        raise RuntimeError(f"GitHub command failed: {result.stderr.strip()[-2000:]}")
    return json.loads(result.stdout)


def assert_private_repo(repo):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("repo must be OWNER/REPO on github.com")
    record = gh_json(["api", f"repos/{repo}"])
    if not record.get("private"):
        raise RuntimeError("the full project snapshot destination must remain a private repository")
    return record


def save_state(path, state, phase=None):
    if phase:
        state["phase"] = phase
    state["updated_at"] = now()
    atomic_json(path, state)


@contextlib.contextmanager
def state_lock(path):
    path = Path(path).with_suffix(".lock")
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = path.open("a+b")
    locked = False
    try:
        handle.seek(0)
        if not handle.read(1):
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        locked = True
        yield
    finally:
        if locked:
            handle.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_UN)
        handle.close()


def make_plan(args):
    source, backups = validate_paths(args.source, args.backup_root, args.state)
    listing = inventory(source)
    size = sum(row["size"] for row in listing["files"].values())
    snapshot_id = args.name or f"MidnightClub3-Port_{datetime.now().strftime('%Y-%m-%d_%H%M%S')}_github_{uuid.uuid4().hex[:6]}"
    if not re.fullmatch(r"MidnightClub3-Port_[A-Za-z0-9_.-]+", snapshot_id):
        raise ValueError("snapshot name must start MidnightClub3-Port_ and contain only letters, digits, dots, hyphens or underscores")
    state_path = Path(args.state).resolve() if args.state else backups / (snapshot_id + ".control") / "state.json"
    control = state_path.parent
    snapshot = backups / snapshot_id
    if inside(control, snapshot) or inside(snapshot, control) or inside(control, source):
        raise ValueError("snapshot, source and control directory must be separate siblings/trees")
    free = shutil.disk_usage(backups).free
    # Compression may be ineffective for ISO/MP4/PNG data. Reserve one copy plus
    # an uncompressed-sized archive and a modest scratch allowance.
    required = size * 2 + 2 * 1024**3
    return {"schema": 1, "snapshot_id": snapshot_id, "source": str(source), "backup_root": str(backups),
            "snapshot": str(snapshot), "control": str(control), "state_path": str(state_path),
            "repo": args.repo, "tag": "checkpoint-" + snapshot_id.removeprefix("MidnightClub3-Port_"),
            "source_files_at_plan": len(listing["files"]), "source_bytes_at_plan": size,
            "source_reparse_points": listing["reparse_points"], "free_backup_bytes": free,
            "estimated_required_bytes": required, "space_available": free >= required,
            "volume_bytes": PART_BYTES, "phase": "planned", "created_at": now(),
            "purpose": "current full-project checkpoint; not the final post-media PrePBR backup"}


def copy_project(state, args):
    if not args.checkpoint_confirmed:
        raise RuntimeError("copy/resume-copy requires --checkpoint-confirmed after game, media workers and source agents reach a stable checkpoint")
    source, backups = validate_paths(state["source"], state["backup_root"], state["state_path"])
    snapshot = Path(state["snapshot"]).resolve()
    if snapshot.parent != backups or inside(snapshot, source) or snapshot == backups:
        raise ValueError("resolved snapshot target escaped its intended backup root")
    before = inventory(source)
    if before["reparse_points"]:
        # /XJ deliberately excludes junctions. Refuse to silently call this a
        # complete project if anything in the current tree needs interpretation.
        raise RuntimeError(f"project contains reparse points; inspect plan before copying: {before['reparse_points']}")
    control = Path(state["control"])
    control.mkdir(parents=True, exist_ok=True)
    atomic_json(control / "source-before-copy.json", before)
    state["source_git_head"] = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    state["copy_started_at"] = now()
    save_state(Path(state["state_path"]), state, "copying")
    code = run_logged([checked_tool("robocopy"), source, snapshot, "/E", "/COPY:DAT", "/DCOPY:DAT", "/XJ",
                       "/R:1", "/W:1", "/MT:8", "/NP", "/BYTES", "/NFL", "/NDL"], control / "robocopy.log", allowed=tuple(range(8)))
    state["robocopy_exit"] = code
    state["copy_completed_at"] = now()
    state["copied"] = True
    save_state(Path(state["state_path"]), state, "copied")


def create_manifest(state):
    snapshot, source = Path(state["snapshot"]), Path(state["source"])
    data = hash_snapshot(snapshot)
    comparison = compare_source(source, data)
    manifest = {"schema": 1, "snapshot_id": state["snapshot_id"], "created_at": now(),
                "source_git_head": state["source_git_head"], "snapshot_root_name": snapshot.name,
                "copy_started_at": state["copy_started_at"], "copy_completed_at": state["copy_completed_at"],
                **data, "source_comparison": comparison,
                "snapshot_files_verified": True, "is_post_media_pbr_gate": False}
    manifest_path = Path(state["control"]) / "snapshot-manifest.json"
    atomic_json(manifest_path, manifest)
    state["manifest"] = str(manifest_path)
    state["manifest_sha256"] = sha256(manifest_path)
    state["snapshot_files"] = data["file_count"]
    state["snapshot_bytes"] = data["total_bytes"]
    state["source_comparison"] = {key: comparison[key] for key in ("all_regular_files_match_at_checks", "stable_files_match")}
    if not comparison["stable_files_match"]:
        save_state(Path(state["state_path"]), state, "source-changed")
        raise RuntimeError(f"non-volatile source files differ from copied snapshot; see {manifest_path}. No archive/upload allowed.")
    state["verified"] = True
    save_state(Path(state["state_path"]), state, "verified")


def archive_parts(first):
    first = Path(first)
    parts = sorted(first.parent.glob(first.name.removesuffix(".001") + ".[0-9][0-9][0-9]"))
    if not parts or parts[0] != first:
        raise ValueError("first split archive volume is missing")
    for index, path in enumerate(parts, 1):
        if path.suffix != f".{index:03d}" or not 0 < path.stat().st_size <= PART_BYTES:
            raise ValueError(f"invalid split archive volume: {path}")
        if index < len(parts) and path.stat().st_size != PART_BYTES:
            raise ValueError(f"non-final archive volume is short: {path}")
    return parts


def archive_listing(first):
    result = subprocess.run([checked_tool("7z"), "l", "-slt", "-ba", "-sccUTF-8", str(first)], capture_output=True,
                            text=True, encoding="utf-8", errors="strict", check=True)
    rows, row = [], {}
    for line in result.stdout.splitlines() + [""]:
        if not line.strip():
            if row:
                rows.append(row)
                row = {}
        elif " = " in line:
            key, value = line.split(" = ", 1)
            row[key] = value
    return rows


def verify_archive_listing(first, snapshot, manifest):
    expected = {snapshot.name + "/" + row["path"]: row["size"] for row in manifest["files"]}
    actual = {}
    dirs = set()
    for row in archive_listing(first):
        name = row.get("Path", "").replace("\\", "/").rstrip("/")
        if row.get("Folder") == "+" or row.get("Attributes", "").startswith("D"):
            dirs.add(name)
        elif "Size" in row and "Type" not in row:
            actual[name] = int(row["Size"])
    if actual != expected:
        missing = sorted(set(expected) - set(actual))[:10]
        extra = sorted(set(actual) - set(expected))[:10]
        raise ValueError(f"archive coverage does not match full snapshot manifest; missing={missing}, extra={extra}")
    expected_dirs = {snapshot.name, *(snapshot.name + "/" + path for path in manifest["directories"])}
    if dirs != expected_dirs:
        raise ValueError("archive directory coverage does not match snapshot (including empty directories)")


def create_archive(state):
    control, snapshot = Path(state["control"]), Path(state["snapshot"])
    manifest = json.loads(Path(state["manifest"]).read_text(encoding="utf-8"))
    if sha256(state["manifest"]) != state["manifest_sha256"]:
        raise ValueError("manifest changed after verification")
    verify_snapshot(snapshot, manifest)
    # Failed/incomplete split archives are retained. 7z cannot safely update a
    # multi-volume archive, so each fresh attempt gets a new external directory.
    attempt = control / f"archive-attempt-{len(state.get('archive_attempts', [])) + 1:02d}"
    attempt.mkdir(exist_ok=False)
    state.setdefault("archive_attempts", []).append(str(attempt))
    save_state(Path(state["state_path"]), state, "archiving")
    archive_base = attempt / (snapshot.name + ".7z")
    run_logged([checked_tool("7z"), "a", "-t7z", "-mx=1", "-mmt=8", "-ms=off", "-sse", "-bd", "-bsp0",
                f"-v{PART_BYTES}b", str(archive_base), snapshot.name], control / "archive-create.log", cwd=snapshot.parent)
    first = Path(str(archive_base) + ".001")
    parts = archive_parts(first)
    run_logged([checked_tool("7z"), "t", "-bd", "-bsp0", str(first)], control / "archive-test.log")
    verify_archive_listing(first, snapshot, manifest)
    rows = [{"name": path.name, "path": str(path), "size": path.stat().st_size, "sha256": sha256(path)} for path in parts]
    checksum_path = control / "SHA256SUMS.txt"
    checksum_path.write_text("".join(f"{row['sha256']}  {row['name']}\n" for row in rows) +
                             f"{state['manifest_sha256']}  snapshot-manifest.json\n", encoding="utf-8", newline="\n")
    state["parts"] = rows
    state["checksums"] = str(checksum_path)
    state["archived"] = True
    state["archive_tested_at"] = now()
    save_state(Path(state["state_path"]), state, "archived")


def release_assets(state):
    paths = [Path(row["path"]) for row in state["parts"]] + [Path(state["manifest"]), Path(state["checksums"])]
    expected = {row["name"]: row for row in state["parts"]}
    for path in paths:
        size = path.stat().st_size
        if not 0 < size < 2 * 1024**3:
            raise ValueError(f"release asset exceeds GitHub limit or is empty: {path}")
        digest = sha256(path)
        if path.name in expected and (size != expected[path.name]["size"] or digest != expected[path.name]["sha256"]):
            raise ValueError(f"archive volume changed after its integrity test: {path}")
    if sha256(state["manifest"]) != state["manifest_sha256"]:
        raise ValueError("snapshot manifest changed before upload")
    return paths


def release_record(state):
    return gh_json(["api", f"repos/{state['repo']}/releases/tags/{quote(state['tag'], safe='')}"], missing_ok=True)


def remote_assets(state, release):
    pages = gh_json(["api", f"repos/{state['repo']}/releases/{release['id']}/assets?per_page=100", "--paginate", "--slurp"])
    return {asset["name"]: asset for page in pages for asset in page}


def asset_matches(asset, path, digest):
    if asset.get("state") != "uploaded" or asset.get("size") != path.stat().st_size:
        return False
    remote_digest = asset.get("digest")
    if remote_digest and remote_digest != "sha256:" + digest:
        return False
    return True


def upload_and_publish(state, draft_only=False):
    assert_private_repo(state["repo"])
    paths = release_assets(state)
    control = Path(state["control"])
    marker = "Snapshot ID: " + state["snapshot_id"]
    notes = (f"Complete private project checkpoint. {marker}\n\n"
             f"Includes the original ISO, extracted game data, current native build, textures, movies, tools, documentation and .git. "
             f"Snapshot: {state['snapshot_files']:,} files, {state['snapshot_bytes']:,} bytes. Git HEAD: {state['source_git_head']}.\n\n"
             "Download every .7z.NNN volume, snapshot-manifest.json and SHA256SUMS.txt into one folder. "
             "Verify SHA256SUMS and extract from .7z.001 with 7-Zip; all volumes are required. "
             "The snapshot-manifest records every copied file's relative path, size and SHA256. "
             "Any live volatile-file differences observed during copying are recorded in that manifest.\n\n"
             "This is a current checkpoint. The ongoing media upscale pass and the later post-media/pre-PBR backup are separate work. "
             "This release does not assert that the game or the PBR conversion is complete.\n")
    notes_path = control / "release-notes.md"
    notes_path.write_text(notes, encoding="utf-8", newline="\n")
    release = release_record(state)
    if release is None:
        run_logged([checked_tool("gh"), "release", "create", state["tag"], "--repo", state["repo"], "--draft", "--target",
                    state["source_git_head"], "--title", "Full project checkpoint " + state["snapshot_id"],
                    "--notes-file", notes_path, "--latest=false"], control / "github-release.log")
        release = release_record(state)
    if marker not in release.get("body", "") or release["tag_name"] != state["tag"]:
        raise RuntimeError("existing release is not owned by this snapshot state")
    state["release_id"] = release["id"]
    state["release_url"] = release["html_url"]
    save_state(Path(state["state_path"]), state, "uploading")
    for path in paths:
        digest = sha256(path)
        assets = remote_assets(state, release)
        asset = assets.get(path.name)
        if asset and asset_matches(asset, path, digest):
            print(f"already uploaded: {path.name}", flush=True)
            continue
        if asset:
            if release["draft"] and asset.get("state") == "starter" and asset.get("size") == 0:
                # GitHub documents that failed uploads can leave an empty
                # starter asset. Only remove this snapshot's empty draft item.
                run_logged([checked_tool("gh"), "api", "--method", "DELETE", f"repos/{state['repo']}/releases/assets/{asset['id']}"],
                           control / "github-upload.log")
            else:
                raise RuntimeError(f"existing release asset differs; preserving it rather than clobbering: {path.name}")
        if not release["draft"]:
            raise RuntimeError("published release is missing an expected asset; refusing to modify it")
        last_error = None
        for attempt in range(3):
            try:
                run_logged([checked_tool("gh"), "release", "upload", state["tag"], str(path), "--repo", state["repo"]], control / "github-upload.log")
            except RuntimeError as error:
                last_error = error
            assets = remote_assets(state, release)
            if path.name in assets and asset_matches(assets[path.name], path, digest):
                break
            if path.name in assets:
                raise RuntimeError(f"incomplete/mismatching asset remains; resume can clean an empty starter: {path.name}")
            if attempt < 2:
                time.sleep(5 * (attempt + 1))
        else:
            raise RuntimeError(f"upload failed after retries: {path.name}: {last_error}")
        state.setdefault("uploaded_assets", {})[path.name] = {"size": path.stat().st_size, "sha256": digest,
                                                              "remote_digest": assets[path.name].get("digest"), "verified_at": now()}
        save_state(Path(state["state_path"]), state, "uploading")
    assert_private_repo(state["repo"])
    assets = remote_assets(state, release)
    for path in paths:
        if path.name not in assets or not asset_matches(assets[path.name], path, sha256(path)):
            raise RuntimeError(f"final release asset verification failed: {path.name}")
    if set(assets) != {path.name for path in paths}:
        raise RuntimeError("release contains unexpected assets; inspect before publishing")
    state["verified_remote_assets"] = [{"name": path.name, "size": assets[path.name]["size"],
                                         "local_sha256": sha256(path), "remote_digest": assets[path.name].get("digest"),
                                         "asset_id": assets[path.name]["id"]} for path in paths]
    state["remote_sizes_verified"] = True
    state["remote_available_sha256_digests_verified"] = True
    state["uploaded"] = True
    save_state(Path(state["state_path"]), state, "uploaded")
    if draft_only:
        return
    if release["draft"]:
        run_logged([checked_tool("gh"), "release", "edit", state["tag"], "--repo", state["repo"], "--draft=false", "--latest=false"],
                   control / "github-release.log")
    release = release_record(state)
    if release["draft"]:
        raise RuntimeError("release remained a draft after publication")
    state["published"] = True
    state["published_at"] = release.get("published_at")
    save_state(Path(state["state_path"]), state, "published")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--backup-root", type=Path, default=DEFAULT_BACKUPS)
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--name", help="optional unique snapshot directory name beginning MidnightClub3-Port_")
    parser.add_argument("--state", type=Path, help="external durable state path; use the same path with --resume")
    parser.add_argument("--execute", action="store_true", help="perform the plan; absent means read-only dry-run")
    parser.add_argument("--resume", action="store_true", help="continue an existing state instead of creating a new snapshot")
    parser.add_argument("--checkpoint-confirmed", action="store_true", help="parent has stopped game/media writers and quiesced source agents for copying")
    parser.add_argument("--stop-after", choices=("copy", "verify", "archive", "upload"))
    parser.add_argument("--draft-only", action="store_true", help="upload and verify all assets but leave the release as a draft")
    args = parser.parse_args()
    if args.resume:
        if not args.state or not args.state.is_file():
            parser.error("--resume requires the existing external --state file")
        state = json.loads(args.state.read_text(encoding="utf-8"))
        if Path(state["state_path"]).resolve() != args.state.resolve():
            raise ValueError("state file location differs from the recorded path")
        validate_paths(state["source"], state["backup_root"], args.state)
    else:
        if args.state and args.state.exists():
            parser.error("state already exists; use --resume or a unique new --state path")
        state = make_plan(args)
    if not args.execute:
        print(json.dumps(state, indent=2))
        return 0
    if not state.get("copied") and not args.checkpoint_confirmed:
        raise RuntimeError("no snapshot started: --checkpoint-confirmed is required for the copy stage")
    if not state.get("space_available", True) and not args.resume:
        raise RuntimeError("insufficient backup space for full copy plus archive")
    if not args.resume and Path(state["snapshot"]).exists():
        raise RuntimeError("new snapshot target already exists; use a unique destination")
    assert_private_repo(state["repo"])
    for tool in ("7z", "robocopy", "gh"):
        checked_tool(tool)
    state_path = Path(state["state_path"])
    with state_lock(state_path):
        save_state(state_path, state)
        try:
            if not state.get("copied"):
                copy_project(state, args)
            if args.stop_after == "copy":
                return 2
            if not state.get("verified"):
                save_state(state_path, state, "verifying")
                create_manifest(state)
            if args.stop_after == "verify":
                return 2
            if not state.get("archived"):
                create_archive(state)
            if args.stop_after == "archive":
                return 2
            upload_and_publish(state, draft_only=args.draft_only or args.stop_after == "upload")
            print(json.dumps({"phase": state["phase"], "snapshot": state["snapshot"], "release_url": state.get("release_url"),
                              "state_path": state["state_path"]}, indent=2))
            return 0 if state.get("published") else 2
        except BaseException as error:
            state["last_error"] = str(error)
            state["last_error_at"] = now()
            save_state(state_path, state)
            raise


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"snapshot publisher: {error}", file=sys.stderr)
        sys.exit(1)
