"""Freeze an independent content cohort without preventing Windows atomic publication.

The default CLI is a read-only plan. --execute requires an explicit mode and NEW external G: directory.
Quiescent mode holds the existing coordinator/install/video-output locks and checks real worker inventory.
Concurrent mode refuses when the actual held-target os.replace capability probe fails.
This copies bytes and validates recorded provenance; actual decode/PTS validation remains pending.
It never imports the movie worker, adopts legacy metadata, retimes, or controls any process.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from contextvars import ContextVar
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
from datetime import datetime, timezone
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).absolute().parents[2]
ISO_SHA256 = "d58490dc8e1837e9f47716268b52ae1f1ac8578e5a5e42e061fa7f1c4b068b52"
SHA = re.compile(r"[0-9a-f]{64}\Z")
PNG = re.compile(r"[0-9A-Fa-f]{16}\.png\Z")
BLOCK = 1024 * 1024
READ_CHECK = ContextVar("freeze_read_check", default=None)


class SnapshotError(RuntimeError):
    pass


class CohortChanged(SnapshotError):
    """A publication crossed our observation; retry only our own unaccepted attempt."""


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha_bytes(value):
    return hashlib.sha256(value).hexdigest()


def absolute(path):
    # resolve() would hide a linked ancestor before we can reject it.
    return Path(os.path.abspath(path))


def safe_relative(value):
    if (not isinstance(value, str) or not value or "\\" in value
            or re.search(r'[\x00-\x1f:\\<>"|?*]', value)):
        raise SnapshotError("Unsafe relative content path")
    p = PurePosixPath(value)
    parts = value.split("/")
    if p.is_absolute() or any(not x or x in (".", "..") or x.endswith((".", " "))
                              or re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", x.split(".")[0], re.I)
                              for x in parts):
        raise SnapshotError("Unsafe relative content component")
    return value


def plain(path, *, directory=None):
    path = absolute(path)
    current = path
    while True:
        info = os.lstat(current)
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise SnapshotError(f"Linked path is forbidden: {current}")
        if current == path and directory is not None and stat.S_ISDIR(info.st_mode) != directory:
            raise SnapshotError(f"Unexpected path type: {path}")
        if current.parent == current:
            return
        current = current.parent


def walk_files(directory):
    directory = absolute(directory)
    plain(directory, directory=True)
    result = []
    pending = [directory]
    while pending:
        parent = pending.pop()
        plain(parent, directory=True)
        with os.scandir(parent) as entries:
            for entry in entries:
                info = entry.stat(follow_symlinks=False)
                linked = stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400
                if stat.S_ISDIR(info.st_mode):
                    if linked:
                        raise SnapshotError(f"Linked directory is forbidden: {entry.path}")
                    pending.append(Path(entry.path))
                else:
                    result.append(Path(entry.path))
    plain(directory, directory=True)
    return result


class _FileInfo(ctypes.Structure):
    _fields_ = [("attributes", wintypes.DWORD), ("created", wintypes.FILETIME),
                ("accessed", wintypes.FILETIME), ("written", wintypes.FILETIME),
                ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]


def windows_api():
    if os.name != "nt":
        raise SnapshotError("Shared-handle snapshots are supported only on Windows")
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    api.CreateFileW.restype = wintypes.HANDLE
    api.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(_FileInfo)]
    api.GetFileInformationByHandle.restype = wintypes.BOOL
    api.GetFileType.argtypes = [wintypes.HANDLE]
    api.GetFileType.restype = wintypes.DWORD
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    api.MoveFileExW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
    api.MoveFileExW.restype = wintypes.BOOL
    return api


def native(path):
    value = str(absolute(path))
    if value.startswith("\\\\?\\"):
        return value
    if value.startswith("\\\\"):
        return "\\\\?\\UNC\\" + value[2:]
    return "\\\\?\\" + value


class SharedReader:
    """Read-only fd, sharing read/write/delete; ownership transfers HANDLE -> CRT fd exactly once."""
    def __init__(self, path):
        import msvcrt
        self.path = absolute(path)
        self.file = None
        self.api = windows_api()
        plain(self.path.parent, directory=True)
        handle = self.api.CreateFileW(native(self.path), 0x80000000, 1 | 2 | 4, None, 3,
                                      0x00200000 | 0x08000000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        fd = None
        try:
            self._handle_info(handle)
            plain(self.path, directory=False)
            fd = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
            handle = None  # The CRT now owns it, including if fdopen fails.
            self.file = os.fdopen(fd, "rb", buffering=0)
            fd = None
        finally:
            if fd is not None:
                os.close(fd)
            if handle is not None:
                self.api.CloseHandle(handle)

    def _handle_info(self, handle):
        value = _FileInfo()
        if self.api.GetFileType(handle) != 1 or not self.api.GetFileInformationByHandle(handle, ctypes.byref(value)):
            raise SnapshotError("Source is not an identifiable disk file")
        if value.attributes & (0x10 | 0x400):
            raise SnapshotError("Directory/reparse source handle is forbidden")
        return {"volume": value.volume, "file_id": (value.index_high << 32) | value.index_low,
                "bytes": (value.size_high << 32) | value.size_low,
                "written_ticks": (value.written.dwHighDateTime << 32) | value.written.dwLowDateTime}

    def fingerprint(self):
        import msvcrt
        return self._handle_info(msvcrt.get_osfhandle(self.file.fileno()))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.file.close()


def event(hook, name, **values):
    if hook is not None:
        hook(name, **values)


def fingerprint(path):
    with SharedReader(path) as reader:
        return reader.fingerprint()


def hash_stream(stream):
    result = hashlib.sha256()
    count = 0
    while data := stream.read(BLOCK):
        check = READ_CHECK.get()
        if check is not None:
            check()
        result.update(data)
        count += len(data)
    return result.hexdigest(), count


def hash_shared(path):
    with SharedReader(path) as reader:
        first = reader.fingerprint()
        digest, count = hash_stream(reader.file)
        if first != reader.fingerprint() or first != fingerprint(path) or first["bytes"] != count:
            raise CohortChanged(f"Source changed while hashing: {path}")
        return digest


def read_anchor(path, *, limit=16 * 1024 * 1024, hook=None):
    """Optional anchor; even tiny JSON/marker reads allow the publisher's atomic replace."""
    path = absolute(path)
    plain(path.parent, directory=True)
    try:
        reader = SharedReader(path)
    except FileNotFoundError:
        return None
    with reader:
        before = reader.fingerprint()
        if before["bytes"] > limit:
            raise SnapshotError(f"Oversized anchor: {path}")
        event(hook, "anchor-open", path=path)
        data = reader.file.read(limit + 1)
        reader.file.seek(0)
        again = reader.file.read(limit + 1)
        if (len(data) != before["bytes"] or data != again or before != reader.fingerprint()
                or before != fingerprint(path)):
            raise CohortChanged(f"Anchor changed: {path}")
    plain(path, directory=False)
    return data


