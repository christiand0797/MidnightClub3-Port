#!/usr/bin/env python3
"""Inspect an Xbox XDVDFS image and verify extracted assets (Python 3.10+, stdlib).

The source image is always opened read-only. Extraction is limited to default.xbe;
an existing destination is accepted only when its complete contents match.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile


SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = (0, 0x18300000)
MAX_TABLE = 16 * 1024 * 1024
MAX_ENTRIES = 100000
CHUNK = 1024 * 1024


class DiscError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    path: str
    offset: int
    size: int
    directory: bool


def safe_name(raw):
    # XDVDFS names are byte strings; cp1252 preserves the Western Xbox filenames.
    try:
        name = raw.decode("cp1252")
    except UnicodeDecodeError as exc:
        raise DiscError("Unsupported filename encoding") from exc
    if (not name or name in (".", "..") or name[-1:] in (".", " ")
            or any(ord(c) < 32 or c in '/\\:\"<>|?*' for c in name)):
        raise DiscError(f"Unsafe Xbox filename: {raw!r}")
    stem = name.split(".", 1)[0].upper()
    if stem in {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1, 10)],
                *[f"LPT{i}" for i in range(1, 10)]}:
        raise DiscError(f"Filename cannot be represented safely on Windows: {name}")
    return name


class Xiso:
    def __init__(self, path):
        self.path = Path(path)
        self.file = None

    def __enter__(self):
        self.file = self.path.open("rb")
        self.size = os.fstat(self.file.fileno()).st_size
        try:
            for base in PARTITIONS:
                at = base + 32 * SECTOR
                if at + SECTOR > self.size:
                    continue
                volume = self.read(at, SECTOR)
                if volume[:20] == MAGIC and volume[0x7EC:0x800] == MAGIC:
                    self.base = base
                    root_sector, root_size = struct.unpack_from("<II", volume, 20)
                    self.entries = {}
                    self._tables = set()
                    self._directory("", base + root_sector * SECTOR, root_size, 0)
                    return self
            raise DiscError("No XDVDFS volume at a supported partition offset")
        except BaseException:
            self.file.close()
            self.file = None
            raise

    def __exit__(self, *_):
        if self.file is not None:
            self.file.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise DiscError(f"Image extent is out of bounds: {offset:#x} + {size:#x}")
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise DiscError("Source image was truncated during verification")
        return data

    def _directory(self, parent, offset, size, depth):
        if depth > 32 or size > MAX_TABLE:
            raise DiscError(f"Directory exceeds safety limits: {parent or '/'}")
        table = self.read(offset, size)
        if not size:
            return
        if (offset, size) in self._tables:
            raise DiscError(f"Directory table is aliased or cyclic: {parent or '/'}")
        self._tables.add((offset, size))
        todo, visited, occupied, children = [0], set(), [], []
        while todo:
            at = todo.pop()
            if at in visited:
                raise DiscError(f"Cyclic/repeated directory node: {parent or '/'} at {at}")
            visited.add(at)
            if at + 14 > size:
                raise DiscError(f"Directory node is out of bounds: {parent or '/'} at {at}")
            if table[at:at + 14] == b"\xff" * 14:
                continue
            left, right, sector, length, attributes, name_size = struct.unpack_from("<HHIIBB", table, at)
            end = at + 14 + name_size
            if end > size or any(at < stop and end > start for start, stop in occupied):
                raise DiscError(f"Truncated/overlapping directory entry: {parent or '/'} at {at}")
            occupied.append((at, end))
            name = safe_name(table[at + 14:end])
            path = f"{parent}/{name}" if parent else name
            key = path.casefold()
            if key in self.entries:
                raise DiscError(f"Duplicate case-insensitive disc path: {path}")
            entry = Entry(path, self.base + sector * SECTOR, length, bool(attributes & 0x10))
            if entry.offset + entry.size > self.size:
                raise DiscError(f"File/directory extends beyond the image: {path}")
            self.entries[key] = entry
            if len(self.entries) > MAX_ENTRIES:
                raise DiscError("Disc entry limit exceeded")
            if entry.directory:
                children.append(entry)
            if right:
                todo.append(right * 4)
            if left:
                todo.append(left * 4)
        for entry in children:
            self._directory(entry.path, entry.offset, entry.size, depth + 1)

    def chunks(self, entry):
        for position in range(0, entry.size, CHUNK):
            yield self.read(entry.offset + position, min(CHUNK, entry.size - position))

    def digests(self, entry):
        sha1, sha256 = hashlib.sha1(), hashlib.sha256()
        for chunk in self.chunks(entry):
            sha1.update(chunk)
            sha256.update(chunk)
        return {"sha1": sha1.hexdigest(), "sha256": sha256.hexdigest()}

    def xbe_info(self):
        entry = self.entries.get("default.xbe")
        if entry is None or entry.directory or entry.size < 0x11C:
            raise DiscError("No valid root default.xbe")
        header = self.read(entry.offset, 0x11C)
        if header[:4] != b"XBEH":
            raise DiscError("default.xbe has no XBE header")
        base = struct.unpack_from("<I", header, 0x104)[0]
        certificate = struct.unpack_from("<I", header, 0x118)[0] - base
        if certificate < 0 or certificate + 92 > entry.size:
            raise DiscError("XBE certificate is outside default.xbe")
        cert = self.read(entry.offset + certificate, 92)
        return {"size": entry.size, **self.digests(entry),
                "title_id": f"0x{struct.unpack_from('<I', cert, 8)[0]:08X}",
                "title": cert[12:92].decode("utf-16-le", errors="replace").rstrip("\0")}


def asset_inventory(root):
    root = Path(root)
    if not root.is_dir() or root.is_symlink():
        raise DiscError(f"Assets must be an ordinary directory: {root}")
    if getattr(root.lstat(), "st_file_attributes", 0) & 0x400:
        raise DiscError(f"Refusing asset root reparse point: {root}")
    entries = {}
    for directory, folders, files in os.walk(root, followlinks=False):
        for name in folders + files:
            path = Path(directory) / name
            mode = path.lstat().st_mode
            if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)) or path.is_symlink():
                raise DiscError(f"Refusing nonregular asset or symbolic link: {path}")
            if getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
                raise DiscError(f"Refusing asset reparse point: {path}")
            relative = path.relative_to(root).as_posix()
            key = relative.casefold()
            if key in entries:
                raise DiscError(f"Duplicate case-insensitive asset path: {relative}")
            entries[key] = (path, stat.S_ISDIR(mode))
    return entries


def verify_assets(disc, root, hashes=False):
    local = asset_inventory(root)
    problems, matched, hashed = [], 0, 0
    for key, entry in sorted(disc.entries.items()):
        found = local.get(key)
        if found is None:
            problems.append({"path": entry.path, "problem": "missing"})
            continue
        path, directory = found
        if directory != entry.directory:
            problems.append({"path": entry.path, "problem": "wrong type"})
            continue
        if directory:
            continue
        actual_size = path.stat().st_size
        if actual_size != entry.size:
            problems.append({"path": entry.path, "problem": "size mismatch",
                             "disc_size": entry.size, "asset_size": actual_size})
            continue
        if hashes:
            actual = hashlib.sha256()
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(CHUNK), b""):
                    actual.update(chunk)
            expected = disc.digests(entry)["sha256"]
            hashed += 1
            if actual.hexdigest() != expected:
                problems.append({"path": entry.path, "problem": "SHA-256 mismatch",
                                 "disc_sha256": expected, "asset_sha256": actual.hexdigest()})
                continue
        matched += 1
    for key, (path, _) in sorted(local.items()):
        if key not in disc.entries:
            problems.append({"path": path.relative_to(root).as_posix(), "problem": "extra"})
    return {"assets": str(Path(root).resolve()), "matched_files": matched,
            "hashed_files": hashed, "problems": problems, "ok": not problems}


def extract_xbe(disc, destination):
    entry = disc.entries["default.xbe"]
    destination = Path(destination)
    expected = disc.digests(entry)["sha256"]
    if destination.exists() or destination.is_symlink():
        if not destination.is_file() or destination.is_symlink():
            raise DiscError(f"Extraction destination is not an ordinary file: {destination}")
        if getattr(destination.lstat(), "st_file_attributes", 0) & 0x400:
            raise DiscError(f"Refusing extraction destination reparse point: {destination}")
        if destination.stat().st_size != entry.size:
            raise DiscError(f"Refusing to overwrite mismatched destination: {destination}")
        actual = hashlib.sha256(destination.read_bytes()).hexdigest()
        if actual != expected:
            raise DiscError(f"Refusing to overwrite mismatched destination: {destination}")
        return "already matches"
    # Creating the sibling temporary file also ensures the parent exists.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=".default.xbe-", delete=False) as target:
            temporary = Path(target.name)
            digest = hashlib.sha256()
            for chunk in disc.chunks(entry):
                target.write(chunk)
                digest.update(chunk)
            target.flush()
            os.fsync(target.fileno())
        if digest.hexdigest() != expected:
            raise DiscError("Source changed while extracting default.xbe")
        # A hard link atomically publishes the complete file and never replaces a
        # concurrently created destination (unlike os.replace on Unix).
        os.link(temporary, destination)
        return "extracted"
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--verify", action="store_true", help="compare all asset paths, types and sizes")
    parser.add_argument("--assets", type=Path, default=Path(__file__).resolve().parents[2] / "templates/game/assets")
    parser.add_argument("--hashes", action="store_true", help="also compare every asset with SHA-256; requires --verify")
    parser.add_argument("--expected-xbe-sha1", help="require this default.xbe SHA-1")
    parser.add_argument("--expected-title-id", type=lambda s: int(s, 0), help="require this XBE title ID, e.g. 0x54540079")
    parser.add_argument("--extract-xbe", type=Path, metavar="DESTINATION", help="atomically extract default.xbe without overwriting a different file")
    parser.add_argument("--report", type=Path, help="also save the JSON report")
    args = parser.parse_args(argv)
    if args.hashes and not args.verify:
        parser.error("--hashes requires --verify")
    try:
        with Xiso(args.iso) as disc:
            xbe = disc.xbe_info()
            if args.expected_xbe_sha1 and xbe["sha1"].lower() != args.expected_xbe_sha1.lower():
                raise DiscError("default.xbe does not match --expected-xbe-sha1")
            if args.expected_title_id is not None and int(xbe["title_id"], 0) != args.expected_title_id:
                raise DiscError("default.xbe does not match --expected-title-id")
            counts = Counter(entry.directory for entry in disc.entries.values())
            report = {"iso": str(args.iso.resolve()), "image_size": disc.size,
                      "partition_offset": disc.base, "files": counts[False], "directories": counts[True],
                      "file_bytes": sum(e.size for e in disc.entries.values() if not e.directory), "xbe": xbe}
            if args.extract_xbe:
                report["extract_xbe"] = {"destination": str(args.extract_xbe.resolve()),
                                         "result": extract_xbe(disc, args.extract_xbe)}
            if args.verify:
                report["verification"] = verify_assets(disc, args.assets, args.hashes)
            report["ok"] = report.get("verification", {}).get("ok", True)
        encoded = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        if args.report:
            if args.report.resolve() == args.iso.resolve():
                raise DiscError("Report path cannot be the source image")
            with args.report.open("x", encoding="utf-8") as output:
                output.write(encoded)
        print(encoded, end="")
        return 0 if report["ok"] else 1
    except (OSError, DiscError) as exc:
        print(f"verify_xiso: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
