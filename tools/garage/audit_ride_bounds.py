#!/usr/bin/env python3
"""Read-only original vehicle-bound/clearance inventory; does not run the game or repeat the solver sweep.

Only resource names, hashes and numeric measurements are emitted. No extracted retail bytes are written.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "texture_pack"))
from dave import Archive


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def checked(data: bytes, offset: int, count: int) -> int:
    if offset < 0 or count <= 0 or offset + count > len(data):
        raise ValueError("serialized range outside resource")
    return offset


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, checked(data, offset, 4))[0]


def layer_summary(vertices: tuple[tuple[float, float, float], ...]) -> dict:
    if not vertices or not all(math.isfinite(v) for row in vertices for v in row):
        raise ValueError("empty or nonfinite vertices")
    # SSE subss/addss in 121150 round the layer thresholds to float32; comparisons are STRICT.
    minimum = min(row[1] for row in vertices)
    epsilon = f32(.05)
    lower, upper = f32(minimum-epsilon), f32(minimum+epsilon)
    bottom = [i for i, row in enumerate(vertices) if lower < row[1] < upper]
    next_layer = [row for row in vertices if row[1] > upper]
    if not next_layer:
        raise ValueError("no strict upper layer")
    return {"minimum_y": minimum, "bottom_indices": bottom, "upper_threshold": upper,
            "cap_y": f32(min(row[1] for row in next_layer)-epsilon),
            "upper_half_x": max(abs(row[0]) for row in next_layer)}


def parse_bound(data: bytes) -> dict:
    checked(data, 0, 0x90)
    base = u32(data, 0)
    if u32(data, 0xC) != len(data)-0x80:
        raise ValueError("unsupported XBCK payload extent")

    def pointer(value: int, count: int) -> int:
        offset = value-base+0x80
        if offset < 0x80:
            raise ValueError("pointer outside payload")
        return checked(data, offset, count)

    description = pointer(u32(data, 0x88), 0x3820)
    bound = pointer(u32(data, 0x8C), 0x70)
    if data[bound+4] != 3:
        raise ValueError("unsupported bound type")
    count, faces = u32(data, bound+0x4C), u32(data, bound+0x50)
    if not 0 < count <= 0x10000 or not faces:
        raise ValueError("invalid geometry count")
    vertices_at = pointer(u32(data, bound+0x68), 12*count)
    polygons_at = pointer(u32(data, bound+0x6C), 0x20*faces)
    spans = [(bound, bound+0x70), (vertices_at, vertices_at+12*count),
             (polygons_at, polygons_at+0x20*faces)]
    if any(max(a, c) < min(b, d) for i, (a, b) in enumerate(spans) for c, d in spans[i+1:]):
        raise ValueError("overlapping geometry storage")
    vertices = tuple(struct.iter_unpack("<3f", data[vertices_at:vertices_at+12*count]))
    for i in range(faces):
        if max(struct.unpack_from("<4H", data, polygons_at+0x20*i+0x10)) >= count:
            raise ValueError("polygon index outside vertex array")
    radii = []
    for axle in range(2):
        node = pointer(u32(data, description+0x3818+4*axle), 0x18)
        radius = struct.unpack_from("<f", data, node+0x14)[0]
        if not math.isfinite(radius) or radius <= 0:
            raise ValueError("invalid original axle node radius")
        radii.append(radius)
    result = layer_summary(vertices)
    result.update({"type": u32(data, description+0x3808), "vertex_count": count, "polygon_count": faces,
                   "bound_file_offset": bound, "vertices_file_offset": vertices_at,
                   "vertex_sha256": sha(data[vertices_at:vertices_at+12*count]), "stock_axle_radii": radii})
    return result


def xbe_floats(data: bytes, address: int, count: int) -> tuple[float, ...]:
    if data[:4] != b"XBEH":
        raise ValueError("not an XBE")
    base, sections, table = (u32(data, at) for at in (0x104, 0x11C, 0x120))
    checked(data, table-base, sections*56)
    for i in range(sections):
        va, _, raw, size = struct.unpack_from("<4I", data, table-base+56*i+4)
        if va <= address and address+4*count <= va+size:
            at = checked(data, raw+address-va, count*4)
            out = struct.unpack_from("<"+"f"*count, data, at)
            if not all(math.isfinite(x) for x in out):
                raise ValueError("nonfinite XBE table")
            return out
    raise ValueError("XBE table not inside a raw section")


def scalar(text: str, key: str, default: float) -> tuple[float, bool]:
    match = re.search(r"\b"+re.escape(key)+r"\s+([-+0-9.eE]+)", text)
    value = f32(float(match[1])) if match else f32(default)
    if not math.isfinite(value):
        raise ValueError("nonfinite "+key)
    return value, match is not None


def audit(root: Path, assets: Path, prior: Path) -> dict:
    tunes, bounds = [], []
    for archive_path in sorted(assets.glob("assets_x.dat*")):
        if not archive_path.is_file() or not re.fullmatch(r"assets_x\.dat(?:_[0-9]+)?", archive_path.name):
            continue
        before = archive_path.stat()
        archive = Archive(archive_path)
        for entry in archive.entries:
            name = PurePosixPath(entry.name)
            is_tune = entry.name.startswith("tune/vehicle/") and name.suffix == ".vehsim"
            is_bound = (entry.name.startswith("resources/vehicle/") and name.suffix == ".xbck"
                        and name.stem in (name.parent.name, name.parent.name+"_g", name.parent.name+"_o"))
            if not (is_tune or is_bound):
                continue
            data = archive.read(entry)
            provenance = {"archive": archive_path.name, "name": entry.name, "sha256": sha(data), "bytes": len(data)}
            if is_bound:
                bounds.append({**provenance, "family": name.parent.name, **parse_bound(data)})
                continue
            text = data.decode("latin1")
            skin, authored = scalar(text, "BoundClearance", .05)
            offsets = []
            for axle in ("WheelFront", "WheelBack"):
                block = re.search(r"\b"+axle+r"\s*\{([^}]*)\}", text)
                if not block:
                    raise ValueError("missing wheel tune "+entry.name)
                offsets.append(scalar(block[1], "SuspensionOffset", 0)[0])
            air = re.search(r"\bAirRide\s*\{([^}]*)\}", text)
            if not air:
                raise ValueError("missing AirRide block "+entry.name)
            air_values = {key: scalar(air[1], key, 0)[0] for key in
                          ("FrontRise", "BackRise", "FrontDrop", "BackDrop", "RateDeflate", "RateInflate")}
            tunes.append({**provenance, "family": re.sub(r"_(base|mods)$", "", name.stem),
                          "clearance": skin, "clearance_authored": authored, "stock_offsets": offsets,
                          "airride_explicit": bool(air[1].strip()), "airride_authored_values": air_values})
        after = archive_path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("archive changed during audit")
    if len(tunes) != 188 or len(bounds) != 282:
        raise ValueError("unexpected original resource catalogue size")
    for row in bounds:
        related = [t for t in tunes if t["family"] == row["family"]]
        if len(related) != 2:
            raise ValueError("missing/duplicate base and mods clearance")
        row["clearances"] = sorted(set(t["clearance"] for t in related))
        row["skin_cap_margin"] = min(row["cap_y"]-f32(row["minimum_y"]+x) for x in row["clearances"])
        if row["type"] not in (2, 4) or len(row["bottom_indices"]) != (2 if row["type"] == 2 else 4):
            raise ValueError("unsupported original bottom-layer classification")
        if row["type"] == 4 and row["skin_cap_margin"] < 0:
            raise ValueError("authored skin exceeds original cap")
    xbe = (assets/"default.xbe").read_bytes()
    garage_path = root/"templates/game/src/game/garage.cpp"
    garage = garage_path.read_bytes()
    match = re.search(rb"kExtraProfileRatios1000\[\]\s*=\s*\{([^}]+)\}", garage)
    if not match:
        raise ValueError("missing extended profile table")
    profiles = list(xbe_floats(xbe, 0x471958, 8))+[f32(int(x)/1000) for x in re.findall(rb"\d+", match[1])]
    rims = xbe_floats(xbe, 0x4B3910+12*4, 17)
    if len(profiles) != 34 or not all(0 < x < 1 for x in profiles) or not all(x > 0 for x in rims):
        raise ValueError("unexpected profile/rim table")
    radii = [(f32(f32(ratio/profile)*.5), rim, index) for rim, ratio in enumerate(rims, 12)
             for index, profile in enumerate(profiles)]
    prior_data = prior.read_bytes()
    if b"Verified 188 original vehicle tunes, both axles, 20304 opposite-side render queries" not in prior_data:
        raise ValueError("prior catalogue evidence does not identify the expected sweep")
    return {"schema": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
            "scope": "read-only source metadata; no game/GPU, no repeated original-query sweep or road simulation",
            "tool_sha256": sha(Path(__file__).read_bytes()), "xbe_sha256": sha(xbe),
            "garage_source_sha256": sha(garage), "prior_solver_log": str(prior.relative_to(root)).replace("\\", "/"),
            "prior_solver_log_sha256": sha(prior_data), "prior_solver_log_reexecuted": False,
            "summary": {"tunes": len(tunes), "main_resource_variants": len(bounds),
                        "families": len({b["family"] for b in bounds}),
                        "types": dict(Counter(str(b["type"]) for b in bounds)),
                        "authored_clearances": dict(Counter(format(t["clearance"], ".2f") for t in tunes if t["clearance_authored"])),
                        "default_clearances": sum(not t["clearance_authored"] for t in tunes),
                        "nonzero_stock_offsets": sum(any(t["stock_offsets"]) for t in tunes),
                        "explicit_airride_blocks": sum(t["airride_explicit"] for t in tunes),
                        "minimum_four_wheel_skin_cap_margin": min(b["skin_cap_margin"] for b in bounds if b["type"] == 4),
                        "minimum_y_range": [min(b["minimum_y"] for b in bounds), max(b["minimum_y"] for b in bounds)],
                        "allowed_radius_min_max": [list(min(radii)), list(max(radii))]},
            "stock_ride_steps_6_7_8": list(xbe_floats(xbe, 0x471A90+6*4, 3)), "tunes": tunes, "bounds": bounds}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--prior-log", type=Path)
    parser.add_argument("--report", type=Path, required=True, help="New output JSON; existing files are refused")
    args = parser.parse_args()
    root = args.root.resolve()
    report = audit(root, (args.assets or root/"templates/game/assets").resolve(),
                   (args.prior_log or root/".local/stock-ride-catalogue-20261024.log").resolve())
    with args.report.open("x", encoding="utf8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(report["summary"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