def mkdir(path):
    path = absolute(path)
    if not path.exists():
        mkdir(path.parent)
        path.mkdir()
    plain(path, directory=True)


def write_new(path, data):
    path = absolute(path)
    mkdir(path.parent)
    with path.open("xb") as outgoing:
        outgoing.write(data)
        outgoing.flush()
        os.fsync(outgoing.fileno())
    plain(path, directory=False)
    if path.stat().st_nlink != 1 or hash_shared(path) != sha_bytes(data):
        raise SnapshotError("Independent evidence copy failed")


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def move_new(source, target):
    plain(source, directory=False)
    plain(target.parent, directory=True)
    if os.path.lexists(target):
        raise SnapshotError(f"Refusing existing output: {target}")
    if not windows_api().MoveFileExW(native(source), native(target), 0x8):  # WRITE_THROUGH, no REPLACE_EXISTING
        raise ctypes.WinError(ctypes.get_last_error())


def publication_capability(parent):
    """Probe the actual writer API on owned tiny files; never infer replacement safety from share flags."""
    parent = absolute(parent)
    plain(parent, directory=True)
    directory = parent / (".mc3-freeze-share-probe-" + uuid.uuid4().hex)
    directory.mkdir()
    old, new = directory / "held.bin", directory / "replacement.bin"
    result = {"api": "os.replace/MoveFileExW(REPLACE_EXISTING)", "supported": False,
              "sharing": "FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE"}
    try:
        write_new(old, b"old private probe")
        write_new(new, b"new private probe")
        with SharedReader(old) as held:
            try:
                os.replace(new, old)
                result["supported"] = held.file.read() == b"old private probe" and read_anchor(old) == b"new private probe"
            except OSError as error:
                result["winerror"] = error.winerror
                result["reason"] = "Held destination prevents the running worker's atomic replace on this host"
        return result
    finally:
        # Remove only these two known independent synthetic files and this empty exclusive directory.
        for p in (old, new):
            if os.path.lexists(p):
                plain(p, directory=False)
                if p.stat().st_nlink != 1 or read_anchor(p) not in (b"old private probe", b"new private probe"):
                    raise SnapshotError("Unexpected probe change; preserve the private probe directory")
                p.unlink()
        directory.rmdir()


