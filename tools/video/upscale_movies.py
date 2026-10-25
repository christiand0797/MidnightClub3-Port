#!/usr/bin/env python3
"""Upscale original Bink pictures without changing their frame order or timing.

  py -3.12 tools/video/upscale_movies.py --model realesrgan-x4plus
  py -3.12 tools/video/upscale_movies.py --status
  py -3.12 tools/video/upscale_movies.py --audit-only --only mc3intro

The title still supplies Bink audio/timing. HD pictures are 3x source size; 4x
models are reduced with Lanczos. Work always starts from assets/video/*.bik,
never an earlier upscale. A completed movie is decoded/validated before an
atomic replacement of its published MP4. A provenance sidecar records source,
model and output hashes. Older model markers are accepted only after validating
the complete picture stream, and their historical model attribution remains
explicitly unverified. Create .local/stop-video-upscale to stop after a movie.
Use --require-original-provenance for an all-original pass: historical markers
remain usable by default but cannot satisfy that stricter resume/audit gate.
Optional --pause-file keeps the same encoder/decoded frames across a game pause
and bounds new inference calls to eight frames. With this option, --stop-file
discards only the in-flight movie and exits 2; its next run starts that movie over.
"""
import argparse
import contextlib
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from fractions import Fraction
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "templates" / "game" / "assets" / "video"
OUT = ROOT / "templates" / "game" / "out" / "native" / "video_hd"
QUARANTINE = ROOT / ".local" / "video-upscale-candidates"
ESRGAN_DIR = ROOT / "upscale_tools" / "realesrgan-20220424"
SCALE = 3
CHUNK = 400
PAUSE_CHUNK = 8
PAUSE_POLL_SECONDS = 1.0
PIPELINE_VERSION = 1
MODELS = {  # folder, inference scale, post-resize unsharp amount
    "realesr-animevideov3": (ESRGAN_DIR / "models", 3, 0.5),
    "realesrgan-x4plus": (ESRGAN_DIR / "models", 4, 0.25),
    "upscayl-lite-4x": (ROOT / "upscale_tools" / "models_extra", 4, 0.4),
    "ultrasharp-4x": (ROOT / "upscale_tools" / "models_extra", 4, 0.0),
    "high-fidelity-4x": (ROOT / "upscale_tools" / "models_extra", 4, 0.0),
    "remacri-4x": (ROOT / "upscale_tools" / "models_extra", 4, 0.0),
}
PRIORITY = ("rockstar", "sdlogo", "mc3intro", "reward/", "story/", "tokyotut/", "tutorial/")


class MovieStopped(RuntimeError):
    """Cooperative cancellation/orphan exit; the movie remains pending, not failed."""


class PauseOwner:
    """Pin the startup parent handle against later PID reuse; equality does not exclude reuse before open."""
    def __init__(self, pid):
        self.handle = None
        if os.name != "nt" or pid <= 0 or pid != os.getppid():
            raise MovieStopped("pause owner must be this worker's live Windows parent")
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.api.OpenProcess.restype = wintypes.HANDLE
        self.api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.api.WaitForSingleObject.restype = wintypes.DWORD
        self.api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api.CloseHandle.restype = wintypes.BOOL
        self.handle = self.api.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE only; no mutation rights.
        if not self.handle:
            raise MovieStopped("pause owner is unavailable; inference will not start")
        try:
            if pid != os.getppid():
                raise MovieStopped("pause parent identity changed during startup")
            self.check()
        except BaseException:
            self.close()
            raise

    def check(self):
        if not self.handle or self.api.WaitForSingleObject(self.handle, 0) != 0x102:  # WAIT_TIMEOUT=still live.
            raise MovieStopped("pause owner exited or its liveness is uncertain; inference will not resume")

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self): return self
    def __exit__(self, *_args): self.close()


def cooperate(pause_file=None, stop_file=None, owner=None):
    """Presence requests a pause. Never clear markers; explicit stop wins over pause/owner checks."""
    announced = False
    while True:
        if stop_file is not None and os.path.lexists(stop_file):
            raise MovieStopped(f"stop requested by {stop_file}; in-flight movie will restart on resume")
        if owner is not None:
            owner.check()
        if pause_file is None or not os.path.lexists(pause_file):
            if announced:
                print("    cooperative pause ended; resuming the same movie/encoder", flush=True)
            return announced
        if not announced:
            print(f"    cooperative pause requested by {pause_file}; no new inference until removed", flush=True)
            announced = True
        time.sleep(PAUSE_POLL_SECONDS)


def check_inference_identity(model, recipe):
    if model_identity(model) != recipe.get("model"):
        raise ValueError("model weights/executable changed during the movie; pinned recipe cannot be published")


def cooperate_checked(pause_file, stop_file, owner, check):
    while True:
        cooperate(pause_file, stop_file, owner)
        check()
        # If a pause began while hashing, revalidate after that wait rather than trust a pre-pause identity.
        if not cooperate(pause_file, stop_file, owner):
            return


