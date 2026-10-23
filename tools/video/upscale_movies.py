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
"""
import argparse
import contextlib
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
ESRGAN_DIR = ROOT / "upscale_tools" / "realesrgan-20220424"
SCALE = 3
CHUNK = 400
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
        command.append("-count_frames")
    command += ["-show_entries", "stream=width,height,r_frame_rate,avg_frame_rate,nb_frames,nb_read_frames,codec_name,pix_fmt",
                "-of", "json", str(path)]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    # A decoder may exit successfully after reporting damaged frames. Do not
    # accept those clips merely because ffprobe still returned a stream record.
    if result.stderr.strip():
        raise RuntimeError(f"video decode errors in {path}: {result.stderr.strip()[-2000:]}")
    streams = json.loads(result.stdout).get("streams", [])
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
    return {"width": int(stream["width"]), "height": int(stream["height"]),
            "fps": str(fps), "frames": int(frames) if frames not in (None, "N/A") else None,
            "codec": stream.get("codec_name"), "pix_fmt": stream.get("pix_fmt")}


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


def validate_movie(ff, dst, source, expected_frames):
    info = probe(ff, dst, count_frames=True)
    validate_info(info, source, expected_frames)
    return info


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


def publish(part, dst, attempts=600, delay=1):
    # Only called after validation. The game can have the previous picture open.
    for attempt in range(attempts):
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
    return {"pipeline_version": PIPELINE_VERSION, "source_sha256": sha256(src),
            "source_bytes": src.stat().st_size, "source": source,
            "model": identity, "output_scale": SCALE,
            "encoder": {"codec": "libx264", "preset": "slow", "crf": 16, "pix_fmt": "yuv420p"}}


def current_output(ff, src, dst, source, recipe, adopt_legacy=True):
    if not dst.is_file():
        return False
    meta = dst.with_suffix(".upscale.json")
    if meta.exists():
        try:
            saved = json.loads(meta.read_text(encoding="utf-8"))
            return (saved["recipe"] == recipe and saved["output_sha256"] == sha256(dst))
        except (OSError, ValueError, KeyError, TypeError):
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


def upscale_movie(ff, src, dst, model, source=None, recipe=None, chunk_size=CHUNK):
    model_dir, model_scale, sharpen = MODELS[model]
    source = source or probe(ff, src)
    recipe = recipe or recipe_for(src, source, model_identity(model))
    w, h, fps = source["width"], source["height"], Fraction(source["fps"])
    W, H = w * SCALE, h * SCALE
    if W % 2 or H % 2 or chunk_size < 1:
        raise ValueError("H.264 yuv420p needs even dimensions; chunk size must be positive")
    dst.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f".{dst.stem}.", suffix=".part.mp4", dir=dst.parent)
    os.close(fd)
    part = Path(name)
    enc = None
    try:
        with tempfile.TemporaryDirectory(prefix="mc3movie_") as tmp_name:
            tmp = Path(tmp_name)
            lo = tmp / "lo"
            lo.mkdir()
            subprocess.run([binary(ff, "ffmpeg"), "-v", "error", "-xerror", "-i", str(src),
                            "-map", "0:v:0", "-an", "-fps_mode", "passthrough", str(lo / "f%06d.png")], check=True)
            frames = sorted(lo.glob("*.png"))
            if not frames or (source["frames"] is not None and len(frames) != source["frames"]):
                raise ValueError(f"Bink decode returned {len(frames)} frames; expected {source['frames']}")
            enc = subprocess.Popen([binary(ff, "ffmpeg"), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                    "-s", f"{W}x{H}", "-r", f"{fps.numerator}/{fps.denominator}", "-i", "-",
                                    "-an", "-vf", f"unsharp=5:5:{sharpen}", "-c:v", "libx264", "-preset", "slow",
                                    "-crf", "16", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(part)],
                                   stdin=subprocess.PIPE)
            for i in range(0, len(frames), chunk_size):
                cin, cout = tmp / "cin", tmp / "cout"
                for folder in (cin, cout):
                    if folder.exists():
                        shutil.rmtree(folder)
                    folder.mkdir()
                chunk = frames[i:i + chunk_size]
                for frame in chunk:
                    frame.rename(cin / frame.name)
                result = subprocess.run([str(ESRGAN_DIR / "realesrgan-ncnn-vulkan.exe"), "-i", str(cin), "-o", str(cout),
                                         "-n", model, "-s", str(model_scale), "-m", str(model_dir), "-f", "png", "-j", "1:2:2"],
                                        capture_output=True)
                if result.returncode:
                    raise RuntimeError(f"upscaler exited {result.returncode}: {result.stderr.decode(errors='replace')[-2000:]}")
                if {p.name for p in cout.glob("*.png")} != {p.name for p in chunk}:
                    raise ValueError("upscaler returned a missing or unexpected frame")
                for frame in chunk:
                    with Image.open(cout / frame.name) as image:
                        if image.size != (w * model_scale, h * model_scale):
                            raise ValueError(f"incorrect model output size: {frame.name}, {image.size}")
                        image = image.convert("RGB")
                        if image.size != (W, H):
                            image = image.resize((W, H), Image.Resampling.LANCZOS)
                        enc.stdin.write(image.tobytes())
                print(f"    {min(i + chunk_size, len(frames))}/{len(frames)}", flush=True)
            enc.stdin.close()
            if enc.wait():
                raise RuntimeError(f"encoding {dst} failed")
        info = validate_movie(ff, part, source, len(frames))
        if sha256(src) != recipe["source_sha256"]:
            raise ValueError("original movie changed while the upscale was running")
        # Windows FlushFileBuffers requires a handle opened for writing.
        with part.open("r+b") as f:
            os.fsync(f.fileno())
        record = {"recipe": recipe, "output": info, "output_sha256": sha256(part),
                  "origin": "original-bink", "historical_source_and_weights_verified": True,
                  "validated_at": datetime.now(timezone.utc).isoformat()}
        publish(part, dst)
        # JSON is authoritative for resume. Text markers remain for old tools.
        atomic_write(dst.with_suffix(".upscale.json"), json.dumps(record, indent=2) + "\n")
        atomic_write(dst.with_suffix(".model.txt"), model + "\n")
        return len(frames)
    finally:
        if enc is not None:
            stop_encoder(enc)
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


def status(ff, movies, model, report_path=None):
    rows = []
    for src in movies:
        rel = src.relative_to(ASSETS).as_posix().lower()
        dst = (OUT / rel).with_suffix(".mp4")
        made = marker_model(dst)
        info = probe(ff, src)
        rows.append({"movie": rel, "frames": info["frames"], "published": dst.is_file(), "recorded_model": made,
                     "needs_model": made != model or not dst.is_file(), "has_provenance": dst.with_suffix(".upscale.json").exists()})
    remaining = sum(row["frames"] or 0 for row in rows if row["needs_model"])
    report = {"mode": "marker-inventory", "requested_model": model, "movies": len(rows),
              "covers_all_sources": len(movies) == len(movie_paths()), "all_requested_model_validated": False,
              "all_source_movies_validated": False,
              "recorded_complete": sum(not r["needs_model"] for r in rows), "remaining_frames": remaining,
              "estimated_hours_at_2_6_seconds_per_frame": remaining * 2.6 / 3600,
              "note": "Marker inventory only; not a validation result or guaranteed ETA.", "details": rows}
    print(json.dumps(write_report(report_path, report), indent=2))


def run_report(args, rows, mode):
    verified = sum(row["status"] == "verified" for row in rows)
    covers_all = len(rows) == len(movie_paths())
    all_verified = bool(rows) and verified == len(rows)
    report = {"mode": mode, "requested_model": args.model, "movies": len(rows),
              "covers_all_sources": covers_all, "verified": verified,
              "all_requested_model_validated": all_verified,
              "all_source_movies_validated": covers_all and all_verified,
              "failed": sum(row["status"] == "failed" for row in rows),
              "remaining_frames": sum(row["frames"] for row in rows if row["status"] != "verified"),
              "details": rows}
    return write_report(args.report, report)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="", help="only movies whose path contains this")
    ap.add_argument("--model", default="realesrgan-x4plus", choices=sorted(MODELS))
    ap.add_argument("--force", action="store_true", help="redo even verified outputs made with this recipe")
    ap.add_argument("--status", action="store_true", help="read-only marker/frame inventory; no GPU")
    ap.add_argument("--audit-only", action="store_true", help="decode/validate published files; no GPU or file changes")
    ap.add_argument("--max-movies", type=int, default=0, help="exit after this many newly completed movies (0 = all)")
    ap.add_argument("--stop-file", type=Path, default=ROOT / ".local" / "stop-video-upscale")
    ap.add_argument("--report", type=Path, help="atomic machine-readable coverage/progress JSON (also with --status/--audit-only)")
    args = ap.parse_args()
    if args.max_movies < 0:
        ap.error("--max-movies cannot be negative")
    ff = ffmpeg_bin()
    movies = movie_paths(args.only)
    if not movies:
        print("No original Bink movies matched; verify assets/video and --only", file=sys.stderr)
        return 1
    if args.status:
        status(ff, movies, args.model, args.report)
        return 0
    failures, completed = 0, 0
    rows = [{"movie": p.relative_to(ASSETS).as_posix().lower(), "frames": bink_header(p)["frames"],
             "status": "pending", "output_validated": False, "recorded_model": "unrecorded"} for p in movies]
    run_report(args, rows, "audit" if args.audit_only else "upscale")
    if args.audit_only:
        for src, row in zip(movies, rows):
            dst = (OUT / src.relative_to(ASSETS).as_posix().lower()).with_suffix(".mp4")
            try:
                source = probe(ff, src)
                row["output"] = validate_movie(ff, dst, source, source["frames"])
                row["output_validated"] = True
                row["recorded_model"] = marker_model(dst)
                meta_path = dst.with_suffix(".upscale.json")
                if meta_path.exists():
                    saved = json.loads(meta_path.read_text(encoding="utf-8"))
                    if saved["output_sha256"] != sha256(dst) or saved["recipe"]["source_sha256"] != sha256(src):
                        raise ValueError("published movie disagrees with its source/output provenance hashes")
                    row["recorded_model"] = saved["recipe"]["model"]["name"]
                    row["historical_source_and_weights_verified"] = saved["historical_source_and_weights_verified"]
                else:
                    row["historical_source_and_weights_verified"] = False
                row["status"] = "verified" if row["recorded_model"] == args.model else "model-pending"
                print(f"valid: {src.relative_to(ASSETS)} ({row['recorded_model']})", flush=True)
            except Exception as e:
                failures += 1
                row.update(status="failed", error=str(e))
                print(f"failed: {dst}: {e}", flush=True)
            report = run_report(args, rows, "audit")
        report = run_report(args, rows, "audit")
        return 1 if failures else (0 if report["all_requested_model_validated"] else 2)
    identity = model_identity(args.model)
    with worker_lock(OUT):
        for src, row in zip(movies, rows):
            if args.stop_file.exists():
                print(f"Stopped at movie boundary: remove {args.stop_file} to resume", flush=True)
                break
            rel = src.relative_to(ASSETS).as_posix().lower()
            dst = (OUT / rel).with_suffix(".mp4")
            try:
                source = probe(ff, src)
                recipe = recipe_for(src, source, identity)
                if not args.force and current_output(ff, src, dst, source, recipe):
                    row.update(status="verified", output_validated=True, recorded_model=args.model)
                    run_report(args, rows, "upscale")
                    continue
                print(f"{rel} -> {dst.relative_to(OUT)}", flush=True)
                start = time.monotonic()
                n = upscale_movie(ff, src, dst, args.model, source, recipe)
                completed += 1
                row.update(status="verified", output_validated=True, recorded_model=args.model)
                run_report(args, rows, "upscale")
                print(f"  done, {n} frames, {time.monotonic() - start:.1f} s", flush=True)
                if args.max_movies and completed >= args.max_movies:
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