def copy_verified(source, destination, *, expected=None, hook=None):
    """Two reads of one pinned source HANDLE, then a separate destination hash and path-ID recheck."""
    source, destination = absolute(source), absolute(destination)
    mkdir(destination.parent)
    with SharedReader(source) as reader:
        before = reader.fingerprint()
        event(hook, "source-open", source=source, destination=destination)
        first = hashlib.sha256()
        count = 0
        with destination.open("xb") as outgoing:
            while data := reader.file.read(BLOCK):
                outgoing.write(data)
                first.update(data)
                count += len(data)
                event(hook, "copy-chunk", source=source, destination=destination, bytes=count)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        event(hook, "after-first-copy", source=source, destination=destination)
        reader.file.seek(0)
        second, second_count = hash_stream(reader.file)
        digest = first.hexdigest()
        if (before != reader.fingerprint() or count != before["bytes"] or second_count != count
                or digest != second or before != fingerprint(source)):
            raise CohortChanged(f"Source object changed during copy: {source}")
    plain(source, directory=False)
    plain(destination, directory=False)
    if expected is not None and digest != expected:
        raise SnapshotError(f"Source SHA-256 differs from required anchor: {source}")
    if destination.stat().st_nlink != 1 or os.path.samefile(source, destination) or hash_shared(destination) != digest:
        raise SnapshotError(f"Independent destination verification failed: {destination}")
    event(hook, "destination-verified", source=source, destination=destination)
    if before != fingerprint(source):
        raise CohortChanged(f"Source pathname changed after copy: {source}")
    return {"bytes": count, "sha256": digest, "source": str(source), "source_fingerprint": before}


def bink_header(path):
    with SharedReader(path) as reader:
        data = reader.file.read(36)
    if len(data) != 36 or data[:3] != b"BIK" or data[3] not in b"bfghik":
        raise SnapshotError("Invalid original Bink header")
    frames, width, height, num, den = [struct.unpack_from("<I", data, n)[0] for n in (8, 20, 24, 28, 32)]
    if not 0 < frames <= 1_000_000 or not width or not height or not num or not den:
        raise SnapshotError("Invalid original Bink geometry/rate")
    return {"frames": frames, "width": width, "height": height, "fps": str(Fraction(num, den))}


