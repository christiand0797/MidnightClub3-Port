#!/usr/bin/env python3
"""Durable, opt-in MC3 media -> verified full backup -> offline PBR coordinator.

Inventory only (default): py -3.12 tools/maintenance/overnight_pipeline.py --dry-run
Execute authorized work: py -3.12 tools/maintenance/overnight_pipeline.py --run

No process is killed. Existing game/media workers must exit before GPU work starts.
Create .local/stop-overnight-pipeline to stop children at a batch/movie boundary.
PBR never runs unless both media audits and an all-file SHA-256 backup verify.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
PASS_ID = "2026-10-22-originals-pass"
EXPECTED_MOVIES = 106
LIVE = ".local/overnight_pipeline/live"
LIVE_NOTE = "docs/notes/overnight_pipeline.md"


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def atomic_json(path, value):
    atomic_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def atomic_text(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix="." + path.name + ".", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def exclusive_lock(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = path.open("a+b")
    try:
        if os.fstat(handle.fileno()).st_size == 0:
            handle.write(b" ")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError("another overnight coordinator holds the operating-system lock") from error
        yield
    finally:
        handle.close()  # Windows releases byte-range locks when the handle closes.


@contextmanager
def keep_awake(set_state=None, notify=None):
    """Request system wakefulness only for this main thread's lifetime; leave display/power policy alone."""
    continuous, system_required = 0x80000000, 0x00000001
    if set_state is None:
        if os.name != "nt":
            raise RuntimeError("keep-awake requests require Windows")
        import ctypes
        from ctypes import wintypes
        set_state = ctypes.WinDLL("kernel32", use_last_error=True).SetThreadExecutionState
        set_state.argtypes = [wintypes.DWORD]
        set_state.restype = wintypes.DWORD
    if not set_state(continuous | system_required):
        raise RuntimeError("Windows refused the process-scoped keep-awake request")
    try:
        if notify:
            notify("ES_CONTINUOUS|ES_SYSTEM_REQUIRED acquired; display/power policy unchanged")
        yield
    finally:
        restored = set_state(continuous)
        if notify:
            notify("ES_CONTINUOUS restored" if restored else "wake request release failed; process exit releases it")


def ignored(relative):
    return (relative == LIVE or relative.startswith(LIVE + "/") or relative == LIVE_NOTE
            or (relative.startswith("docs/notes/.overnight_pipeline.md.") and relative.endswith(".tmp")))


def inventory(root):
    """Metadata snapshot with explicit exclusions; never follow directory reparse points."""
    root = Path(root).resolve()
    files, exclusions = {}, []
    for directory, directories, names in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in list(directories):
            path = current / name
            relative = path.relative_to(root).as_posix()
            junction = path.is_junction() if hasattr(path, "is_junction") else False
            if ignored(relative) or junction or path.is_symlink():
                directories.remove(name)
                exclusions.append({"path": relative, "reason": "live coordinator files" if ignored(relative)
                                   else "directory link excluded by /XJ policy", "directory": True})
        for name in names:
            path = current / name
            relative = path.relative_to(root).as_posix()
            if ignored(relative) or path.is_symlink():
                exclusions.append({"path": relative, "reason": "live coordinator documentation" if ignored(relative)
                                   else "file symlink (recorded, not dereferenced)", "directory": False})
                continue
            info = path.stat()
            files[relative] = {"bytes": info.st_size, "mtime_ns": info.st_mtime_ns}
    return {"files": dict(sorted(files.items())), "exclusions": sorted(exclusions, key=lambda item: item["path"])}


def media_signature(root):
    prefixes = ("templates/game/assets/video", "templates/game/out/native/video_hd",
                "templates/game/out/native/textures/source", "templates/game/out/native/textures/dump",
                "templates/game/out/native/textures/replace", "upscale_tools")
    records = {}
    for prefix in prefixes:
        for path in (root / prefix).rglob("*"):
            if path.is_file() and not path.is_symlink() and path.suffix.lower() not in (".lock", ".part", ".tmp"):
                info = path.stat()
                records[path.relative_to(root).as_posix()] = [info.st_size, info.st_mtime_ns]
    for relative in ("tools/texture_pack/upscale.py", "tools/texture_pack/audit.py", "tools/video/upscale_movies.py",
                     "templates/game/out/native/textures/upscale_passes.json",
                     "templates/game/out/native/textures/upscale_state.json",
                     "templates/game/out/native/textures/source_index.json",
                     "templates/game/out/native/textures/vinyl_hashes.txt"):
        path = root / relative
        if path.exists():
            info = path.stat()
            records[relative] = [info.st_size, info.st_mtime_ns]
    return hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()