def sha256(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def ffmpeg_bin():
    if shutil.which("ffmpeg") and shutil.which("ffprobe"):
        return Path(shutil.which("ffmpeg")).parent
    pk = Path.home() / "AppData" / "Local" / "Microsoft" / "WinGet" / "Packages"
    found = sorted(pk.glob("Gyan.FFmpeg*/ffmpeg-*/bin"))
    if not found:
        raise RuntimeError("ffmpeg not found: winget install Gyan.FFmpeg")
    return found[-1]


def binary(ff, name):
    return str(ff / (name + (".exe" if os.name == "nt" else "")))


def bink_header(path):
    # Bink stores the number of video frames at offset 8. FFmpeg exposes it as
    # stream duration (not nb_frames); see libavformat/bink.c read_header.
    with Path(path).open("rb") as f:
        header = f.read(36)
    if len(header) != 36 or header[:3] != b"BIK" or header[3] not in b"bfghik":
        raise ValueError(f"unsupported/truncated Bink header: {path}")
    frames, width, height, num, den = (struct.unpack_from("<I", header, off)[0] for off in (8, 20, 24, 28, 32))
    if not 0 < frames <= 1_000_000 or not width or not height or not num or not den:
        raise ValueError(f"invalid Bink header: {path}")
    return {"frames": frames, "width": width, "height": height, "fps": str(Fraction(num, den))}


def probe(ff, path, count_frames=False):
    command = [binary(ff, "ffprobe"), "-v", "error", "-select_streams", "v:0"]
    if count_frames:
        command += ["-count_frames", "-show_frames"]
    entries = "stream=width,height,r_frame_rate,avg_frame_rate,time_base,duration_ts,nb_frames,nb_read_frames,codec_name,pix_fmt"
    if count_frames:
        entries += ":frame=pts,duration,pkt_duration"
    command += ["-show_entries", entries,
                "-of", "json", str(path)]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    # A decoder may exit successfully after reporting damaged frames. Do not
    # accept those clips merely because ffprobe still returned a stream record.
    if result.stderr.strip():
        raise RuntimeError(f"video decode errors in {path}: {result.stderr.strip()[-2000:]}")
    document = json.loads(result.stdout)
    streams = document.get("streams", [])
    if len(streams) != 1:
        raise ValueError(f"expected a video stream in {path}")
    stream = streams[0]
    fps = Fraction(stream["r_frame_rate"])
    if fps <= 0 or int(stream["width"]) <= 0 or int(stream["height"]) <= 0:
        raise ValueError(f"invalid video geometry/rate in {path}")
    key = "nb_read_frames" if count_frames else "nb_frames"
    frames = stream.get(key)
    if not count_frames and stream.get("codec_name") == "binkvideo":
        header = bink_header(path)
        if (header["width"], header["height"], header["fps"]) != (int(stream["width"]), int(stream["height"]), str(fps)):
            raise ValueError(f"Bink header disagrees with decoder: {path}")
        frames = header["frames"]
    info = {"width": int(stream["width"]), "height": int(stream["height"]),
            "fps": str(fps), "frames": int(frames) if frames not in (None, "N/A") else None,
            "codec": stream.get("codec_name"), "pix_fmt": stream.get("pix_fmt")}
    if count_frames:
        decoded = document.get("frames", [])
        info["timeline"] = {"time_base": stream.get("time_base"), "duration_ts": stream.get("duration_ts"),
                            "avg_fps": stream.get("avg_frame_rate"),
                            "pts": [frame.get("pts") for frame in decoded],
                            "durations": [frame.get("duration", frame.get("pkt_duration")) for frame in decoded]}
    return info


def validate_info(info, source, expected_frames):
    expected = (source["width"] * SCALE, source["height"] * SCALE)
    if (info["width"], info["height"]) != expected:
        raise ValueError(f"HD size differs: {info['width']}x{info['height']}, expected {expected}")
    if Fraction(info["fps"]) != Fraction(source["fps"]):
        raise ValueError(f"frame rate differs: {info['fps']} vs {source['fps']}")
    if not expected_frames or info["frames"] != expected_frames:
        raise ValueError(f"frame count differs: {info['frames']} vs {expected_frames}")
    if info["codec"] != "h264" or info["pix_fmt"] != "yuv420p":
        raise ValueError("HD movie must be H.264 yuv420p for the native Media Foundation decoder")


def validate_timeline(info, fps, expected_frames):
    info = dict(info)
    timeline = info.pop("timeline")
    fps = Fraction(fps)
    try:
        time_base = Fraction(timeline["time_base"])
        average = Fraction(timeline["avg_fps"])
        if time_base <= 0:
            raise ValueError("nonpositive time base")
    except (ValueError, TypeError, ZeroDivisionError) as error:
        raise ValueError("movie has no valid timestamp time base/average rate") from error
    step = 1 / (fps * time_base)
    if step.denominator != 1 or average != fps:
        raise ValueError("movie time base cannot represent the exact source frame interval")
    if (len(timeline["pts"]) != expected_frames or len(timeline["durations"]) != expected_frames
            or any(pts != index * step for index, pts in enumerate(timeline["pts"]))
            or any(duration != step for duration in timeline["durations"])):
        raise ValueError("movie presentation timestamps/durations differ from the exact source frame grid")
    if timeline["duration_ts"] != expected_frames * step:
        raise ValueError("movie stream duration differs from the exact source duration")
    info["timing"] = {"time_base": str(time_base), "frame_step_ts": int(step),
                      "duration_ts": timeline["duration_ts"], "frames_checked": expected_frames,
                      "exact_source_timing": True,
                      "presentation_timestamps_sha256": hashlib.sha256(
                          "".join(f"{pts}\n" for pts in timeline["pts"]).encode("ascii")).hexdigest()}
    return info


def validate_movie(ff, dst, source, expected_frames):
    info = probe(ff, dst, count_frames=True)
    validate_info(info, source, expected_frames)
    return validate_timeline(info, source["fps"], expected_frames)


def atomic_write(path, data):
    path = Path(path)
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temp = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def publish(part, dst, attempts=600, delay=1, *, pause_file=None, stop_file=None, owner=None, before_publish=None):
    # Only called after validation. The game can have the previous picture open.
    for attempt in range(attempts):
        if before_publish is not None:
            cooperate_checked(pause_file, stop_file, owner, before_publish)
        else:
            cooperate(pause_file, stop_file, owner)
        try:
            os.replace(part, dst)
            return
        except PermissionError:
            if attempt + 1 == attempts:
                raise
            time.sleep(delay)


def model_identity(model):
    folder, scale, sharpen = MODELS[model]
    names = [model]
    if model == "realesr-animevideov3":
        names = [f"{model}-x{scale}"]
    paths = [folder / f"{name}.{ext}" for name in names for ext in ("param", "bin")]
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(f"missing model file: {path}")
    exe = ESRGAN_DIR / "realesrgan-ncnn-vulkan.exe"
    return {"name": model, "weights": {p.name: sha256(p) for p in paths},
            "executable_sha256": sha256(exe), "inference_scale": scale, "unsharp": sharpen}


def recipe_for(src, source, identity):
    fps = Fraction(source["fps"])
    return {"pipeline_version": PIPELINE_VERSION, "source_sha256": sha256(src),
            "source_bytes": src.stat().st_size, "source": source,
            "model": identity, "output_scale": SCALE,
            "encoder": {"codec": "libx264", "preset": "slow", "crf": 16, "pix_fmt": "yuv420p",
                        "time_base": str(1 / fps), "video_track_timescale": fps.numerator}}


def previous_timing_recipe(recipe):
    """Only the old container timing defaults may differ; source, weights and picture settings must match."""
    if not {"time_base", "video_track_timescale"} <= recipe.get("encoder", {}).keys():
        return None
    return dict(recipe, encoder={key: value for key, value in recipe["encoder"].items()
                                 if key not in ("time_base", "video_track_timescale")})


def encoded_picture_hash(ff, path):
    result = subprocess.run([binary(ff, "ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_packets",
                             "-show_data_hash", "sha256", "-show_entries", "packet=data_hash", "-of", "json", str(path)],
                            capture_output=True, text=True, check=True)
    if result.stderr.strip():
        raise ValueError("packet read errors while checking lossless retiming")
    packets = json.loads(result.stdout).get("packets", [])
    hashes = [packet["data_hash"] for packet in packets]
    if not hashes:
        raise ValueError("no encoded picture packets")
    return hashlib.sha256("\n".join(hashes).encode("ascii")).hexdigest()


def lossless_timing_copy(ff, src, part, source):
    """Repair only the proven default-timescale case; validate every old/new PTS and unchanged H.264 payload."""
    info = probe(ff, src, count_frames=True)
    if (Fraction(source["fps"]) != Fraction(10000000, 333333)
            or Fraction(info["fps"]) != Fraction(999991, 33333)
            or Fraction(info["timeline"]["time_base"]) != Fraction(1, 999991)):
        raise ValueError("candidate is not the verified default-timescale rounding case")
    validate_info(info, dict(source, fps=info["fps"]), source["frames"])
    validate_timeline(info, info["fps"], source["frames"])
    # Prove that integer rescaling alone maps every PTS/duration to the exact source grid. This is not a
    # floating-point tolerance or permission to retime arbitrary rates, missing frames, gaps or duplicates.
    target_scale = Fraction(source["fps"]).numerator
    target_step = Fraction(source["fps"]).denominator
    base = Fraction(info["timeline"]["time_base"])
    def rescale(ticks):
        value = ticks * base * target_scale
        return (2 * value.numerator + value.denominator) // (2 * value.denominator)
    if (any(rescale(pts) != index * target_step for index, pts in enumerate(info["timeline"]["pts"]))
            or any(rescale(duration) != target_step for duration in info["timeline"]["durations"])
            or rescale(info["timeline"]["duration_ts"]) != source["frames"] * target_step):
        raise ValueError("integer timestamp rescaling does not produce the exact source grid")
    payload = encoded_picture_hash(ff, src)
    subprocess.run([binary(ff, "ffmpeg"), "-v", "error", "-y", "-i", str(src), "-map", "0:v:0", "-an",
                    "-c:v", "copy", "-video_track_timescale", str(target_scale), "-movflags", "+faststart", str(part)], check=True)
    validated = validate_movie(ff, part, source, source["frames"])
    if encoded_picture_hash(ff, part) != payload:
        raise ValueError("encoded picture payload changed during lossless timing copy")
    return validated, payload


def retime_proven_output(ff, src, dst, source, recipe, saved):
    # A text marker or adopted legacy record cannot establish the original inference recipe. Only an existing
    # original-Bink record with matching source/model/weights/settings and output SHA may take this path.
    if saved.get("origin") != "original-bink" or saved.get("historical_source_and_weights_verified") is not True:
        return False
    if (saved.get("recipe") != previous_timing_recipe(recipe)
            or sha256(dst) != saved.get("output_sha256") or sha256(src) != recipe.get("source_sha256")):
        return False
    fd, name = tempfile.mkstemp(prefix=f".{dst.stem}.retime.", suffix=".part.mp4", dir=dst.parent)
    os.close(fd)
    part = Path(name)
    try:
        info, payload = lossless_timing_copy(ff, dst, part, source)
        if sha256(dst) != saved["output_sha256"] or sha256(src) != recipe["source_sha256"]:
            raise ValueError("source/published output changed during retiming")
        with part.open("r+b") as stream:
            os.fsync(stream.fileno())
        record = dict(saved, output=info, output_sha256=sha256(part), validated_timing_recipe=recipe,
                      container_retimed_from_sha256=saved["output_sha256"], encoded_picture_sha256=payload,
                      container_timing_transform={"method": "stream-copy", "video_track_timescale": Fraction(source["fps"]).numerator},
                      validated_at=datetime.now(timezone.utc).isoformat())
        publish(part, dst)
        atomic_write(dst.with_suffix(".upscale.json"), json.dumps(record, indent=2) + "\n")
        print(f"retimed verified picture without inference/re-encoding: {dst}", flush=True)
        return True
    finally:
        part.unlink(missing_ok=True)


def original_provenance(saved):
    """Only actual original-source inference records establish historical source/weights verification."""
    return (saved.get("origin") == "original-bink"
            and saved.get("historical_source_and_weights_verified") is True)


def current_output(ff, src, dst, source, recipe, adopt_legacy=True, require_original_provenance=False):
    if not dst.is_file():
        return False
    meta = dst.with_suffix(".upscale.json")
    if meta.exists():
        try:
            saved = json.loads(meta.read_text(encoding="utf-8"))
            if saved["output_sha256"] != sha256(dst):
                return False
            if require_original_provenance and not original_provenance(saved):
                return False
            if saved["recipe"] == recipe:
                return True
            if saved["recipe"] != previous_timing_recipe(recipe):
                return False
            if (saved.get("validated_timing_recipe") == recipe
                    and saved.get("output", {}).get("timing", {}).get("exact_source_timing") is True):
                return True
            # Reuse a previously generated clip only after full decode and exact PTS verification. Preserve
            # its original attribution, including any explicitly unverified historical model/source claim.
            try:
                info = validate_movie(ff, dst, source, source["frames"])
            except ValueError:
                if adopt_legacy:
                    return retime_proven_output(ff, src, dst, source, recipe, saved)
                return False
            if adopt_legacy:
                saved.update(validated_timing_recipe=recipe, output=info, validated_at=datetime.now(timezone.utc).isoformat(),
                             timing_validation_upgrade=True)
                atomic_write(meta, json.dumps(saved, indent=2) + "\n")
            return True
        except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError):
            return False
    if require_original_provenance:
        return False
    marker = dst.with_suffix(".model.txt")
    if not marker.exists() or marker.read_text(encoding="utf-8").strip() != recipe["model"]["name"]:
        return False
    # A historical text marker does not prove weights or source identity. Keep
    # that distinction in the record rather than inventing historical evidence.
    try:
        info = validate_movie(ff, dst, source, source["frames"])
    except (OSError, ValueError, subprocess.SubprocessError, RuntimeError):
        return False
    if adopt_legacy:
        record = {"recipe": recipe, "output": info, "output_sha256": sha256(dst),
                  "origin": "validated-legacy-model-marker", "historical_source_and_weights_verified": False,
                  "validated_at": datetime.now(timezone.utc).isoformat()}
        atomic_write(meta, json.dumps(record, indent=2) + "\n")
    return True


def stop_encoder(enc):
    if enc.stdin and not enc.stdin.closed:
        try:
            enc.stdin.close()
        except OSError:
            pass
    if enc.poll() is None:
        enc.terminate()
        try:
            enc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            enc.kill()
            enc.wait()


def quarantine_encoded_movie(part, src, dst, recipe, error):
    """Keep completed inference/encode work out of the published movie tree for later validated recovery."""
    QUARANTINE.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix=dst.stem + "-", dir=QUARANTINE))
    candidate = folder / "candidate.mp4"
    os.replace(part, candidate)
    record = {"status": "unvalidated-encoded-candidate", "source_path": str(src.resolve()),
              "published_destination": str(dst.resolve()), "recipe": recipe,
              "candidate_sha256": sha256(candidate), "candidate_bytes": candidate.stat().st_size,
              "failure": f"{type(error).__name__}: {error}", "automatic_reuse": False,
              "saved_at": datetime.now(timezone.utc).isoformat()}
    atomic_write(folder / "candidate.json", json.dumps(record, indent=2) + "\n")
    print(f"  completed candidate retained for recovery: {candidate}", flush=True)


