#!/usr/bin/env python3
"""Makes a player's install look like the developer's: HD textures, PBR materials and HD movies, built on this PC from
the player's own copy of the game (nothing of the game's is downloaded or shipped).

The release puts this beside its own Python, the Real-ESRGAN upscaler, the PBR models and ffmpeg:

  <install>\\mc3.exe
  <install>\\Make HD.cmd                       double-click: runs this in a window
  <install>\\enhance\\python\\python.exe
  <install>\\enhance\\tools\\enhance\\build_hd.py  (this file; the other tools beside it as in the repository)

Steps, each resumable (close the window any time; the next run carries on where it stopped):
  1. copy the files the steps read off the disc image named by config\\disc-path.txt (or a game folder) into work\\
  2. every texture on the disc -> textures\\source            (tools/texture_pack/extract_all.py)
  3. HD textures, Real-ESRGAN UltraSharp 4x -> textures\\replace (tools/texture_pack/upscale.py)
  4. PBR maps for every city and prop texture -> textures\\pbr  (tools/pbr/generate.py --set sets/hd_release.json)
  5. the PBR pack -> pbr-cities, named in config\\hd-pack.txt, which the game adopts on its next start
  6. HD movies -> video_hd                                      (tools/video/upscale_movies.py)

From a source checkout (for testing): py -3.12 -I tools/enhance/build_hd.py --native DIR [--disc ISO] [--only N]
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]                      # enhance\ in a release, the repository in a checkout
TOOLS = ROOT / "tools"
STEPS = ("Copying the game files from your disc", "Finding every texture", "Making the textures HD",
         "Making the PBR materials (bumps, shine)", "Building the PBR pack", "Making the movies HD")
NEEDED = re.compile(r"(?i)^(assets_x\.dat.*|.*\.(xbck|xbpf|xpr|bik))$")
# the movie step's decoder/encoder (Bink in, H.264 out), fetched when that step first runs: a fixed versioned build
FFMPEG_URL = "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip"
FFMPEG_SHA256 = "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"


def say(text=""):
    print(text, flush=True)


def ensure_ffmpeg(folder):
    """ffmpeg.exe and ffprobe.exe in folder: the checkout's own when on PATH, else the pinned build, SHA-256 checked."""
    if (folder / "ffmpeg.exe").exists() and (folder / "ffprobe.exe").exists():
        return True
    if shutil.which("ffmpeg") and shutil.which("ffprobe"):
        return True
    import hashlib
    import urllib.request
    import zipfile
    folder.mkdir(parents=True, exist_ok=True)
    part = folder / "ffmpeg.zip.part"
    say("    downloading ffmpeg (115 MB, once)")
    urllib.request.urlretrieve(FFMPEG_URL, part)
    with open(part, "rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != FFMPEG_SHA256:
            part.unlink()
            say("    the ffmpeg download did not match its checksum")
            return False
    with zipfile.ZipFile(part) as z:
        for info in z.infolist():
            name = info.filename.rsplit("/", 1)[-1]
            if name in ("ffmpeg.exe", "ffprobe.exe", "LICENSE"):
                (folder / name).write_bytes(z.read(info))
    part.unlink()
    return True


def default_native():
    install = ROOT.parent
    return install if (install / "mc3.exe").exists() else None


def find_game(native, disc):
    """(assets folder, disc image or None): a game folder beside mc3.exe (or above it), else the disc image."""
    if disc:
        return None, Path(disc)
    for d in (native, *native.parents):
        if (d / "assets" / "default.xbe").exists():
            return d / "assets", None
    note = native / "config" / "disc-path.txt"
    if note.exists():
        p = Path(note.read_text(encoding="utf-8").strip().strip('"'))
        if p.is_file():
            return None, p
        if (p / "default.xbe").exists():
            return p, None
    hits = sorted(native.glob("*.iso"))
    return (None, hits[0]) if hits else (None, None)


def copy_from_disc(iso_path, out):
    sys.path.insert(0, str(TOOLS / "texture_pack"))
    import xiso
    iso = xiso.XIso(iso_path)
    files = [(p, sz) for p, sz in iso.walk() if NEEDED.match(p.rsplit("/", 1)[-1])]
    total = sum(sz for _, sz in files)
    done = 0
    for i, (p, sz) in enumerate(sorted(files)):
        dst = out / p
        if not (dst.exists() and dst.stat().st_size == sz):
            dst.parent.mkdir(parents=True, exist_ok=True)
            data = iso.read(p)
            tmp = dst.with_name(dst.name + ".part")
            tmp.write_bytes(data)
            tmp.replace(dst)
        done += sz
        if i % 50 == 0 or i == len(files) - 1:
            say(f"    {done * 100 // max(total, 1):3d}%  ({done >> 20} of {total >> 20} MB)")


class PlayGuard(threading.Thread):
    """While the game (any mc3.exe) runs, every tool under this process is suspended, so the GPU and CPU stay the
    game's and play never stutters; they carry on when it closes. Windows only (ToolHelp snapshot, NtSuspendProcess)."""

    def __init__(self):
        super().__init__(daemon=True)
        self.paused = set()
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.nt = ctypes.WinDLL("ntdll")
        self.k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
        self.k32.OpenProcess.restype = ctypes.c_void_p

    def processes(self):
        """pid -> (parent pid, exe name)"""
        class Entry(ctypes.Structure):
            _fields_ = [("dwSize", ctypes.c_ulong), ("cntUsage", ctypes.c_ulong), ("th32ProcessID", ctypes.c_ulong),
                        ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", ctypes.c_ulong),
                        ("cntThreads", ctypes.c_ulong), ("th32ParentProcessID", ctypes.c_ulong),
                        ("pcPriClassBase", ctypes.c_long), ("dwFlags", ctypes.c_ulong),
                        ("szExeFile", ctypes.c_wchar * 260)]
        snap = self.k32.CreateToolhelp32Snapshot(2, 0)  # TH32CS_SNAPPROCESS
        out = {}
        e = Entry()
        e.dwSize = ctypes.sizeof(Entry)
        ok = self.k32.Process32FirstW(ctypes.c_void_p(snap), ctypes.byref(e))
        while ok:
            out[e.th32ProcessID] = (e.th32ParentProcessID, e.szExeFile)
            ok = self.k32.Process32NextW(ctypes.c_void_p(snap), ctypes.byref(e))
        self.k32.CloseHandle(ctypes.c_void_p(snap))
        return out

    def suspend(self, pid, on):
        h = self.k32.OpenProcess(0x0800, False, pid)  # PROCESS_SUSPEND_RESUME
        if h:
            (self.nt.NtSuspendProcess if on else self.nt.NtResumeProcess)(ctypes.c_void_p(h))
            self.k32.CloseHandle(ctypes.c_void_p(h))

    def tick(self):
        procs = self.processes()
        game = any(name.lower() == "mc3.exe" for _, name in procs.values())
        tree, frontier = set(), {os.getpid()}
        while frontier:
            frontier = {pid for pid, (parent, _) in procs.items() if parent in frontier and pid not in tree}
            tree |= frontier
        if game:
            new = tree - self.paused
            if new and not self.paused:
                say("    Paused while you play the game. It carries on by itself when you close the game.")
            for pid in new:
                self.suspend(pid, True)
            self.paused |= new
        elif self.paused:
            for pid in self.paused:
                self.suspend(pid, False)
            self.paused.clear()
            say("    The game is closed: carrying on.")

    def run(self):
        while True:
            try:
                self.tick()
            except Exception:  # a process that ended between the snapshot and the call
                pass
            time.sleep(3)


def run(args, env, log):
    """A tool's output goes to the log; its progress lines are summarised on screen."""
    with open(log, "a", encoding="utf-8") as f:
        f.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(map(str, args))}\n")
        f.flush()
        p = subprocess.Popen([sys.executable, "-I", "-X", "utf8", *map(str, args)], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        last = 0.0
        for line in p.stdout:
            f.write(line)
            f.flush()
            m = re.search(r"\[(\d+)/(\d+)\]|(\d+)/(\d+) (?:done|textures|movies)", line)
            if m and time.time() - last > 5:
                a, b = (m.group(1), m.group(2)) if m.group(1) else (m.group(3), m.group(4))
                say(f"    {int(a) * 100 // max(int(b), 1):3d}%  ({a} of {b})")
                last = time.time()
        return p.wait()


def generate_pbr(env, log, threads):
    """generate.py over the release set, in parallel shards (each about 2 GB of memory at its largest texture)."""
    shards = max(1, min(3, (os.cpu_count() or 4) // 4))
    args = [TOOLS / "pbr" / "generate.py", "--set", TOOLS / "pbr" / "sets" / "hd_release.json",
            "--threads", str(max(1, threads // shards))]
    if shards == 1:
        return run(args, env, log)
    procs = []
    for k in range(shards):
        f = open(log.with_name(f"{log.stem}.pbr{k}.log"), "a", encoding="utf-8")
        procs.append((subprocess.Popen([sys.executable, "-I", "-X", "utf8", *map(str, args), "--shard", f"{k}/{shards}"],
                                       env=env, stdout=f, stderr=subprocess.STDOUT), f))
    total = len(json.load(open(TOOLS / "pbr" / "sets" / "hd_release.json", encoding="utf-8"))["textures"])
    pbr = Path(env["MC3_NATIVE"]) / "textures" / "pbr"
    while any(p.poll() is None for p, _ in procs):
        time.sleep(15)
        done = len(list(pbr.glob("*_n.png")))
        say(f"    {min(done, total) * 100 // total:3d}%  ({min(done, total)} of {total})")
    for _, f in procs:
        f.close()
    codes = [p.returncode for p, _ in procs]
    if any(codes):
        return max(codes)
    return run([TOOLS / "pbr" / "generate.py", "--merge"], env, log)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--native", type=Path, default=default_native(), help="the folder holding mc3.exe")
    ap.add_argument("--disc", type=Path, help="the disc image (default: the one the game remembers)")
    ap.add_argument("--only", type=int, choices=range(1, len(STEPS) + 1), help="run this step alone")
    ap.add_argument("--skip-movies", action="store_true", help="stop after the PBR pack")
    ap.add_argument("--keep-work", action="store_true", help="keep the copied game files when done")
    ap.add_argument("--no-pause", action="store_true", help="keep working while the game runs")
    a = ap.parse_args()
    if not a.native or not (a.native / "mc3.exe").exists() and not a.only:
        sys.exit("Could not find mc3.exe. Keep the enhance folder next to mc3.exe.")
    native = a.native.resolve()
    if os.name == "nt":  # one builder at a time; the game (hd_build.cpp) checks it before offering to start one
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateMutexW.restype = ctypes.c_void_p
        mutex = kernel32.CreateMutexW(None, True, "Local\\MC3MakeHD")  # held until this process exits
        if mutex and ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
            sys.exit("Make HD is already running in another window.")
    work = ROOT / "work"
    log = ROOT / "build_hd.log"
    state_path = ROOT / "state.json"
    state = json.loads(state_path.read_text()) if state_path.exists() else {}

    say("=" * 64)
    say("  MIDNIGHT CLUB 3 - MAKE IT HD")
    say("  This makes the textures, materials and movies HD on this PC.")
    say("  It takes a few hours. It pauses while you play the game and")
    say("  carries on when you close it, so leaving it on overnight is best.")
    say("  You can close this window any time: double-click 'Make HD'")
    say("  again later and it carries on where it stopped.")
    say("=" * 64)
    assets, iso = find_game(native, a.disc)
    if not assets and not (iso and iso.exists()):
        sys.exit("Could not find your game. Start mc3.exe once and pick your disc image, then try again.")

    env = dict(os.environ, MC3_NATIVE=str(native), MC3_ASSETS=str(assets or work / "assets"), PYTHONUTF8="1")
    ffmpeg = ROOT / "ffmpeg"
    if ffmpeg.is_dir():
        env["PATH"] = str(ffmpeg) + os.pathsep + env.get("PATH", "")
    threads = max(1, (os.cpu_count() or 4) - 2)
    if os.name == "nt" and not a.no_pause:
        PlayGuard().start()
    steps = range(1, len(STEPS) + 1) if not a.only else [a.only]
    if a.skip_movies:
        steps = [s for s in steps if s != 6]
    for n in steps:
        if state.get(str(n)) == "done" and not a.only:
            say(f"[{n}/{len(STEPS)}] {STEPS[n - 1]}: already done")
            continue
        say(f"[{n}/{len(STEPS)}] {STEPS[n - 1]}...")
        t = time.time()
        code = 0
        if n == 1:
            if iso:
                copy_from_disc(iso, work / "assets")
        elif n == 2:
            code = run([TOOLS / "texture_pack" / "extract_all.py", *(["--iso", iso] if iso else [])], env, log)
        elif n == 3:
            code = run([TOOLS / "texture_pack" / "upscale.py"], env, log)
        elif n == 4:
            code = generate_pbr(env, log, threads)
        elif n == 5:
            out = subprocess.run([sys.executable, "-I", "-X", "utf8", TOOLS / "pbr" / "test_pack.py", "--cities",
                                  "--native", native], env=env, capture_output=True, text=True)
            with open(log, "a", encoding="utf-8") as f:
                f.write(out.stdout + out.stderr)
            m = re.search(r"approved_manifest_sha256 = ([0-9a-f]{64})", out.stdout)
            code = out.returncode or (0 if m else 1)
            if m:
                (native / "config").mkdir(exist_ok=True)
                note = native / "config" / "hd-pack.txt"
                note.write_text(f"pack_path = pbr-cities\nsha256 = {m.group(1)}\n", encoding="utf-8")
                say("    the game turns the PBR pack on the next time it starts")
        elif n == 6:
            if not ensure_ffmpeg(ffmpeg):
                code = 1
            else:
                env["PATH"] = str(ffmpeg) + os.pathsep + env.get("PATH", "")
            code = code or run([TOOLS / "video" / "upscale_movies.py", "--model", "remacri-4x"], env, log)
        if code:
            say(f"    That step stopped (code {code}). The details are in {log}.")
            say("    Double-click 'Make HD' to try again; finished work is kept.")
            sys.exit(code)
        state[str(n)] = "done"
        state_path.write_text(json.dumps(state))
        say(f"    done in {int(time.time() - t) // 60} min")
    if not a.only and all(state.get(str(n)) == "done" for n in range(1, 6)):
        if not a.keep_work and (a.skip_movies is False and state.get("6") == "done"):
            shutil.rmtree(work, ignore_errors=True)
        (ROOT / "done.txt").write_text(time.strftime("%Y-%m-%d %H:%M:%S") + "\n")
        say("")
        say("ALL DONE! Start the game to see it in HD.")


if __name__ == "__main__":
    main()
