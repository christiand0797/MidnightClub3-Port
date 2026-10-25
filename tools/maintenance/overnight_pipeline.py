#!/usr/bin/env python3
"""Durable, opt-in MC3 media -> verified full backup -> offline PBR coordinator.

Inventory only (default): py -3.12 tools/maintenance/overnight_pipeline.py --dry-run
Execute authorized work: py -3.12 tools/maintenance/overnight_pipeline.py --run

MC3, Fortnite and Black Ops III defer GPU launch. Textures yield at batch boundaries; video pauses the same
child between NCNN invocations of at most eight frames, then resumes after the game closes. The current
invocation finishes; there is no wall-clock pause guarantee. No game process or physical input is stopped.
Create .local/stop-overnight-pipeline to cancel. Video discards only its in-flight temporary movie; completed
published movies stay intact. User cancellation wins over game resume and never clears user stop flags.
PBR never runs unless both media audits and an all-file SHA-256 backup verify.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
PASS_ID = "2026-10-22-originals-pass"
EXPECTED_MOVIES = 106
MEDIA_PROVENANCE_POLICY = "original-bink-and-model-weights-v1"
# A human session in another installed game gets the same cooperative GPU scheduling as MC3.
# Keep this explicit; launchers and unrelated desktop apps are not reasons to stop CPU work.
INTERACTIVE_GPU_GAMES = frozenset(("mc3.exe", "fortniteclient-win64-shipping.exe", "blackops3.exe"))
LIVE = ".local/overnight_pipeline/live"
LIVE_NOTE = "docs/notes/overnight_pipeline.md"
TEXTURES = "templates/game/out/native/textures"
ACTIVE_TEXTURE_TRANSACTION = ".local/reviewed_texture_install/active.json"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
TEXTURE_NAME = re.compile(r"[0-9A-F]{16}\.png\Z")


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
    root = Path(root).resolve()
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
                     "tools/texture_pack/reviewed_fixes.py", "tools/texture_pack/cpu_x4plus.py",
                     "tools/texture_pack/loading_garage_join.py", "tools/maintenance/overnight_pipeline.py",
                     "tools/texture_pack/install_reviewed.py", "tools/texture_pack/apply_reviewed.py",
                     "templates/game/out/native/textures/upscale_passes.json",
                     "templates/game/out/native/textures/upscale_state.json",
                     "templates/game/out/native/textures/source_index.json",
                     "templates/game/out/native/textures/vinyl_hashes.txt"):
        path = root / relative
        if path.exists():
            info = path.stat()
            records[relative] = [info.st_size, info.st_mtime_ns]
    ledger = root / TEXTURES / "reviewed_fixes.json"
    if ledger.exists() or ledger.is_symlink():
        # Authenticate the whole small correction graph, including archives and review/inference proofs.
        # Hashes also catch equal-size edits with preserved mtimes, which ordinary media inventory cannot.
        try:
            if root not in ledger.resolve().parents:
                raise ValueError("ledger escapes project through a link")
            content = ledger.read_bytes()
            document = json.loads(content)
            dependencies = document["dependencies"]
            if not isinstance(dependencies, dict) or not dependencies:
                raise ValueError("empty/non-object dependencies")
            records[ledger.relative_to(root).as_posix()] = hashlib.sha256(content).hexdigest()
            for relative, expected in dependencies.items():
                if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative
                        or any(part in ("", ".", "..") for part in relative.split("/"))
                        or PurePosixPath(relative).is_absolute() or not isinstance(expected, str)
                        or not SHA256.fullmatch(expected)):
                    raise ValueError("invalid dependency path/hash")
                path = (root / relative).resolve()
                if root not in path.parents or not path.is_file() or path.is_symlink():
                    raise ValueError(f"missing/escaping dependency: {relative}")
                digest = sha256(path)
                if digest != expected:
                    raise ValueError(f"changed dependency: {relative}")
                info = path.stat()
                records[relative] = [info.st_size, info.st_mtime_ns, digest]
            if ledger.read_bytes() != content:
                raise ValueError("ledger changed during signature")
        except (OSError, ValueError, TypeError, KeyError) as error:
            raise RuntimeError(f"reviewed texture dependency signature failed; preserve corrections: {error}") from error
    return hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()


def validate_texture_report(texture, pass_id=PASS_ID):
    if not isinstance(texture, dict) or not isinstance(texture.get("counts"), dict) or not isinstance(texture.get("issues", {}), dict):
        raise RuntimeError("texture audit report is malformed")
    counts = texture.get("counts", {})
    if texture.get("format") == "mc3-reviewed-texture-audit-v1":
        entries = texture.get("reviewed_fixes", {})
        base = texture.get("base_audit", {})
        if not isinstance(base, dict) or not isinstance(base.get("counts"), dict) or not isinstance(base.get("issues"), dict):
            raise RuntimeError("reviewed base texture audit report is malformed")
        base_counts = base.get("counts", {})
        base_issues = base.get("issues", {})
        integers = ("eligible", "base_named_pass_verified", "current_original_named_pass",
                    "reviewed_fixes", "accounted_changed_pass_records")
        valid = (all(type(counts.get(key)) is int and counts[key] >= 0 for key in integers)
                 and counts.get("eligible", 0) > 0 and 1 <= counts.get("reviewed_fixes", 0) <= 8
                 and texture.get("complete") is True and texture.get("base_complete") is True
                 and texture.get("base_model") == "ultrasharp-4x" and texture.get("base_pass_id") == pass_id
                 and texture.get("current_model") == "mixed-reviewed"
                 and isinstance(texture.get("ledger_sha256"), str) and SHA256.fullmatch(texture["ledger_sha256"])
                 and not any(texture.get("issues", {}).values()) and isinstance(entries, dict)
                 and all(isinstance(name, str) for name in entries)
                 and len(entries) == counts["reviewed_fixes"]
                 and counts["base_named_pass_verified"] == counts["eligible"]
                 and counts["accounted_changed_pass_records"] == counts["reviewed_fixes"]
                 and counts["current_original_named_pass"] + counts["reviewed_fixes"] == counts["eligible"]
                 and base.get("model") == "ultrasharp-4x" and base.get("pass_id") == pass_id
                 and base_counts.get("eligible") == counts["eligible"]
                 and base_counts.get("complete_named_pass") == counts["current_original_named_pass"]
                 and isinstance(base_issues.get("missing_or_changed_pass_record"), list)
                 and all(isinstance(name, str) for name in base_issues["missing_or_changed_pass_record"])
                 and sorted(base_issues["missing_or_changed_pass_record"]) == sorted(entries)
                 and not any(value for key, value in base_issues.items() if key != "missing_or_changed_pass_record")
                 and all(isinstance(name, str) and TEXTURE_NAME.fullmatch(name) and isinstance(entry, dict)
                         and entry.get("recipe") == "garage-joined-cpu-x4plus-v1"
                         and entry.get("current_backend") == "pytorch-cpu-x4plus-v1"
                         and all(isinstance(entry.get(key), str) and SHA256.fullmatch(entry[key])
                                 for key in ("model_sha256", "source_sha256", "before_sha256", "after_sha256", "alpha_sha256"))
                         and entry["before_sha256"] != entry["after_sha256"]
                         and all(entry.get(key) for key in ("base_archive", "review_proof", "candidate_proofs"))
                         for name, entry in entries.items()))
    else:
        valid = (texture.get("complete") is True and texture.get("model") == "ultrasharp-4x"
            and texture.get("pass_id") == pass_id and counts.get("eligible", 0) > 0
            and counts.get("complete_named_pass") == counts.get("eligible")
            and not any(texture.get("issues", {}).values()))
    if not valid:
        raise RuntimeError("fresh texture pass has not passed its checksum/integrity audit")


def validate_media_reports(texture, video, pass_id=PASS_ID):
    validate_texture_report(texture, pass_id)
    rows = video.get("details", [])
    if not (video.get("mode") == "audit" and video.get("requested_model") == "realesrgan-x4plus"
            and video.get("require_original_provenance") is True
            and video.get("all_original_source_and_weights_verified") is True
            and video.get("covers_all_sources") is True and video.get("movies") == EXPECTED_MOVIES
            and video.get("all_source_movies_validated") is True
            and video.get("all_requested_model_validated") is True and video.get("failed") == 0
            and len(rows) == EXPECTED_MOVIES and len({row.get("movie") for row in rows}) == EXPECTED_MOVIES
            and all(row.get("status") == "verified" and row.get("output_validated") is True
                    and row.get("recorded_model") == "realesrgan-x4plus"
                    and row.get("historical_source_and_weights_verified") is True
                    and row.get("origin") == "original-bink" for row in rows)):
        raise RuntimeError("all 106 original movies have not passed the strict source/weights provenance and decode audit")


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
             "@('python.exe','pythonw.exe','realesrgan-ncnn-vulkan.exe','mc3.exe',"
             "'FortniteClient-Win64-Shipping.exe','BlackOps3.exe') } | "
             "Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress")
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", query],
                            capture_output=True, text=True, check=True)
    values = json.loads(result.stdout) if result.stdout.strip() else []
    values = values if isinstance(values, list) else [values]
    return [row for row in values if row["ProcessId"] != os.getpid()
            and (row["Name"].lower() in INTERACTIVE_GPU_GAMES | {"realesrgan-ncnn-vulkan.exe"}
                 or any(name in (row.get("CommandLine") or "").lower().replace(chr(92), "/")
                        for name in ("tools/video/upscale_movies.py", "tools/texture_pack/upscale.py")))]


class Paused(Exception):
    pass


class OwnedChildStop:
    """A private cooperative request; shared user stop flags are never written or removed."""
    def __init__(self, directory, *, purpose="stop"):
        if purpose not in ("stop", "pause"):
            raise ValueError("unknown private child marker purpose")
        self.purpose = purpose
        token = uuid.uuid4().hex
        self.path = directory / ("child-" + purpose + "-" + token)
        self.content = "overnight-pipeline:" + token + "\n"
        self.created = False

    def unchanged(self):
        try:
            return (not self.path.is_symlink()
                    and not (hasattr(self.path, "is_junction") and self.path.is_junction())
                    and self.path.is_file() and self.path.read_text(encoding="utf-8") == self.content)
        except (OSError, UnicodeError):
            return False

    def request(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with self.path.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(self.content)
            self.created = True
        except FileExistsError:
            if not self.created or not self.unchanged():
                raise Paused(f"private child {self.purpose} marker was replaced; preserved: {self.path}")

    def clear(self):
        if self.created and os.path.lexists(self.path):
            if not self.unchanged():
                raise Paused(f"private child {self.purpose} marker was replaced; preserved: {self.path}")
            self.path.unlink()
            self.created = False


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
                "MC3, Fortnite and Black Ops III defer GPU launch. Texture workers yield at batch boundaries. "
                "Video keeps the same child and in-flight movie paused between NCNN invocations of at most eight frames, "
                "then resumes after the game closes. The current invocation finishes; there is no wall-clock pause guarantee. "
                "No game process or physical input is stopped.\n\n"
                "Stop gracefully: create `.local/stop-overnight-pipeline`, or the texture/video user stop flag. "
                "The current texture batch or video inference invocation finishes. Video then discards only its in-flight "
                "temporary movie and retains every published movie. User cancellation takes priority over game resume, "
                "even if its flag is removed while the child drains. Remove user stop flags yourself and rerun "
                "the same command to resume. Only this coordinator's unique private child markers are cleared.\n\n"
                "Backup exclusions are only the live coordinator directory, this generated checkpoint document and "
                "its publication temporaries, plus explicitly listed file/directory links excluded without dereferencing. "
                "The external verification manifest records every exclusion and every included file hash.\n")
        atomic_text(self.root / LIVE_NOTE, note)

    def stop_reason(self, child_stop=None):
        if self.stop_file.exists():
            return "coordinator stop file exists"
        # A crashed installer releases its OS lock. Its durable registration must still stop base
        # regeneration, cached media/backup reuse and PBR until its exact transaction is recovered.
        # Do not parse, clear or trust the marker: even malformed/foreign bytes remain a closed gate.
        if os.path.lexists(self.root / ACTIVE_TEXTURE_TRANSACTION):
            return "unfinished reviewed texture transaction; preserve and recover " + str(self.root / ACTIVE_TEXTURE_TRANSACTION)
        if child_stop and child_stop.exists():
            return f"child stop flag already exists: {child_stop}"

    def check_stop(self, child_stop=None):
        reason = self.stop_reason(child_stop)
        if reason:
            raise Paused(reason)

    def wait_idle(self, child_stop=None):
        while True:
            self.check_stop(child_stop)
            workers = active_workers()
            if not workers:
                self.state.pop("waiting", None)
                return
            self.checkpoint("waiting-for-game-and-existing-media-workers", waiting=workers)
            time.sleep(30)

    def command(self, phase, args, success=(0,), child_stop=None, pause_for_games=False):
        self.check_stop(child_stop)
        if pause_for_games and (phase != "videos" or not child_stop
                                or "--pause-file" in args or "--pause-owner-pid" in args):
            raise ValueError("game-pause protocol requires a video child with its own private pause argument")
        if child_stop:
            if args.count("--stop-file") != 1:
                raise ValueError("cooperative child requires one explicit --stop-file")
            stop_index = args.index("--stop-file") + 1
            if stop_index >= len(args) or Path(args[stop_index]).resolve() != child_stop.resolve():
                raise ValueError("cooperative child stop argument differs from its user cancellation flag")
        while True:
            request = OwnedChildStop(self.live) if child_stop else None
            pause = OwnedChildStop(self.live, purpose="pause") if pause_for_games else None
            command = list(args)
            if request:
                self.wait_idle(child_stop)
                command[stop_index] = str(request.path)
            if pause:
                command += ["--pause-file", str(pause.path), "--pause-owner-pid", str(os.getpid())]
            code, game_pause = self.command_once(phase, command, child_stop, request, pause)
            if code == 2 and request and game_pause and pause is None:
                self.checkpoint(phase + "-waiting-for-game", resume_phase=phase)
                self.wait_idle(child_stop)
                continue  # Both children already resume from their durable completed-batch/movie records.
            if code == 2 and child_stop:
                raise Paused(f"child stopped without a game-pause request; inspect {self.live / (phase + '.log')}")
            if code not in success:
                raise RuntimeError(f"{phase} exited {code}; see {self.live / (phase + '.log')}")
            return

    def command_once(self, phase, args, child_stop, request, pause=None):
        self.checkpoint(phase, command=args, child_stop=str(request.path) if request else None,
                        child_pause=str(pause.path) if pause else None,
                        user_child_stop=str(child_stop) if child_stop else None, resume_phase=None)
        self.live.mkdir(parents=True, exist_ok=True)
        cancelled, game_pause = None, False
        def cancel(reason):
            nonlocal cancelled
            cancelled = cancelled or reason
            if request:
                try:
                    request.request()
                except Paused:
                    # A foreign existing stop still makes the worker stop; never overwrite it.
                    pass
        try:
            with (self.live / (phase + ".log")).open("a", encoding="utf-8") as log:
                log.write(f"\n{utc()} {subprocess.list2cmdline(args)}\n")
                log.flush()
                with subprocess.Popen(args, cwd=self.root, stdout=log, stderr=subprocess.STDOUT,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as process:
                    try:
                        self.checkpoint(phase, child_pid=process.pid)
                    except (Exception, KeyboardInterrupt) as error:
                        cancel("coordinator interrupted" if isinstance(error, KeyboardInterrupt)
                               else "coordinator monitoring failed: " + str(error))
                    heartbeat = time.monotonic()
                    while True:
                        try:
                            if process.poll() is not None:
                                break
                            # Latch cancellation before inspecting the game. Removing a user flag while this child
                            # drains must not silently turn that cancellation into an automatic game-pause resume.
                            cancelled = cancelled or self.stop_reason(child_stop)
                            if cancelled and request:
                                request.request()
                            elif pause:
                                if os.path.lexists(pause.path) and (not pause.created or not pause.unchanged()):
                                    raise Paused(f"private child pause marker was replaced; preserved: {pause.path}")
                                # The child is deliberately still alive. Waiting on active_workers() here
                                # would wait on that same child forever; inspect interactive games only.
                                games = [row for row in active_workers() if row["Name"].lower() in INTERACTIVE_GPU_GAMES]
                                if games:
                                    pause.request()
                                    if not game_pause:
                                        self.checkpoint(phase + "-pause-requested-for-game", waiting=games)
                                    game_pause = True
                                elif game_pause:
                                    pause.clear()  # Only the exact marker this owner created may be removed.
                                    game_pause = False
                                    self.checkpoint(phase + "-resumed-after-game", waiting=[])
                            elif request and not game_pause:
                                games = [row for row in active_workers() if row["Name"].lower() in INTERACTIVE_GPU_GAMES]
                                if games:
                                    game_pause = True
                                    request.request()
                                    self.checkpoint(phase + "-pausing-for-game", waiting=games)
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
                                current = phase + ("-stopping" if cancelled else
                                                   "-pause-requested-for-game" if game_pause and pause else
                                                   "-pausing-for-game" if game_pause else "")
                                self.checkpoint(current, progress=details)
                                heartbeat = time.monotonic()
                            time.sleep(5)
                        except (Exception, KeyboardInterrupt) as error:
                            cancel("coordinator interrupted" if isinstance(error, KeyboardInterrupt)
                                   else "coordinator monitoring failed: " + str(error))
                            try:
                                time.sleep(5)
                            except (KeyboardInterrupt, OSError):
                                pass
                    code = process.returncode
            self.checkpoint(phase + "-exited", child_exit_code=code, child_pid=None, child_stop=None, child_pause=None)
            if cancelled:
                raise Paused(cancelled)
            self.check_stop(child_stop)
            return code, game_pause
        finally:
            errors = []
            for marker in (pause, request):
                if marker:
                    try:
                        marker.clear()
                    except Paused as error:
                        errors.append(str(error))
            if errors:
                raise Paused("; ".join(errors))

    def cached_media_verified(self, signature):
        """Older model-only gates are cache misses, never permanent workflow failures."""
        saved = self.state.get("media_gate", {})
        try:
            if (saved.get("provenance_policy") == MEDIA_PROVENANCE_POLICY and saved.get("signature") == signature
                    and self.texture_report.exists() and self.video_report.exists()
                    and saved.get("texture_report_sha256") == sha256(self.texture_report)
                    and saved.get("video_report_sha256") == sha256(self.video_report)):
                validate_media_reports(json.loads(self.texture_report.read_text()),
                                       json.loads(self.video_report.read_text()), self.pass_id)
                return True
        except (OSError, ValueError, RuntimeError, TypeError, AttributeError):
            pass
        return False

    def audit_textures(self, phase="texture-audit"):
        ledger = self.root / TEXTURES / "reviewed_fixes.json"
        if ledger.is_symlink() and not ledger.exists():
            raise RuntimeError("reviewed texture ledger link is broken; preserve corrections")
        reviewed = ledger.exists()
        # The reviewed CLI never overwrites a report. Publish its successful result atomically; retain failed
        # evidence separately and leave the last good report intact. The immutable base CLI stays unchanged.
        output = self.live / ("reviewed-audit-" + uuid.uuid4().hex + ".json") if reviewed else self.texture_report
        script = "reviewed_fixes.py" if reviewed else "audit.py"
        self.command(phase, [self.python, "tools/texture_pack/" + script, "--model", "ultrasharp-4x",
                             "--pass-id", self.pass_id, "--output", str(output)])
        if reviewed:
            validate_texture_report(json.loads(output.read_text(encoding="utf-8")), self.pass_id)
            os.replace(output, self.texture_report)

    def media_gate(self):
        self.check_stop()
        signature = media_signature(self.root)
        if self.cached_media_verified(signature):
            self.checkpoint("using-unchanged-verified-media-gate")
            return signature
        self.wait_idle()
        if (self.root / TEXTURES / "reviewed_fixes.json").exists():
            # An approved correction is already a completed base+reviewed pack. A bad ledger must stop here,
            # before the original worker can regenerate an approved PNG or overwrite diagnostic evidence.
            self.audit_textures("reviewed-textures-preflight")
        else:
            self.command("textures", [self.python, "tools/texture_pack/upscale.py", "--model", "ultrasharp-4x",
                                      "--pass-id", self.pass_id, "--batch", "16", "--stop-file", str(self.texture_stop)],
                         child_stop=self.texture_stop)
        self.wait_idle()
        self.command("videos", [self.python, "tools/video/upscale_movies.py", "--model", "realesrgan-x4plus",
                                "--require-original-provenance",
                                "--stop-file", str(self.video_stop), "--report", str(self.work / "video-progress.json")],
                     child_stop=self.video_stop, pause_for_games=True)
        self.wait_idle()
        before = media_signature(self.root)
        self.audit_textures()
        self.command("video-audit", [self.python, "tools/video/upscale_movies.py", "--audit-only", "--model",
                                     "realesrgan-x4plus", "--require-original-provenance", "--report", str(self.video_report)])
        validate_media_reports(json.loads(self.texture_report.read_text()), json.loads(self.video_report.read_text()), self.pass_id)
        signature = media_signature(self.root)
        if signature != before:
            raise RuntimeError("media changed during the final audits; backup gate remains closed")
        self.checkpoint("media-verified", media_gate={"signature": signature, "verified_at": utc(),
                        "provenance_policy": MEDIA_PROVENANCE_POLICY,
                        "texture_report_sha256": sha256(self.texture_report),
                        "video_report_sha256": sha256(self.video_report)})
        return signature

    def backup(self, signature):
        self.wait_idle()
        existing = self.state.get("backup_marker")
        if existing and Path(existing).exists():
            try:
                marker = verify_gate(existing, self.root, signature)
                if marker.get("media_gate", {}).get("provenance_policy") != MEDIA_PROVENANCE_POLICY:
                    raise RuntimeError("backup gate predates strict original-movie provenance")
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
                self.check_stop()
                previous = self.state.get("pbr_complete", {})
                old_marker = self.state.get("backup_marker")
                old_pbr = self.root / "templates/game/out/native/textures/pbr/manifest.json"
                if (old_marker and Path(old_marker).exists() and old_pbr.exists()
                        and previous.get("backup_marker_sha256") == sha256(old_marker)
                        and previous.get("manifest_sha256") == sha256(old_pbr)
                        and self.cached_media_verified(media_signature(self.root))):
                    try:
                        prior = verify_gate(old_marker, self.root, media_signature(self.root), require_source_snapshot=False)
                    except (OSError, ValueError, RuntimeError, KeyError, TypeError):
                        prior = {}  # A stale completed checkpoint needs fresh gates; preserve its old artifacts.
                    if prior.get("media_gate", {}).get("provenance_policy") == MEDIA_PROVENANCE_POLICY:
                        self.checkpoint("complete", runtime_pbr_changed=False)
                        return 0
                signature = self.media_gate()
                marker = self.backup(signature)
                verified = verify_gate(marker, self.root, media_signature(self.root))
                if verified.get("media_gate", {}).get("provenance_policy") != MEDIA_PROVENANCE_POLICY:
                    raise RuntimeError("verified backup gate lacks strict original-movie provenance")
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