def upscale_movie(ff, src, dst, model, source=None, recipe=None, chunk_size=CHUNK, *, pause_file=None, stop_file=None,
                  pause_owner=None):
    cooperate(pause_file, stop_file, pause_owner)
    model_dir, model_scale, sharpen = MODELS[model]
    source = source or probe(ff, src)
    recipe = recipe or recipe_for(src, source, model_identity(model))
    w, h, fps = source["width"], source["height"], Fraction(source["fps"])
    W, H = w * SCALE, h * SCALE
    if W % 2 or H % 2 or chunk_size < 1:
        raise ValueError("H.264 yuv420p needs even dimensions; chunk size must be positive")
    if pause_file is not None:
        chunk_size = min(chunk_size, PAUSE_CHUNK)
    dst.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f".{dst.stem}.", suffix=".part.mp4", dir=dst.parent)
    os.close(fd)
    part = Path(name)
    enc = None
    encoded = False
    try:
        with tempfile.TemporaryDirectory(prefix="mc3movie_") as tmp_name:
            tmp = Path(tmp_name)
            lo = tmp / "lo"
            lo.mkdir()
            cooperate(pause_file, stop_file, pause_owner)
            subprocess.run([binary(ff, "ffmpeg"), "-v", "error", "-xerror", "-i", str(src),
                            "-map", "0:v:0", "-an", "-fps_mode", "passthrough", str(lo / "f%06d.png")], check=True)
            frames = sorted(lo.glob("*.png"))
            if not frames or (source["frames"] is not None and len(frames) != source["frames"]):
                raise ValueError(f"Bink decode returned {len(frames)} frames; expected {source['frames']}")
            cooperate(pause_file, stop_file, pause_owner)
            enc = subprocess.Popen([binary(ff, "ffmpeg"), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                    "-s", f"{W}x{H}", "-r", f"{fps.numerator}/{fps.denominator}", "-i", "-",
                                    "-an", "-vf", f"unsharp=5:5:{sharpen}", "-c:v", "libx264", "-preset", "slow",
                                    "-crf", "16", "-pix_fmt", "yuv420p",
                                    "-enc_time_base", f"{fps.denominator}:{fps.numerator}",
                                    "-video_track_timescale", str(fps.numerator), "-movflags", "+faststart", str(part)],
                                   stdin=subprocess.PIPE)
            for i in range(0, len(frames), chunk_size):
                cooperate(pause_file, stop_file, pause_owner)
                cin, cout = tmp / "cin", tmp / "cout"
                for folder in (cin, cout):
                    if folder.exists():
                        shutil.rmtree(folder)
                    folder.mkdir()
                chunk = frames[i:i + chunk_size]
                for frame in chunk:
                    frame.rename(cin / frame.name)
                cooperate(pause_file, stop_file, pause_owner)
                if pause_file is not None:
                    cooperate_checked(pause_file, stop_file, pause_owner, lambda: check_inference_identity(model, recipe))
                result = subprocess.run([str(ESRGAN_DIR / "realesrgan-ncnn-vulkan.exe"), "-i", str(cin), "-o", str(cout),
                                         "-n", model, "-s", str(model_scale), "-m", str(model_dir), "-f", "png", "-j", "1:2:2"],
                                        capture_output=True)
                if result.returncode:
                    raise RuntimeError(f"upscaler exited {result.returncode}: {result.stderr.decode(errors='replace')[-2000:]}")
                if {p.name for p in cout.glob("*.png")} != {p.name for p in chunk}:
                    raise ValueError("upscaler returned a missing or unexpected frame")
                for frame in chunk:
                    cooperate(pause_file, stop_file, pause_owner)
                    with Image.open(cout / frame.name) as image:
                        if image.size != (w * model_scale, h * model_scale):
                            raise ValueError(f"incorrect model output size: {frame.name}, {image.size}")
                        image = image.convert("RGB")
                        if image.size != (W, H):
                            image = image.resize((W, H), Image.Resampling.LANCZOS)
                        enc.stdin.write(image.tobytes())
                print(f"    {min(i + chunk_size, len(frames))}/{len(frames)}", flush=True)
            cooperate(pause_file, stop_file, pause_owner)
            enc.stdin.close()
            if enc.wait():
                raise RuntimeError(f"encoding {dst} failed")
            encoded = True
        cooperate(pause_file, stop_file, pause_owner)
        info = validate_movie(ff, part, source, len(frames))
        cooperate(pause_file, stop_file, pause_owner)
        if pause_file is not None:
            check_inference_identity(model, recipe)
        if sha256(src) != recipe["source_sha256"]:
            raise ValueError("original movie changed while the upscale was running")
        # Windows FlushFileBuffers requires a handle opened for writing.
        with part.open("r+b") as f:
            os.fsync(f.fileno())
        record = {"recipe": recipe, "output": info, "output_sha256": sha256(part),
                  "origin": "original-bink", "historical_source_and_weights_verified": True,
                  "validated_at": datetime.now(timezone.utc).isoformat()}
        cooperate(pause_file, stop_file, pause_owner)
        if pause_file is not None:
            def before_publish():
                check_inference_identity(model, recipe)
                if sha256(src) != recipe["source_sha256"]:
                    raise ValueError("original movie changed before publication")
            publish(part, dst, pause_file=pause_file, stop_file=stop_file, owner=pause_owner, before_publish=before_publish)
        else:
            publish(part, dst)
        # JSON is authoritative for resume. Text markers remain for old tools.
        atomic_write(dst.with_suffix(".upscale.json"), json.dumps(record, indent=2) + "\n")
        atomic_write(dst.with_suffix(".model.txt"), model + "\n")
        return len(frames)
    except MovieStopped:
        # Close/stop our encoder in finally before unlinking its output (required by Windows file sharing).
        # A fully encoded but unpublished file is also discarded on explicit cooperative cancellation.
        encoded = False
        raise
    except Exception as error:
        if encoded and part.exists():
            try:
                quarantine_encoded_movie(part, src, dst, recipe, error)
            except Exception as retention_error:
                location = part if part.exists() else QUARANTINE
                print(f"  completed candidate retained under {location}; quarantine metadata failed: {retention_error}",
                      file=sys.stderr, flush=True)
        raise
    finally:
        if enc is not None:
            stop_encoder(enc)
        if not encoded:
            part.unlink(missing_ok=True)