def validate_media_reports(texture, video, pass_id=PASS_ID):
    counts = texture.get("counts", {})
    if not (texture.get("complete") is True and texture.get("model") == "ultrasharp-4x"
            and texture.get("pass_id") == pass_id and counts.get("eligible", 0) > 0
            and counts.get("complete_named_pass") == counts.get("eligible")
            and not any(texture.get("issues", {}).values())):
        raise RuntimeError("fresh texture pass has not passed its checksum/integrity audit")
    rows = video.get("details", [])
    if not (video.get("mode") == "audit" and video.get("requested_model") == "realesrgan-x4plus"
            and video.get("covers_all_sources") is True and video.get("movies") == EXPECTED_MOVIES
            and video.get("all_source_movies_validated") is True
            and video.get("all_requested_model_validated") is True and video.get("failed") == 0
            and len(rows) == EXPECTED_MOVIES and len({row.get("movie") for row in rows}) == EXPECTED_MOVIES
            and all(row.get("status") == "verified" and row.get("output_validated") is True
                    and row.get("recorded_model") == "realesrgan-x4plus" for row in rows)):
        raise RuntimeError("all 106 original movies have not passed the requested-model decode audit")


def safe_backup_destination(root, destination):
    root, destination = Path(root).resolve(), Path(destination).resolve()
    if root == destination or root in destination.parents or destination in root.parents:
        raise ValueError("backup destination must be outside and separate from the project")
    return destination


def robocopy_command(root, destination, snapshot):
    safe_backup_destination(root, destination)
    command = ["robocopy", str(root), str(destination), "/E", "/COPY:DAT", "/DCOPY:DAT", "/XJ",
               "/R:1", "/W:1", "/MT:8", "/NP", "/NFL", "/NDL"]
    directories = [str(root / item["path"]) for item in snapshot["exclusions"] if item["directory"]]
    files = [str(root / item["path"]) for item in snapshot["exclusions"] if not item["directory"]]
    if directories:
        command += ["/XD", *directories]
    command += ["/XF", *files, ".overnight_pipeline.md.*.tmp"]
    return command


def verify_backup(root, destination, snapshot, manifest_path, progress=None):
    """Verify every included file; resume hashing only when both saved file identities remain unchanged."""
    root, destination = Path(root).resolve(), safe_backup_destination(root, destination)
    manifest_path = Path(manifest_path)
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    if manifest and (manifest.get("source") != str(root) or manifest.get("destination") != str(destination)):
        raise RuntimeError("backup manifest belongs to a different source/destination")
    manifest.update(version=1, source=str(root), destination=str(destination), verified=False,
                    exclusions=snapshot["exclusions"])
    verified = manifest.setdefault("files", {})
    copied = inventory(destination)["files"]
    if set(copied) != set(snapshot["files"]):
        raise RuntimeError("backup relative file list differs from the source snapshot")
    total = len(snapshot["files"])
    for number, (relative, before) in enumerate(snapshot["files"].items(), 1):
        source, target = root / relative, destination / relative
        now, other = source.stat(), target.stat()
        if now.st_size != before["bytes"] or now.st_mtime_ns != before["mtime_ns"]:
            raise RuntimeError(f"source changed during backup: {relative}")
        if other.st_size != before["bytes"]:
            raise RuntimeError(f"backup file length differs: {relative}")
        identities = {"bytes": now.st_size, "source_mtime_ns": now.st_mtime_ns,
                      "backup_mtime_ns": other.st_mtime_ns}
        old = verified.get(relative, {})
        if not (all(old.get(key) == value for key, value in identities.items())
                and old.get("source_sha256") and old.get("source_sha256") == old.get("backup_sha256")):
            source_hash, target_hash = sha256(source), sha256(target)
            if source_hash != target_hash:
                raise RuntimeError(f"backup SHA-256 mismatch: {relative}")
            verified[relative] = dict(identities, source_sha256=source_hash, backup_sha256=target_hash)
        if number % 128 == 0 or number == total:
            manifest["checked_files"] = number
            manifest["updated_at"] = utc()
            atomic_json(manifest_path, manifest)
            if progress:
                progress(number, total)
    after = inventory(root)
    if after != snapshot:
        raise RuntimeError("source inventory changed while copying/hashing; no backup gate was issued")
    if inventory(destination)["files"] != copied:
        raise RuntimeError("backup inventory changed while hashing")
    manifest["files"] = {relative: verified[relative] for relative in snapshot["files"]}
    manifest.update(verified=True, verified_at=utc(), file_count=total,
                    byte_count=sum(value["bytes"] for value in snapshot["files"].values()))
    atomic_json(manifest_path, manifest)
    return manifest