def classify_movie(saved, marker, copied, original, identity):
    """Classify authenticated *recorded* evidence; this performs no decode and makes no fresh timing claim."""
    if saved is None:
        return {"classification": "historical-marker-only" if marker is not None else "historical-unrecorded",
                "strict_recorded_provenance": False, "reason": "No original/weights provenance JSON"}
    if not isinstance(saved, dict) or saved.get("output_sha256") != copied["sha256"]:
        raise CohortChanged("Movie JSON does not authenticate the copied output")
    def need(condition):
        if not condition:
            raise ValueError("Strict recorded provenance mismatch")
    try:
        need(saved.get("origin") == "original-bink" and saved.get("historical_source_and_weights_verified") is True)
        recipe = saved["recipe"]
        effective = saved.get("validated_timing_recipe", recipe)
        header = original["header"]
        for value in (recipe, effective):
            need(value["source_sha256"] == original["sha256"] and value["source_bytes"] == original["bytes"])
            need(value["model"] == identity and value["pipeline_version"] == 1 and value["output_scale"] == 3)
            need(all(value["source"][k] == v for k, v in header.items()))
            need(value["source"]["codec"] == "binkvideo" and value["source"]["pix_fmt"] == "yuv420p")
        fps = Fraction(header["fps"])
        encoder = {"codec": "libx264", "preset": "slow", "crf": 16, "pix_fmt": "yuv420p",
                   "time_base": str(1 / fps), "video_track_timescale": fps.numerator}
        need(effective["encoder"] == encoder)
        output = saved["output"]
        need(output["width"] == header["width"] * 3 and output["height"] == header["height"] * 3)
        need(output["frames"] == header["frames"] and Fraction(output["fps"]) == fps)
        need(output["codec"] == "h264" and output["pix_fmt"] == "yuv420p")
        timing = output["timing"]
        base = Fraction(timing["time_base"])
        step = 1 / (fps * base)
        need(base > 0 and step.denominator == 1 and timing["exact_source_timing"] is True)
        need(timing["frame_step_ts"] == step and timing["frames_checked"] == header["frames"])
        need(timing["duration_ts"] == header["frames"] * step)
        pts = hashlib.sha256()
        for i in range(header["frames"]):
            pts.update(f"{i * int(step)}\n".encode("ascii"))
        need(timing["presentation_timestamps_sha256"] == pts.hexdigest())
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return {"classification": "historical-json-unverified", "strict_recorded_provenance": False,
                "reason": "Frozen output matches JSON, but strict original/model/header/recipe/timing evidence does not"}
    return {"classification": "strict-original-record", "strict_recorded_provenance": True,
            "reason": "Frozen bytes match exact original/model/recipe and recorded timing evidence; fresh decode pending"}


@dataclass
class SnapshotConfig:
    root: Path
    output: Path
    iso: Path
    textures: Path
    source_video: Path
    video: Path
    ledger: Path
    audit: Path
    model_files: dict
    expected_textures: int = 7081
    expected_movies: int = 106
    expected_iso_sha256: str = ISO_SHA256
    attempts: int = 3
    source_revision: str | None = None  # Test seam; production always obtains Git HEAD itself.
    held_stop_sha256: str | None = None


def quiescent_helpers():
    # Reuse the tested exact OS exclusion locks and stricter Windows process query; no worker is imported/run.
    directory = str(ROOT / "tools/texture_pack")
    if directory not in sys.path:
        sys.path.insert(0, directory)
    import apply_reviewed
    import install_reviewed
    return apply_reviewed.exclusion, install_reviewed.installation_workers


class QuiescentGuard:
    def __init__(self, config, worker_query):
        self.config, self.worker_query = config, worker_query
        self.last_query = None
        self.queries = 0

    def check(self, *, force=False):
        root = absolute(self.config.root)
        if os.path.lexists(root / ".local/reviewed_texture_install/active.json"):
            raise SnapshotError("Unfinished/foreign reviewed transaction marker preserved")
        for name in ("stop-video-upscale", "stop-texture-upscale"):
            if os.path.lexists(root / ".local" / name):
                raise SnapshotError("User stop flag preserved; snapshot refused: " + name)
        stop = read_anchor(root / ".local/stop-overnight-pipeline")
        if stop is None or sha_bytes(stop) != self.config.held_stop_sha256:
            raise SnapshotError("Required unchanged owned coordinator stop is absent/changed; preserved")
        now = time.monotonic()
        if force or self.last_query is None or now - self.last_query >= 1:
            workers = self.worker_query()
            self.last_query = now
            self.queries += 1
            if workers:
                raise SnapshotError("Quiescent snapshot blocked by live game/media/coordinator workers: "
                                    + json.dumps(workers, sort_keys=True))


@contextmanager
def video_exclusion(config):
    # The caller already holds coordinator -> installer. A manual normal video worker takes byte 0
    # of this exact output lock even though it does not take the coordinator lock.
    import overnight_pipeline
    directory = absolute(config.video)
    plain(directory, directory=True)
    path = directory / ".upscale.lock"
    if os.path.lexists(path):
        plain(path, directory=False)
        if path.stat().st_nlink != 1:
            raise SnapshotError("Linked existing video-output lock is forbidden")
    with overnight_pipeline.exclusive_lock(path):
        plain(path, directory=False)
        if path.stat().st_nlink != 1:
            raise SnapshotError("Video-output lock changed into a linked file")
        yield


