#!/usr/bin/env python3
"""Verify the versioned original-disc volumes; optionally restore their exact ISO."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NAME = "Midnight Club 3 - DUB Edition Remix (USA).xiso.iso"
EXPECTED = "d58490dc8e1837e9f47716268b52ae1f1ac8578e5a5e42e061fa7f1c4b068b52"
SIZE = 4840554496


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extract", action="store_true")
    args = parser.parse_args()
    folder = ROOT / "game-data/original"
    manifest = json.loads((folder / "disc-manifest.json").read_text(encoding="utf-8"))
    if (manifest["filename"], manifest["size"], manifest["sha256"]) != (NAME, SIZE, EXPECTED):
        raise ValueError("Unexpected original-disc identity")
    volumes = manifest["volumes"]
    if len(volumes) != 35:
        raise ValueError("Expected all 35 original-disc volumes")
    for number, volume in enumerate(volumes, 1):
        if volume["path"] != f"mc3-original-usa.7z.{number:03d}":
            raise ValueError("Unexpected or unordered archive volume name")
        part = folder / volume["path"]
        if part.stat().st_size != volume["size"] or digest(part) != volume["sha256"]:
            raise ValueError(f"Archive volume failed SHA-256: {part.name}")
        print(f"Verified volume {number}/35", flush=True)
    if not args.extract:
        return
    destination = ROOT / NAME
    if destination.exists():
        if destination.stat().st_size == SIZE and digest(destination) == EXPECTED:
            print("Original disc already matches; preserved.")
            return
        raise ValueError("A different disc image already exists; preserved without changes")
    sevenzip = shutil.which("7z") or shutil.which("7z.exe")
    if not sevenzip:
        raise RuntimeError("Install 7-Zip and add 7z.exe to PATH before restoration")
    temporary = ROOT / (NAME + ".restore-part")
    created = False
    try:
        with temporary.open("xb") as stream:
            created = True
            subprocess.run([sevenzip, "x", "-so", str(folder / volumes[0]["path"])], stdout=stream, check=True)
            stream.flush()
            os.fsync(stream.fileno())
        if temporary.stat().st_size != SIZE or digest(temporary) != EXPECTED:
            raise ValueError("Decompressed disc failed SHA-256")
        # Atomic creation without replacing a file that appeared during extraction.
        os.link(temporary, destination)
        print(f"Restored and verified {destination.name}")
    finally:
        if created:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