def legacy_workers():
    """Find old Python workers that predate the output-directory OS lock."""
    if os.name == "nt":
        command = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*.exe' "
                   "-and $_.CommandLine -like '*upscale_movies.py*' } "
                   "| Select-Object -ExpandProperty ProcessId")
        r = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True, check=True)
        return [int(p) for p in r.stdout.split() if int(p) != os.getpid()]
    result = []
    for entry in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            args = entry.read_bytes().split(b"\0")
            if int(entry.parent.name) != os.getpid() and any(Path(os.fsdecode(a)).name == "upscale_movies.py" for a in args[1:] if a):
                result.append(int(entry.parent.name))
        except (OSError, ValueError):
            pass
    return result


@contextlib.contextmanager
def worker_lock(output):
    output.mkdir(parents=True, exist_ok=True)
    handle = (output / ".upscale.lock").open("a+b")
    locked = False
    try:
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            # Keep a real byte so Windows can lock even a fresh file.
            if not handle.read(1):
                handle.write(b"\0")
                handle.flush()
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        locked = True
        workers = legacy_workers()
        if workers:
            raise RuntimeError(f"another video upscale worker is active (PID {workers}); let it checkpoint/exit first")
        yield
    finally:
        if locked:
            if os.name == "nt":
                import msvcrt
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_UN)
        handle.close()