def verify_gate(marker_path, root, signature, require_source_snapshot=True):
    marker = json.loads(Path(marker_path).read_text(encoding="utf-8"))
    if not (marker.get("status") == "verified" and marker.get("source") == str(root.resolve())
            and marker.get("media_signature") == signature):
        raise RuntimeError("verified full-backup gate is absent or belongs to different media")
    manifest_path = Path(marker["manifest"])
    if marker.get("manifest_sha256") != sha256(manifest_path):
        raise RuntimeError("backup manifest changed after verification")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not manifest.get("verified") or len(manifest.get("files", {})) != manifest.get("file_count"):
        raise RuntimeError("backup verification manifest is incomplete")
    if any(not value.get("source_sha256") or value.get("source_sha256") != value.get("backup_sha256")
           for value in manifest["files"].values()):
        raise RuntimeError("backup manifest contains an unverified file")
    destination = safe_backup_destination(root, marker["destination"])
    for relative, value in manifest["files"].items():
        target = destination / relative
        info = target.stat()
        if info.st_size != value["bytes"] or info.st_mtime_ns != value["backup_mtime_ns"]:
            raise RuntimeError(f"verified backup file changed: {relative}")
    if require_source_snapshot:
        current = inventory(root)["files"]
        expected = {relative: {"bytes": value["bytes"], "mtime_ns": value["source_mtime_ns"]}
                    for relative, value in manifest["files"].items()}
        if current != expected:
            raise RuntimeError("project changed after the verified snapshot; a fresh backup is required")
    return marker


def active_workers():
    if os.name != "nt":
        raise RuntimeError("live orchestration requires Windows process inventory")
    query = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -in "
             "@('python.exe','pythonw.exe','realesrgan-ncnn-vulkan.exe','mc3.exe') } | "
             "Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress")
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", query],
                            capture_output=True, text=True, check=True)
    values = json.loads(result.stdout) if result.stdout.strip() else []
    values = values if isinstance(values, list) else [values]
    return [row for row in values if row["ProcessId"] != os.getpid()
            and (row["Name"].lower() in ("mc3.exe", "realesrgan-ncnn-vulkan.exe")
                 or any(name in (row.get("CommandLine") or "").lower().replace(chr(92), "/")
                        for name in ("tools/video/upscale_movies.py", "tools/texture_pack/upscale.py")))]


class Paused(Exception):
    pass