@contextmanager
def quiescent(config, *, worker_query=None):
    if not isinstance(config.held_stop_sha256, str) or not SHA.fullmatch(config.held_stop_sha256):
        raise SnapshotError("Quiescent execution requires the exact held owned stop SHA-256")
    exclusion, real_query = quiescent_helpers()
    # Verify plain existing ancestors before the helper may create its lock directories.
    plain(config.root, directory=True)
    for relative in (".local", ".local/overnight_pipeline", ".local/overnight_pipeline/live", ".local/reviewed_texture_install"):
        p = absolute(config.root) / relative
        if os.path.lexists(p):
            plain(p, directory=True)
    with exclusion(config.root), video_exclusion(config):
        guard = QuiescentGuard(config, real_query if worker_query is None else worker_query)
        guard.check(force=True)
        token = READ_CHECK.set(guard.check)
        try:
            yield guard
        finally:
            READ_CHECK.reset(token)


def production_config(root, output):
    root = absolute(root)
    model = root / "upscale_tools/realesrgan-20220424"
    return SnapshotConfig(root, absolute(output), root / "Midnight Club 3 - DUB Edition Remix (USA).xiso.iso",
                          root / "templates/game/out/native/textures/replace", root / "templates/game/assets/video",
                          root / "templates/game/out/native/video_hd",
                          root / "templates/game/out/native/textures/reviewed_fixes.json",
                          root / ".local/overnight_pipeline/texture-audit.json",
                          {"executable": model / "realesrgan-ncnn-vulkan.exe",
                           "realesrgan-x4plus.param": model / "models/realesrgan-x4plus.param",
                           "realesrgan-x4plus.bin": model / "models/realesrgan-x4plus.bin"})


def revision(config):
    result = config.source_revision or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=config.root, text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", result):
        raise SnapshotError("Exact source revision is unavailable")
    return result


def select(config):
    if not 1 <= config.attempts <= 10 or not SHA.fullmatch(config.expected_iso_sha256):
        raise SnapshotError("Invalid snapshot policy")
    root, output = absolute(config.root), absolute(config.output)
    if output == root or root in output.parents or output in root.parents:
        raise SnapshotError("Output must be outside the project")
    if os.path.lexists(output):
        raise SnapshotError("Output must be NEW; partial snapshots cannot be resumed")
    plain(output.parent, directory=True)
    plain(config.iso, directory=False)
    originals, targets = {}, set()
    for p in walk_files(config.source_video):
        if p.suffix.lower() != ".bik":
            continue
        relative = safe_relative(p.relative_to(absolute(config.source_video)).as_posix())
        target = str(PurePosixPath(relative).with_suffix(".mp4")).lower()
        if target in targets:
            raise SnapshotError("Case-insensitive duplicate original movie")
        targets.add(target)
        plain(p, directory=False)
        originals[target] = (relative, p)
    if len(originals) != config.expected_movies:
        raise SnapshotError(f"Wrong original movie count: {len(originals)}")
    generated = {}
    for p in walk_files(config.video):
        relative = p.relative_to(absolute(config.video)).as_posix()
        if relative.lower() not in targets:
            continue  # In-flight .part.mp4, markers and unrelated exports are never payloads.
        if relative.lower() in generated:
            raise SnapshotError("Case-insensitive duplicate generated movie")
        plain(p, directory=False)
        generated[relative.lower()] = p
    if set(generated) != targets:
        raise SnapshotError("Missing expected generated movie")
    textures = {}
    plain(config.textures, directory=True)
    for p in absolute(config.textures).iterdir():
        if p.suffix.lower() != ".png":
            continue
        if not PNG.fullmatch(p.name) or p.name.lower() in textures:
            raise SnapshotError("Unexpected/duplicate active texture name")
        plain(p, directory=False)
        textures[p.name.lower()] = p
    if len(textures) != config.expected_textures:
        raise SnapshotError(f"Wrong texture count: {len(textures)}")
    return originals, generated, textures