def movie_paths(only=""):
    def rank(path):
        rel = path.relative_to(ASSETS).as_posix().lower()
        return next((i for i, key in enumerate(PRIORITY) if key in rel), len(PRIORITY)), path.stat().st_size
    return [p for p in sorted(ASSETS.rglob("*.bik"), key=rank) if only.lower() in p.relative_to(ASSETS).as_posix().lower()]


def write_report(path, report):
    report["updated_at"] = datetime.now(timezone.utc).isoformat()
    if path is not None:
        path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(path, json.dumps(report, indent=2) + "\n")
    return report


def marker_model(dst):
    marker = dst.with_suffix(".model.txt")
    return marker.read_text(encoding="utf-8").strip() if marker.exists() else "unrecorded"


def status(ff, movies, model, report_path=None, require_original_provenance=False):
    rows = []
    for src in movies:
        rel = src.relative_to(ASSETS).as_posix().lower()
        dst = (OUT / rel).with_suffix(".mp4")
        made = marker_model(dst)
        info = probe(ff, src)
        try:
            saved = json.loads(dst.with_suffix(".upscale.json").read_text(encoding="utf-8"))
        except (OSError, ValueError, TypeError):
            saved = {}
        needs_original = require_original_provenance and not original_provenance(saved)
        rows.append({"movie": rel, "frames": info["frames"], "published": dst.is_file(), "recorded_model": made,
                     "needs_model": made != model or not dst.is_file(), "needs_original_provenance": needs_original,
                     "has_provenance": dst.with_suffix(".upscale.json").exists()})
    def needed(row):
        return row["needs_model"] or row["needs_original_provenance"]
    remaining = sum(row["frames"] or 0 for row in rows if needed(row))
    report = {"mode": "marker-inventory", "requested_model": model, "movies": len(rows),
              "require_original_provenance": bool(require_original_provenance),
              "covers_all_sources": len(movies) == len(movie_paths()), "all_requested_model_validated": False,
              "all_source_movies_validated": False,
              "all_original_source_and_weights_verified": False,
              "recorded_complete": sum(not needed(r) for r in rows), "remaining_frames": remaining,
              "estimated_hours_at_2_6_seconds_per_frame": remaining * 2.6 / 3600,
              "note": "Marker inventory only; not a validation result or guaranteed ETA.", "details": rows}
    print(json.dumps(write_report(report_path, report), indent=2))


