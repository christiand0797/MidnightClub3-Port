#!/usr/bin/env python3
"""Run the C++ radial-fit helper against immutable, original-format ordinary tire vertices.

The caller supplies a compiled tests/radial_fit_test.cpp executable. No game, GPU, asset or settings edits occur.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

import tire_mesh_support as tire


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def original_ratios(path: Path) -> tuple[float, ...]:
    data = path.read_bytes()
    if data[:4] != b"XBEH":
        raise ValueError("not an XBE")
    base, count, table = (tire.u32(data, x) for x in (0x104, 0x11C, 0x120))
    tire.checked(data, table-base, count*56, "XBE sections")
    for i in range(count):
        va, _, offset, size = struct.unpack_from("<IIII", data, table-base+i*56+4)
        if va <= 0x471958 and 0x471958+32 <= va+size:
            at = offset+0x471958-va
            tire.checked(data, at, 32, "profile ratios")
            result = struct.unpack_from("<8f", data, at)
            if not all(0 < x < 1 for x in result):
                raise ValueError("unsupported stock profile ratios")
            return result
    raise ValueError("profile ratio table not in an XBE raw section")


def input_fixture(pool: Path, xbe: Path, garage: Path) -> tuple[bytes, dict]:
    data, records, map_bytes = tire.load_pool(pool)
    stock = original_ratios(xbe)
    source = garage.read_text()
    match = re.search(r"kExtraProfileRatios1000\[\]\s*=\s*\{([^}]+)\}", source)
    if not match:
        raise ValueError("extended profile catalogue missing")
    extra = tuple(int(x)/1000 for x in re.findall(r"\d+", match.group(1)))
    if len(extra) != 26 or not all(0 < x < 1 for x in extra):
        raise ValueError("unsupported extended profile catalogue")
    target = stock+extra
    geometries = []
    source_profiles = []
    for record in records:
        if record.kind != "drwShaderModel":
            continue
        textures = [r.name for r in records if r.page == record.page and r.kind == "mip0"]
        if not textures or not textures[0].startswith("whl_tir_"):
            continue
        for geometry in tire.decode_record(data, record):
            minimum = min((p[1]**2+p[2]**2)**.5 for p in geometry.positions)
            index = min(range(8), key=lambda i: abs(stock[i]*.5-minimum))
            if abs(stock[index]*.5-minimum) >= 1.e-5:
                raise ValueError("ordinary bead does not match a stock ratio")
            if geometry.slot >= 2 or geometry.stride != 28:
                raise ValueError("unsupported ordinary draw-slot layout")
            at = record.start+geometry.vertex_offset
            raw = data[at:at+len(geometry.positions)*28]
            geometries.append(struct.pack("<III", index, geometry.slot, len(geometry.positions))+raw)
            source_profiles.append({"page": record.page, "model": record.name, "slot": geometry.slot,
                                    "stock_profile": index, "vertices": len(geometry.positions),
                                    "bead_minimum": minimum, "vertex_sha256": hashlib.sha256(raw).hexdigest()})
    if len(geometries) != 176:
        raise ValueError("expected 176 ordinary tire draw geometries")
    binary = b"MC3RFIT1"+struct.pack("<8f34fI", *stock, *target, len(geometries))+b"".join(geometries)
    report = {"format": "mc3-original-tire-bead-v1", "pool_sha256": hashlib.sha256(data).hexdigest(),
              "map_sha256": hashlib.sha256(map_bytes).hexdigest(), "xbe_sha256": sha(xbe),
              "garage_source_sha256": sha(garage), "stock_ratios": stock, "requested_ratios": target,
              "fixture_sha256": hashlib.sha256(binary).hexdigest(), "geometry": source_profiles}
    return binary, report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True, help="new JSON proof, never overwritten")
    parser.add_argument("--root", type=Path, default=tire.ROOT)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("report already exists")
    root = args.root.resolve()
    binary, report = input_fixture(root/"templates/game/assets/resources/vehicle/tire.xbpf",
                                   root/"templates/game/assets/default.xbe",
                                   root/"templates/game/src/game/garage.cpp")
    with tempfile.TemporaryDirectory(prefix="mc3-radial-fixture-") as directory:
        path = Path(directory)/"original-vertices.bin"
        path.write_bytes(binary)
        completed = subprocess.run([str(args.executable.resolve()), "--original", str(path)],
                                   capture_output=True, text=True, timeout=60)
    report.update(checked_at=datetime.now(timezone.utc).isoformat(), executable_sha256=sha(args.executable),
                  helper_sha256=sha(root/"runtime/gpu/radial_fit.h"), stdout=completed.stdout,
                  stderr=completed.stderr, exit_code=completed.returncode)
    if completed.returncode:
        print(completed.stdout+completed.stderr)
        return completed.returncode
    unchanged = (sha(root/"templates/game/assets/resources/vehicle/tire.xbpf") == report["pool_sha256"]
                 and sha(root/"templates/game/assets/resources/vehicle/tire.xbpmap") == report["map_sha256"]
                 and sha(root/"templates/game/assets/default.xbe") == report["xbe_sha256"])
    if not unchanged:
        raise RuntimeError("original assets changed while checking; no successful proof published")
    report["original_assets_sha256_unchanged"] = True
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(completed.stdout, end="")
    print(f"Immutable source proof -> {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