class Coordinator:
    def __init__(self, root, backup_root, python=sys.executable, pass_id=PASS_ID):
        self.root = Path(root).resolve()
        self.backup_root = Path(backup_root).resolve()
        self.python, self.pass_id = python, pass_id
        self.work = self.root / ".local/overnight_pipeline"
        self.live = self.root / LIVE
        self.state_path = self.live / "state.json"
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {"version": 1}
        self.stop_file = self.root / ".local/stop-overnight-pipeline"
        self.texture_stop = self.root / ".local/stop-texture-upscale"
        self.video_stop = self.root / ".local/stop-video-upscale"
        self.texture_report = self.work / "texture-audit.json"
        self.video_report = self.work / "video-audit.json"

    def checkpoint(self, phase, **values):
        if phase not in ("failed", "paused"):
            self.state.pop("reason", None)
        self.state.update(phase=phase, updated_at=utc(), pid=os.getpid(), **values)
        atomic_json(self.state_path, self.state)
        note = ("# Overnight pipeline checkpoint\n\nGenerated by `tools/maintenance/overnight_pipeline.py`.\n\n"
                f"Updated UTC: {self.state['updated_at']}\n\nCurrent phase: **{phase}**\n\n"
                f"State: `{self.state_path}`\n\n"
                "Order: wait for the game and previous media workers to exit; fresh original-derived UltraSharp pass; "
                "all106 x4plus movies; both complete audits; full project copy and all-file SHA-256 verification; "
                "verified backup gate; offline starter PBR regeneration. Runtime PBR is unchanged.\n\n"
                f"Current details:\n\n```json\n{json.dumps(self.state, indent=2)}\n```\n\n"
                "Stop gracefully: create `.local/stop-overnight-pipeline`. The current GPU batch/movie completes first. "
                "Remove that flag and the child stop flags named in state to resume the same command.\n\n"
                "Backup exclusions are only the live coordinator directory, this generated checkpoint document and "
                "its publication temporaries, plus explicitly listed file/directory links excluded without dereferencing. "
                "The external verification manifest records every exclusion and every included file hash.\n")
        atomic_text(self.root / LIVE_NOTE, note)

    def check_stop(self):
        if self.stop_file.exists():
            raise Paused("coordinator stop file exists")

    def wait_idle(self):
        while True:
            self.check_stop()
            workers = active_workers()
            if not workers:
                return
            self.checkpoint("waiting-for-game-and-existing-media-workers", waiting=workers)
            time.sleep(30)

    def command(self, phase, args, success=(0,), child_stop=None):
        self.check_stop()
        if child_stop and child_stop.exists():
            raise Paused(f"child stop flag already exists: {child_stop}")
        self.checkpoint(phase, command=args, child_stop=str(child_stop) if child_stop else None)
        self.live.mkdir(parents=True, exist_ok=True)
        with (self.live / (phase + ".log")).open("a", encoding="utf-8") as log:
            log.write(f"\n{utc()} {subprocess.list2cmdline(args)}\n")
            log.flush()
            with subprocess.Popen(args, cwd=self.root, stdout=log, stderr=subprocess.STDOUT,
                                  creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as process:
                self.checkpoint(phase, child_pid=process.pid)
                heartbeat = time.monotonic()
                while process.poll() is None:
                    if self.stop_file.exists() and child_stop:
                        child_stop.parent.mkdir(parents=True, exist_ok=True)
                        child_stop.touch(exist_ok=True)
                    if time.monotonic() - heartbeat >= 60:
                        details = {}
                        progress_path = self.work / "video-progress.json"
                        if phase == "videos" and progress_path.exists():
                            progress = json.loads(progress_path.read_text(encoding="utf-8"))
                            details = {key: progress.get(key) for key in ("verified", "movies", "failed", "remaining_frames")}
                        if phase == "textures":
                            progress_path = self.root / "templates/game/out/native/textures/upscale_passes.json"
                            if progress_path.exists():
                                progress = json.loads(progress_path.read_text(encoding="utf-8"))
                                details["named_pass_completed"] = len(progress.get("passes", {}).get(self.pass_id, {}).get("completed", {}))
                        self.checkpoint(phase, progress=details)
                        heartbeat = time.monotonic()
                    time.sleep(5)
                code = process.returncode
        self.checkpoint(phase + "-exited", child_exit_code=code, child_pid=None)
        self.check_stop()
        if code == 2 and child_stop:
            raise Paused(f"child stopped at a safe boundary; inspect {child_stop}")
        if code not in success:
            raise RuntimeError(f"{phase} exited {code}; see {self.live / (phase + '.log')}")

    def media_gate(self):
        signature = media_signature(self.root)
        saved = self.state.get("media_gate", {})
        if (saved.get("signature") == signature and self.texture_report.exists() and self.video_report.exists()
                and saved.get("texture_report_sha256") == sha256(self.texture_report)
                and saved.get("video_report_sha256") == sha256(self.video_report)):
            validate_media_reports(json.loads(self.texture_report.read_text()),
                                   json.loads(self.video_report.read_text()), self.pass_id)
            self.checkpoint("using-unchanged-verified-media-gate")
            return signature
        self.wait_idle()
        self.command("textures", [self.python, "tools/texture_pack/upscale.py", "--model", "ultrasharp-4x",
                                  "--pass-id", self.pass_id, "--batch", "16", "--stop-file", str(self.texture_stop)],
                     child_stop=self.texture_stop)
        self.wait_idle()
        self.command("videos", [self.python, "tools/video/upscale_movies.py", "--model", "realesrgan-x4plus",
                                "--stop-file", str(self.video_stop), "--report", str(self.work / "video-progress.json")],
                     child_stop=self.video_stop)
        self.wait_idle()
        before = media_signature(self.root)
        self.command("texture-audit", [self.python, "tools/texture_pack/audit.py", "--model", "ultrasharp-4x",
                                       "--pass-id", self.pass_id, "--output", str(self.texture_report)])
        self.command("video-audit", [self.python, "tools/video/upscale_movies.py", "--audit-only", "--model",
                                     "realesrgan-x4plus", "--report", str(self.video_report)])
        validate_media_reports(json.loads(self.texture_report.read_text()), json.loads(self.video_report.read_text()), self.pass_id)
        signature = media_signature(self.root)
        if signature != before:
            raise RuntimeError("media changed during the final audits; backup gate remains closed")
        self.checkpoint("media-verified", media_gate={"signature": signature, "verified_at": utc(),
                        "texture_report_sha256": sha256(self.texture_report),
                        "video_report_sha256": sha256(self.video_report)})
        return signature

    def backup(self, signature):
        self.wait_idle()
        existing = self.state.get("backup_marker")
        if existing and Path(existing).exists():
            try:
                verify_gate(existing, self.root, signature)
                return Path(existing)
            except (OSError, ValueError, RuntimeError):
                # Preserve the earlier backup; a changed project gets a new complete snapshot before PBR.
                for key in ("backup_marker", "backup_destination", "backup_manifest"):
                    self.state.pop(key, None)
        if not self.backup_root.is_dir():
            raise RuntimeError(f"backup root is unavailable: {self.backup_root}")
        destination = Path(self.state.get("backup_destination") or
                           self.backup_root / ("MidnightClub3-Port_PrePBR_" + datetime.now().strftime("%Y-%m-%d_%H%M%S_%f")))
        safe_backup_destination(self.root, destination)
        self.checkpoint("backup-inventory", backup_destination=str(destination))
        snapshot = inventory(self.root)
        required = sum(value["bytes"] for value in snapshot["files"].values())
        existing_bytes = sum(value["bytes"] for value in inventory(destination)["files"].values()) if destination.exists() else 0
        if shutil.disk_usage(self.backup_root).free < max(0, required - existing_bytes) + 1024 ** 3:
            raise RuntimeError("insufficient free space for full project backup plus 1 GiB margin")
        self.command("backup-copy", robocopy_command(self.root, destination, snapshot), success=tuple(range(8)))
        manifest_path = self.backup_root / "verification" / (destination.name + ".manifest.json")
        self.checkpoint("backup-all-file-sha256", backup_manifest=str(manifest_path), source_files=len(snapshot["files"]))
        manifest = verify_backup(self.root, destination, snapshot, manifest_path,
                                 lambda checked, total: self.checkpoint("backup-all-file-sha256", hashed_files=checked, source_files=total))
        if signature != media_signature(self.root):
            raise RuntimeError("media changed after audits; no backup gate issued")
        marker_path = manifest_path.with_name(destination.name + ".verified.json")
        marker = {"version": 1, "status": "verified", "source": str(self.root), "destination": str(destination),
                  "manifest": str(manifest_path), "manifest_sha256": sha256(manifest_path),
                  "media_signature": signature, "media_gate": self.state["media_gate"],
                  "files": manifest["file_count"], "bytes": manifest["byte_count"], "verified_at": utc()}
        atomic_json(marker_path, marker)
        self.checkpoint("full-backup-verified", backup_marker=str(marker_path))
        return marker_path

    def run(self):
        def power_event(message):
            self.checkpoint(self.state.get("phase", "starting"), power_request=message)
            with (self.live / "power.log").open("a", encoding="utf-8") as log:
                log.write(utc() + " " + message + "\n")

        with exclusive_lock(self.live / "coordinator.lock"), keep_awake(notify=power_event):
            self.checkpoint("starting", root=str(self.root), texture_pass_id=self.pass_id,
                            stop_file=str(self.stop_file))
            try:
                previous = self.state.get("pbr_complete", {})
                old_marker = self.state.get("backup_marker")
                old_pbr = self.root / "templates/game/out/native/textures/pbr/manifest.json"
                if (old_marker and Path(old_marker).exists() and old_pbr.exists()
                        and previous.get("backup_marker_sha256") == sha256(old_marker)
                        and previous.get("manifest_sha256") == sha256(old_pbr)):
                    verify_gate(old_marker, self.root, media_signature(self.root), require_source_snapshot=False)
                    self.checkpoint("complete", runtime_pbr_changed=False)
                    return 0
                signature = self.media_gate()
                marker = self.backup(signature)
                verify_gate(marker, self.root, media_signature(self.root))
                self.check_stop()
                pbr_manifest = self.root / "templates/game/out/native/textures/pbr/manifest.json"
                previous_pbr = self.state.get("pbr_complete", {})
                if not (previous_pbr.get("backup_marker_sha256") == sha256(marker) and pbr_manifest.exists()
                        and previous_pbr.get("manifest_sha256") == sha256(pbr_manifest)):
                    self.command("pbr-offline-starter", [self.python, "tools/pbr/generate.py", "--set", "tools/pbr/sets/starter50.json",
                                                         "--method", "ensemble", "--device", "cpu", "--input", "source",
                                                         "--max-size", "1024", "--work-scale", "4", "--threads", "4", "--force"])
                    if not pbr_manifest.exists():
                        raise RuntimeError("PBR generator returned success without publishing a manifest")
                    self.state["pbr_complete"] = {"backup_marker_sha256": sha256(marker),
                                                  "manifest_sha256": sha256(pbr_manifest), "finished_at": utc()}
                self.checkpoint("complete", runtime_pbr_changed=False)
                return 0
            except Paused as error:
                self.checkpoint("paused", reason=str(error))
                return 2
            except Exception as error:
                self.checkpoint("failed", reason=str(error))
                raise


def dry_run(root, backup_root):
    snapshot = inventory(root)
    textures = root / "templates/game/out/native/textures"
    assets = root / "templates/game/assets/video"
    return {"mode": "dry-run", "root": str(root), "backup_root": str(backup_root),
            "files": len(snapshot["files"]), "bytes": sum(value["bytes"] for value in snapshot["files"].values()),
            "exclusions": snapshot["exclusions"], "original_movies": len(list(assets.rglob("*.bik"))),
            "original_textures": len(list((textures / "source").glob("*.png"))),
            "active_replacements": len(list((textures / "replace").glob("*.png"))),
            "texture_pass_id": PASS_ID, "actions_started": False,
            "order": ["wait-existing-workers", "fresh-textures", "x4plus-movies", "both-media-audits",
                      "full-backup-copy", "every-file-SHA256-verification", "backup-marker", "offline-PBR-starter"],
            "stop_flags": [str(root / ".local" / name) for name in
                           ("stop-overnight-pipeline", "stop-texture-upscale", "stop-video-upscale")]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--run", action="store_true", help="execute the authorized durable workflow")
    action.add_argument("--dry-run", action="store_true", help="read-only inventory (the default)")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--backup-root", type=Path, default=Path(r"G:\MC3_Backups"))
    parser.add_argument("--python", default=sys.executable)
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        parser.error("project root does not exist")
    safe_backup_destination(root, args.backup_root)
    if not args.run:
        print(json.dumps(dry_run(root, args.backup_root.resolve()), indent=2))
        return 0
    if os.name != "nt":
        parser.error("--run requires Windows")
    return Coordinator(root, args.backup_root, args.python).run()


if __name__ == "__main__":
    raise SystemExit(main())