def run_report(args, rows, mode):
    strict = bool(getattr(args, "require_original_provenance", False))
    def accepted(row):
        return row["status"] == "verified" and (not strict or original_provenance(row))
    verified = sum(accepted(row) for row in rows)
    covers_all = len(rows) == len(movie_paths())
    all_verified = bool(rows) and verified == len(rows)
    report = {"mode": mode, "requested_model": args.model, "movies": len(rows),
              "cooperative_pause_enabled": getattr(args, "pause_file", None) is not None,
              "inference_batch_limit": PAUSE_CHUNK if getattr(args, "pause_file", None) is not None else CHUNK,
              "require_original_provenance": strict,
              "covers_all_sources": covers_all, "verified": verified,
              "all_requested_model_validated": all_verified,
              "all_source_movies_validated": covers_all and all_verified,
              "all_original_source_and_weights_verified": covers_all and all_verified and all(original_provenance(row) for row in rows),
              "failed": sum(row["status"] == "failed" for row in rows),
              "remaining_frames": sum(row["frames"] for row in rows if not accepted(row)),
              "details": rows}
    return write_report(args.report, report)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="", help="only movies whose path contains this")
    ap.add_argument("--model", default="realesrgan-x4plus", choices=sorted(MODELS))
    ap.add_argument("--force", action="store_true", help="redo even verified outputs made with this recipe")
    ap.add_argument("--status", action="store_true", help="read-only marker/frame inventory; no GPU")
    ap.add_argument("--audit-only", action="store_true", help="decode/validate published files; no GPU or file changes")
    ap.add_argument("--require-original-provenance", action="store_true",
                    help="regenerate/withhold legacy pictures lacking verified original-Bink and current model-weight provenance")
    ap.add_argument("--max-movies", type=int, default=0, help="exit after this many newly completed movies (0 = all)")
    ap.add_argument("--stop-file", type=Path, default=ROOT / ".local" / "stop-video-upscale")
    ap.add_argument("--pause-file", type=Path, help="optional cooperative game-pause presence marker; inference batches are bounded to8 frames")
    ap.add_argument("--pause-owner-pid", type=int, help="optional live parent PID; pin a read-only Windows handle and stop if this owner exits")
    ap.add_argument("--report", type=Path, help="atomic machine-readable coverage/progress JSON (also with --status/--audit-only)")
    args = ap.parse_args()
    if args.max_movies < 0:
        ap.error("--max-movies cannot be negative")
    if args.pause_owner_pid is not None and (args.pause_file is None or args.pause_owner_pid <= 0):
        ap.error("--pause-owner-pid requires --pause-file and a positive parent PID")
    ff = ffmpeg_bin()
    movies = movie_paths(args.only)
    if not movies:
        print("No original Bink movies matched; verify assets/video and --only", file=sys.stderr)
        return 1
    if args.status:
        status(ff, movies, args.model, args.report, args.require_original_provenance)
        return 0
    failures, completed = 0, 0
    rows = [{"movie": p.relative_to(ASSETS).as_posix().lower(), "frames": bink_header(p)["frames"],
             "status": "pending", "output_validated": False, "recorded_model": "unrecorded"} for p in movies]
    run_report(args, rows, "audit" if args.audit_only else "upscale")
    if args.audit_only:
        identity = model_identity(args.model) if args.require_original_provenance else None
        for src, row in zip(movies, rows):
            dst = (OUT / src.relative_to(ASSETS).as_posix().lower()).with_suffix(".mp4")
            try:
                source = probe(ff, src)
                row["output"] = validate_movie(ff, dst, source, source["frames"])
                row["output_validated"] = True
                row["recorded_model"] = marker_model(dst)
                meta_path = dst.with_suffix(".upscale.json")
                saved = {}
                if meta_path.exists():
                    saved = json.loads(meta_path.read_text(encoding="utf-8"))
                    if saved["output_sha256"] != sha256(dst) or saved["recipe"]["source_sha256"] != sha256(src):
                        raise ValueError("published movie disagrees with its source/output provenance hashes")
                    row["recorded_model"] = saved["recipe"]["model"]["name"]
                    row["historical_source_and_weights_verified"] = saved["historical_source_and_weights_verified"]
                    row["origin"] = saved.get("origin", "unrecorded")
                else:
                    row["historical_source_and_weights_verified"] = False
                    row["origin"] = "unrecorded"
                row["status"] = "verified" if row["recorded_model"] == args.model else "model-pending"
                if args.require_original_provenance and row["status"] == "verified":
                    current_recipe = recipe_for(src, source, identity)
                    if not (original_provenance(row) and saved.get("recipe") in
                            (current_recipe, previous_timing_recipe(current_recipe))):
                        row["status"] = "provenance-pending"
                print(f"valid: {src.relative_to(ASSETS)} ({row['recorded_model']})", flush=True)
            except Exception as e:
                failures += 1
                row.update(status="failed", error=str(e))
                print(f"failed: {dst}: {e}", flush=True)
            report = run_report(args, rows, "audit")
        report = run_report(args, rows, "audit")
        return 1 if failures else (0 if report["all_requested_model_validated"] else 2)
    identity = model_identity(args.model)
    with worker_lock(OUT), contextlib.ExitStack() as resources:
        owner = None
        if args.pause_owner_pid is not None:
            try:
                owner = resources.enter_context(PauseOwner(args.pause_owner_pid))
            except MovieStopped as error:
                rows[0].update(status="pending", cooperative_stop=True, stop_reason=str(error), restart_inflight_on_resume=True)
                run_report(args, rows, "upscale")
                print(f"Stopped before inference: {error}", flush=True)
                return 2
        for src, row in zip(movies, rows):
            if args.stop_file.exists():
                print(f"Stopped at movie boundary: remove {args.stop_file} to resume", flush=True)
                break
            rel = src.relative_to(ASSETS).as_posix().lower()
            dst = (OUT / rel).with_suffix(".mp4")
            try:
                if args.pause_file is not None:
                    cooperate(args.pause_file, args.stop_file, owner)
                source = probe(ff, src)
                recipe = recipe_for(src, source, identity)
                if not args.force and current_output(ff, src, dst, source, recipe,
                                                      require_original_provenance=args.require_original_provenance):
                    saved = json.loads(dst.with_suffix(".upscale.json").read_text(encoding="utf-8"))
                    row.update(status="verified", output_validated=True, recorded_model=args.model,
                               historical_source_and_weights_verified=saved.get("historical_source_and_weights_verified", False),
                               origin=saved.get("origin", "unrecorded"))
                    run_report(args, rows, "upscale")
                    continue
                print(f"{rel} -> {dst.relative_to(OUT)}", flush=True)
                start = time.monotonic()
                controls = {"pause_file": args.pause_file, "stop_file": args.stop_file, "pause_owner": owner} if args.pause_file is not None else {}
                n = upscale_movie(ff, src, dst, args.model, source, recipe, **controls)
                completed += 1
                row.update(status="verified", output_validated=True, recorded_model=args.model,
                           historical_source_and_weights_verified=True, origin="original-bink")
                run_report(args, rows, "upscale")
                print(f"  done, {n} frames, {time.monotonic() - start:.1f} s", flush=True)
                if args.max_movies and completed >= args.max_movies:
                    break
            except MovieStopped as error:
                row.update(status="pending", output_validated=False, cooperative_stop=True,
                           stop_reason=str(error), restart_inflight_on_resume=True)
                run_report(args, rows, "upscale")
                print(f"  stopped {rel}: {error}", flush=True)
                break
            except Exception as e:
                failures += 1
                row.update(status="failed", error=str(e))
                run_report(args, rows, "upscale")
                print(f"  failed {rel}: {e}", flush=True)
    report = run_report(args, rows, "upscale")
    return 1 if failures else (0 if report["all_requested_model_validated"] else 2)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"video upscale: {error}", file=sys.stderr)
        sys.exit(1)