def model_identity(config):
    if set(config.model_files) != {"executable", "realesrgan-x4plus.param", "realesrgan-x4plus.bin"}:
        raise SnapshotError("Unexpected model input set")
    hashes = {k: hash_shared(p) for k, p in config.model_files.items()}
    return {"name": "realesrgan-x4plus", "weights": {k: v for k, v in hashes.items() if k != "executable"},
            "executable_sha256": hashes["executable"], "inference_scale": 4, "unsharp": 0.25}


def freeze(config, *, execute=False, mode=None, hook=None, publication_probe=publication_capability, worker_query=None):
    originals, generated, textures = select(config)
    plan = {"format": "mc3-content-freeze-plan-v1", "mode": "plan", "output": str(absolute(config.output)),
            "payload_files": 1 + len(textures) + len(originals), "textures": len(textures),
            "movies": len(originals), "expected_iso_sha256": config.expected_iso_sha256,
            "movie_targets": sorted(originals), "no_copy_or_mutation": True,
            "decode_and_full_timing_validation": "PENDING", "requested_mode": mode}
    if not execute:
        return plan
    windows_api()
    if mode not in ("quiescent", "concurrent"):
        raise SnapshotError("Execution requires an explicitly selected quiescent or concurrent mode")
    if mode == "quiescent":
        with quiescent(config, worker_query=worker_query) as guard:
            capability = publication_probe(absolute(config.output).parent)
            return _execute(config, plan, originals, generated, textures, mode, capability, hook, guard)
    capability = publication_probe(absolute(config.output).parent)
    if capability.get("supported") is not True:
        raise SnapshotError("Concurrent snapshot is unsafe: actual held-target os.replace capability failed: "
                            + json.dumps(capability, sort_keys=True))
    return _execute(config, plan, originals, generated, textures, mode, capability, hook, None)


