#!/usr/bin/env python3
"""Exercise production tire-resource guards/scopes using all original ordinary tire records.

Original pool records are read only. A temporary binary fixture carries their bytes and explicit resource
pointer relocations into the existing six-MiB synthetic guest. No original game bytes are added to source.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import tire_mesh_support as tire
import verify_tire_profile_fit as bead


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_fixture(pool: Path, xbe: Path, garage: Path) -> tuple[bytes, dict]:
    # Reuse catalogue and source validation from the independent vertex-math fixture, while retaining whole
    # records here so the real production TireVertexData guard sees both original shape/header/index layouts.
    _, catalogue = bead.input_fixture(pool, xbe, garage)
    stock, target = catalogue["stock_ratios"], catalogue["requested_ratios"]
    data, records, map_bytes = tire.load_pool(pool)
    models, metadata = [], []
    ordinary_pages = sorted({g["page"] for g in catalogue["geometry"]})
    if len(ordinary_pages) != 88:
        raise ValueError("expected 88 ordinary pages")
    for record in records:
        if record.kind != "drwShaderModel" or record.page not in ordinary_pages:
            continue
        raw = data[record.start:record.start+record.size]
        baked = tire.u32(raw, 0)
        geometries = tire.decode_record(data, record)
        if len(geometries) != 2 or [g.slot for g in geometries] != [0, 1] or record.size != 8752:
            raise ValueError("unsupported original ordinary geometry layout")
        profile = min(range(8), key=lambda i: abs(stock[i]*.5-min((p[1]**2+p[2]**2)**.5
                                                               for p in geometries[0].positions)))
        # Every source profile appears once per brand; derive brand from the measured ordinary page groups,
        # then retain each page/model identity and source hash in the proof instead of hiding that assumption.
        ordinal = ordinary_pages.index(record.page)
        brand, within = divmod(ordinal, 8)
        if within != profile or brand >= 11:
            raise ValueError("ordinary page/profile grouping differs from the verified catalogue")
        relocations = {}

        def pointer(field: int, extent: int) -> int:
            tire.checked(raw, field, 4, "pointer field")
            offset = tire.u32(raw, field)-baked+0x20
            if offset < 0x20:
                raise ValueError("resource pointer before payload")
            tire.checked(raw, offset, extent, "resource target")
            if field in relocations and relocations[field] != offset:
                raise ValueError("conflicting relocation")
            relocations[field] = offset
            return offset

        for slot in range(4):
            if not tire.u32(raw, 0x30+4*slot):
                continue
            shape = pointer(0x30+4*slot, 12)
            count = tire.u32(raw, shape+4) >> 16
            table = pointer(shape+8, count*4)
            for index in range(count):
                geometry = pointer(table+4*index, 0x20)
                descriptor = pointer(geometry+0x10, 4)
                pointer(descriptor, 4)
                vb = pointer(geometry+0x18, 12)
                indices = pointer(geometry+0x1C, 1)
                match, = [g for g in geometries if g.slot == slot and g.geometry_index == index]
                # VB Data is a physical address, not a baked virtual pointer. Its rebased physical value
                # equals the fixture VA under the fixture's explicit identity VirtualFromPhysical adapter.
                physical = tire.u32(raw, vb+4)
                if physical & 0xF0000000 or physical != ((baked+match.vertex_offset-0x20) & 0x0FFFFFFF):
                    raise ValueError("unsupported original physical vertex address")
                relocations[vb+4] = match.vertex_offset
                if indices != match.index_offset:
                    raise ValueError("index descriptor/geometry disagreement")
        relocations = sorted(relocations.items())
        vertex_offsets = [g.vertex_offset for g in geometries]
        binary = struct.pack("<IIII", brand, profile, record.size, len(relocations))
        binary += b"".join(struct.pack("<II", *entry) for entry in relocations)
        binary += struct.pack("<2I", *vertex_offsets)+raw
        models.append(binary)
        minimum_x = min(p[0] for g in geometries for p in g.positions)
        maximum_x = max(p[0] for g in geometries for p in g.positions)
        metadata.append({"page": record.page, "model": record.name, "brand": brand,
                         "stock_profile": profile, "record_sha256": hashlib.sha256(raw).hexdigest(),
                         "record_bytes": record.size, "relocations": len(relocations),
                         "vertex_offsets": vertex_offsets, "minimum_x": minimum_x, "maximum_x": maximum_x})
    if len(models) != 88:
        raise ValueError("missing original ordinary model")
    binary = b"MC3TSCP1"+struct.pack("<8f34fI", *stock, *target, len(models))+b"".join(models)
    report = {"format": "mc3-original-tire-draw-scope-v1", "pool_sha256": hashlib.sha256(data).hexdigest(),
              "map_sha256": hashlib.sha256(map_bytes).hexdigest(), "xbe_sha256": sha(xbe),
              "garage_source_sha256": sha(garage), "fixture_sha256": hashlib.sha256(binary).hexdigest(),
              "minimum_x": min(x["minimum_x"] for x in metadata), "maximum_x": max(x["maximum_x"] for x in metadata),
              "stock_ratios": stock, "requested_ratios": target, "models": metadata}
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
    with tempfile.TemporaryDirectory(prefix="mc3-tire-scope-fixture-") as directory:
        path = Path(directory)/"original-records.bin"
        path.write_bytes(binary)
        completed = subprocess.run([str(args.executable.resolve()), "--original", str(path)],
                                   capture_output=True, text=True, timeout=60)
    report.update(checked_at=datetime.now(timezone.utc).isoformat(), executable_sha256=sha(args.executable),
                  tire_fit_source_sha256=sha(root/"templates/game/src/game/tire_fit.cpp"),
                  scope_fixture_source_sha256=sha(root/"tests/tire_draw_scope_fixture.cpp"),
                  stdout=completed.stdout, stderr=completed.stderr, exit_code=completed.returncode)
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
    print(f"Immutable source scope proof -> {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