def _execute(config, plan, originals, generated, textures, mode, capability, hook, guard):
    output = absolute(config.output)
    user_hook = hook
    def checked_hook(name, **values):
        if guard is not None:
            guard.check(force=name == "movie-attempt")
        event(user_hook, name, **values)
    hook = checked_hook
    if guard is not None:
        guard.check(force=True)
    output.mkdir()  # Exclusive directory creation, never reuse or resume a partial snapshot.
    records = {}
    started = utc()
    try:
        write_new(output / "snapshot-started.json", json_bytes({**plan, "mode": "executing", "started_at": started,
                                                               "no_copy_or_mutation": False, "status": "incomplete"}))
        source_revision = revision(config)
        tool_sha = hash_shared(Path(__file__))
        ledger = read_anchor(config.ledger)
        audit = read_anchor(config.audit)
        if ledger is None or audit is None:
            raise SnapshotError("Reviewed ledger and complete texture audit are required")
        ledger_doc, audit_doc = json.loads(ledger), json.loads(audit)
        if (not isinstance(ledger_doc, dict) or audit_doc.get("complete") is not True
                or audit_doc.get("counts", {}).get("eligible") != config.expected_textures):
            raise SnapshotError("Texture audit is incomplete or has wrong coverage")
        identity = model_identity(config)

        def add(source, target, expected=None):
            safe_relative(target)
            destination = output / target
            row = copy_verified(source, destination, expected=expected, hook=hook)
            records[target] = row
            return row

        add(config.iso, "game/game.iso", config.expected_iso_sha256)
        for p in sorted(textures.values(), key=lambda p: p.name.lower()):
            add(p, "textures/replace/" + p.name)
        frozen_originals = {}
        for target, (relative, p) in sorted(originals.items()):
            row = add(p, "evidence/source-video/" + relative)
            row["header"] = bink_header(output / "evidence/source-video" / relative)
            frozen_originals[target] = row
        write_new(output / "evidence/texture-records/reviewed_fixes.json", ledger)
        write_new(output / "evidence/texture-records/texture-audit.json", audit)
        write_new(output / "evidence/current-model-identity.json", json_bytes(identity))
        movie_records = {}
        for target, source in sorted(generated.items()):
            for attempt in range(1, config.attempts + 1):
                attempt_root = output / "evidence/attempts" / hashlib.sha256(target.encode()).hexdigest() / str(attempt)
                mkdir(attempt_root)
                try:
                    event(hook, "movie-attempt", source=source, target=target, attempt=attempt)
                    before = {"json": read_anchor(source.with_suffix(".upscale.json"), hook=hook),
                              "marker": read_anchor(source.with_suffix(".model.txt"), limit=4096, hook=hook)}
                    row = copy_verified(source, attempt_root / "movie-copy.mp4", hook=hook)
                    event(hook, "movie-before-after-anchors", source=source, target=target, attempt=attempt)
                    after = {"json": read_anchor(source.with_suffix(".upscale.json"), hook=hook),
                             "marker": read_anchor(source.with_suffix(".model.txt"), limit=4096, hook=hook)}
                    if before != after or row["source_fingerprint"] != fingerprint(source):
                        raise CohortChanged("Movie/sidecar publication crossed the copy")
                    saved = json.loads(before["json"]) if before["json"] is not None else None
                    if before["json"] is not None and not isinstance(saved, dict):
                        raise CohortChanged("Authoritative JSON must be an object")
                    classification = classify_movie(saved, before["marker"], row, frozen_originals[target], identity)
                    destination = output / "video_hd" / target
                    mkdir(destination.parent)
                    if hash_shared(attempt_root / "movie-copy.mp4") != row["sha256"]:
                        raise SnapshotError("Prepared movie copy changed")
                    move_new(attempt_root / "movie-copy.mp4", destination)
                    records["video_hd/" + target] = row
                    anchors = {}
                    for key, suffix in [("json", ".upscale.json"), ("marker", ".model.txt")]:
                        data = before[key]
                        evidence = "evidence/video-records/" + str(PurePosixPath(target).with_suffix(suffix))
                        if data is not None:
                            write_new(output / evidence, data)
                        anchors[key] = {"present": data is not None, "path": evidence if data is not None else None,
                                        "sha256": sha_bytes(data) if data is not None else None}
                    movie_records[target] = {**row, **classification, "anchors": anchors,
                                             "source_bink": "evidence/source-video/" + originals[target][0],
                                             "attempts": attempt, "recorded_metadata": saved,
                                             "marker_text": before["marker"].decode("utf-8", errors="replace").strip()
                                             if before["marker"] is not None else None,
                                             "decode_and_full_timing_validation": "PENDING"}
                    break
                except (CohortChanged, json.JSONDecodeError) as error:
                    write_new(attempt_root / "rejected.json", json_bytes({"accepted": False, "error": str(error)}))
                    if attempt == config.attempts:
                        raise CohortChanged(f"No stable cohort after {attempt} attempts: {target}") from error
        # Reused originals must retain their exact transitive record inside this same frozen cohort.
        for target, row in movie_records.items():
            reuse = (row["recorded_metadata"] or {}).get("identical_source_reuse")
            if reuse and row["strict_recorded_provenance"]:
                other_target = str(PurePosixPath(safe_relative(reuse["original_movie"])).with_suffix(".mp4")).lower()
                other = movie_records.get(other_target, {})
                if (reuse.get("method") != "byte-identical-original-and-exact-picture-recipe-v1"
                        or reuse.get("original_output_sha256") != row["sha256"]
                        or other.get("sha256") != row["sha256"]
                        or reuse.get("original_source_sha256") != frozen_originals[target]["sha256"]
                        or frozen_originals.get(other_target, {}).get("sha256") != frozen_originals[target]["sha256"]
                        or other.get("anchors", {}).get("json", {}).get("sha256") != reuse.get("original_record_sha256")):
                    row.update(classification="historical-json-unverified", strict_recorded_provenance=False,
                               reason="Identical-source transitive evidence does not match this frozen cohort")
        event(hook, "before-complete", output=output)
        if (read_anchor(config.ledger) != ledger or read_anchor(config.audit) != audit
                or model_identity(config) != identity or revision(config) != source_revision
                or hash_shared(Path(__file__)) != tool_sha):
            raise SnapshotError("Ledger/audit/model/source/tool changed during snapshot")
        again_originals, again_generated, again_textures = select_for_final(config)
        if (again_originals != originals or again_generated != generated or again_textures != textures):
            raise SnapshotError("Selected source catalogue changed during snapshot")
        payload = {"game/game.iso"} | {"textures/replace/" + p.name for p in textures.values()} | {"video_hd/" + t for t in originals}
        actual_payload = {p.relative_to(output).as_posix() for base in ("game", "textures", "video_hd") for p in walk_files(output / base)}
        if actual_payload != payload or len(payload) != 1 + config.expected_textures + config.expected_movies:
            raise SnapshotError("Final payload inventory differs from exact selected catalogue")
        for target, row in records.items():
            p = output / target
            if p.stat().st_nlink != 1 or p.stat().st_size != row["bytes"] or hash_shared(p) != row["sha256"]:
                raise SnapshotError(f"Frozen file changed before final seal: {target}")
        inventory = {p.relative_to(output).as_posix(): {"bytes": p.stat().st_size, "sha256": hash_shared(p)}
                     for p in sorted(walk_files(output))}
        counts = {name: sum(row["classification"] == name for row in movie_records.values()) for name in
                  ("strict-original-record", "historical-marker-only", "historical-unrecorded", "historical-json-unverified")}
        manifest = {"format": "mc3-frozen-content-cohort-v1", "status": "complete", "started_at": started,
                    "completed_at": utc(), "source_revision": source_revision, "tool_sha256": tool_sha,
                    "payload_files": len(payload), "texture_count": len(textures), "movie_count": len(originals),
                    "source_bink_count": len(frozen_originals), "payload": {p: records[p] for p in sorted(payload)},
                    "all_copied_sources": records, "movies": movie_records, "movie_cohort_counts": counts,
                    "ledger_sha256": sha_bytes(ledger), "texture_audit_sha256": sha_bytes(audit),
                    "model_identity": identity, "inventory": inventory,
                    "publication_capability": capability, "execution_mode": mode,
                    "exclusion_locks_in_order": [str(absolute(config.root) / ".local/overnight_pipeline/live/coordinator.lock"),
                                                 str(absolute(config.root) / ".local/reviewed_texture_install/install.lock"),
                                                 str(absolute(config.video) / ".upscale.lock")] if guard else [],
                    "quiescent_worker_queries": guard.queries if guard else None,
                    "held_coordinator_stop_sha256": config.held_stop_sha256 if guard else None,
                    "decode_and_full_timing_validation": "PENDING",
                    "limits": ["Per-file stable observations, not a simultaneous global filesystem snapshot.",
                               "Strict means copied bytes match historical recorded proof, not a new decode/timing audit.",
                               "Failed attempts are retained as unaccepted evidence; only exact payload entries may ship.",
                               "No saves, settings, runtime executable or full-project backup is included."]}
        prepared = output / (".snapshot-manifest-" + uuid.uuid4().hex + ".prepare")
        write_new(prepared, json_bytes(manifest))
        if guard is not None:
            guard.check(force=True)
        move_new(prepared, output / "snapshot-manifest.json")
        return manifest
    except BaseException as error:
        try:
            write_new(output / "snapshot-failed.json", json_bytes({"status": "failed", "at": utc(),
                      "error": type(error).__name__ + ": " + str(error), "complete": False,
                      "partial_directory_retained": True, "resume_supported": False}))
        except Exception:
            pass
        raise


def select_for_final(config):
    # Same selector, without weakening its public exclusive-output guard or mutating the real configuration.
    from dataclasses import replace
    unused = absolute(config.output).with_name(absolute(config.output).name + ".final-inventory-" + uuid.uuid4().hex)
    return select(replace(config, output=unused))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--execute", action="store_true", help="Copy into a NEW external G: directory; default is a read-only plan")
    parser.add_argument("--mode", choices=("quiescent", "concurrent"), help="Explicitly required with --execute")
    parser.add_argument("--held-stop-sha256", help="Quiescent mode requires this exact existing owned coordinator stop; never clears it")
    args = parser.parse_args(argv)
    output = absolute(args.output)
    if args.execute and (os.name != "nt" or not args.output.is_absolute() or output.drive.upper() != "G:"):
        parser.error("Production execution requires a fresh external G: output")
    if args.execute and args.mode is None:
        parser.error("--execute requires an explicitly specified --mode")
    config = production_config(args.root, output)
    config.held_stop_sha256 = args.held_stop_sha256
    result = freeze(config, execute=args.execute, mode=args.mode)
    print(json.dumps({k: result[k] for k in ("format", "status", "payload_files", "movie_cohort_counts") if k in result}
                     if args.execute else result, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
