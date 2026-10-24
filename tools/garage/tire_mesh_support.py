#!/usr/bin/env python3
"""Read the supplied MC3 tire pool and report bounded, per-draw-slot vertex support.

Never changes source assets or the game. The generated scenarios use an explicitly stated upright-wheel
convention, not a captured final game transform. support() also accepts any supplied finite 3x3 transform.
Run from the repository: python tools/garage/tire_mesh_support.py --output .local/tire-mesh-support.json
"""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/texture_pack"))
RIGHT_YAW = struct.unpack("<f", struct.pack("<I", 0x40490FDB))[0]


class FormatError(ValueError):
    pass


def checked(data: bytes, start: int, size: int, what: str) -> None:
    if start < 0 or size < 0 or start + size > len(data):
        raise FormatError(f"{what}: range {start:#x}+{size:#x} exceeds {len(data):#x} bytes")


def u32(data: bytes, offset: int) -> int:
    checked(data, offset, 4, "u32")
    return struct.unpack_from("<I", data, offset)[0]


@dataclass(frozen=True)
class Record:
    page: int
    offset: int
    name: str
    kind: str
    start: int
    size: int


@dataclass(frozen=True)
class Geometry:
    slot: int
    geometry_index: int
    vertex_offset: int
    index_offset: int
    stride: int
    positions: tuple[tuple[float, float, float], ...]
    indices: tuple[int, ...]

    @property
    def used_indices(self) -> tuple[int, ...]:
        return tuple(sorted(set(self.indices)))

    @property
    def used_positions(self) -> tuple[tuple[float, float, float], ...]:
        return tuple(self.positions[i] for i in self.used_indices)


def validate_pool(data: bytes, map_text: str) -> tuple[Record, ...]:
    """Validate the page/map envelope before calling the existing pool parser."""
    checked(data, 0, 12, "pool header")
    magic, pages, slot_size = struct.unpack_from("<4sII", data)
    if magic != b"pf05" or not 0 < pages <= 65536 or not slot_size:
        raise FormatError("unsupported pool header/page count/slot size")
    checked(data, 12, pages * 4, "page table")
    spans = []
    for i in range(pages):
        value = u32(data, 12 + i * 4)
        start, size = (value & 0xFFFFF) * 2048, (value >> 20) * 2048
        if start < 12 + 4 * pages or not 0 < size <= slot_size:
            raise FormatError(f"page {i}: invalid start/extent")
        checked(data, start, size, f"page {i}")
        spans.append((start, size))
    for (start, size), (following, _) in zip(sorted(spans), sorted(spans)[1:]):
        if start + size > following:
            raise FormatError("overlapping stored pages")
    records = []
    try:
        rows = csv.reader(io.StringIO(map_text))
        for row in rows:
            if not row or not any(x.strip() for x in row) or row[0].lstrip().startswith("#"):
                continue
            if len(row) != 5:
                raise FormatError("map record must contain five CSV fields")
            page, offset, size = int(row[0]), int(row[1]), int(row[4])
            name, kind = row[2], row[3]
            if not 0 <= page < pages or offset < 0 or size < 0x20 or not name or not kind or "\0" in name:
                raise FormatError("invalid map page/offset/size/name/type")
            start, extent = spans[page]
            if offset + size > extent:
                raise FormatError(f"{name}: record exceeds its stored page")
            r = start + offset
            if u32(data, r + 12) != size or u32(data, r + 4) or u32(data, r + 8):
                raise FormatError(f"{name}: map/header size or reserved-field mismatch")
            baked = u32(data, r)
            # Texture records use zero here; the model's internal pointers require a real baked base.
            if not (kind.startswith("mip") and baked == 0) and (
                    not 0x80000000 <= baked < 0x90000000 or baked + size - 0x20 > 0x100000000):
                raise FormatError(f"{name}: invalid baked payload address")
            records.append(Record(page, offset, name, kind, r, size))
    except (ValueError, csv.Error) as error:
        if isinstance(error, FormatError):
            raise
        raise FormatError(f"invalid resource map: {error}") from error
    if not records:
        raise FormatError("empty resource map")
    ordered = sorted(records, key=lambda r: r.start)
    for a, b in zip(ordered, ordered[1:]):
        if a.start + a.size > b.start:
            raise FormatError("overlapping/duplicate mapped records")
    return tuple(records)


def load_pool(path: Path) -> tuple[bytes, tuple[Record, ...], bytes]:
    data = path.read_bytes()
    map_data = path.with_suffix(".xbpmap").read_bytes()
    try:
        records = validate_pool(data, map_data.decode("utf-8-sig"))
    except UnicodeError as error:
        raise FormatError("resource map is not UTF-8") from error
    # Reuse the project's established page/address parser after strict envelope validation. It imports the
    # existing texture tools' NumPy dependency; decoding and support below otherwise use the standard library.
    from xbpf import Pool
    pool = Pool(path)
    expected = [(r.name, r.kind, r.start, r.size) for r in records]
    if pool.b != data or list(pool.records()) != expected:
        raise FormatError("source changed while reading or existing parser/map disagreement")
    for r in records:
        if pool.to_file(r.start, u32(data, r.start)) != r.start + 0x20:
            raise FormatError("baked pointer normalization disagreement")
    return data, records, map_data


def decode_record(data: bytes, record: Record) -> tuple[Geometry, ...]:
    if record.kind != "drwShaderModel":
        raise FormatError("requested record is not drwShaderModel")
    checked(data, record.start, record.size, "model record")
    b = data[record.start:record.start + record.size]
    checked(b, 0x20, 0x50, "shader-model header")
    baked = u32(b, 0)

    def pointer(address: int, size: int, what: str) -> int:
        # Baked base denotes the record payload at +0x20, not the record header.
        offset = address - baked + 0x20
        if offset < 0x20:
            raise FormatError(f"{what}: pointer before this record's payload")
        checked(b, offset, size, what)
        return offset

    result = []
    for slot in range(4):
        address = u32(b, 0x30 + 4 * slot)
        if not address:
            continue
        shape = pointer(address, 12, "draw-slot header")
        count = u32(b, shape + 4) >> 16
        if not 0 < count <= 4096:
            raise FormatError("invalid draw-slot geometry count")
        table = pointer(u32(b, shape + 8), count * 4, "geometry pointer table")
        for index in range(count):
            geom = pointer(u32(b, table + 4 * index), 0x20, "geometry")
            stride = u32(b, geom + 4) >> 24
            if stride != 28:
                raise FormatError(f"unsupported vertex stride {stride}; no heuristic position scanning")
            vb = pointer(u32(b, geom + 0x18), 12, "vertex-buffer header")
            common, physical, lock = struct.unpack_from("<III", b, vb)
            if common != 1 or lock or not physical or physical & 0xF0000000:
                raise FormatError("unsupported vertex-buffer resource header")
            vertices = pointer(physical | 0x80000000, 12, "physical vertex data")
            descriptor = pointer(u32(b, geom + 0x10), 4, "index-buffer descriptor")
            count_word = pointer(u32(b, descriptor), 4, "index count word")
            ib = count_word - 4
            checked(b, ib, 12, "index-buffer header")
            common, encoded_count, lock = struct.unpack_from("<III", b, ib)
            if common != 1 or lock or encoded_count & 0xFFFF:
                raise FormatError("unsupported index-buffer resource header")
            index_count = encoded_count >> 16
            if not index_count:
                raise FormatError("empty index buffer")
            indices_at = pointer(u32(b, geom + 0x1C), index_count * 2, "index data")
            if indices_at < ib + 12 or ib <= vertices or (ib - vertices) % stride:
                raise FormatError("unsupported/noncontiguous vertex-to-index-buffer layout")
            vertex_count = (ib - vertices) // stride
            indices = struct.unpack_from(f"<{index_count}H", b, indices_at)
            if max(indices) >= vertex_count:
                raise FormatError("index outside decoded vertex stream")
            positions = tuple(struct.unpack_from("<3f", b, vertices + stride * i) for i in range(vertex_count))
            if not all(math.isfinite(x) for p in positions for x in p):
                raise FormatError("nonfinite vertex position")
            result.append(Geometry(slot, index, vertices, indices_at, stride, positions, indices))
    if not result:
        raise FormatError("model has no supported draw slots")
    return tuple(result)


def matmul(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def rotation(axis: str, angle: float):
    c, s = math.cos(angle), math.sin(angle)
    if axis == "x":
        return ((1.,0.,0.),(0.,c,-s),(0.,s,c))
    if axis == "y":
        return ((c,0.,s),(0.,1.,0.),(-s,0.,c))
    if axis == "z":
        return ((c,-s,0.),(s,c,0.),(0.,0.,1.))
    raise ValueError("unknown rotation axis")


def wheel_matrix(width: float, diameter: float, camber: float, side: str, degree_factor: float,
                 roll: float = 0., steer: float = 0., spin: float = 0.):
    """Explicit illustrative convention: bodyRz * steerRy * signedCamberRz * spinRx * sideRy * scale."""
    if side not in ("left", "right") or not all(math.isfinite(x) for x in
            (width, diameter, camber, degree_factor, roll, steer, spin)) or width <= 0 or diameter <= 0:
        raise ValueError("invalid wheel dimensions/angles/side")
    scale = ((2 * width,0.,0.),(0.,diameter,0.),(0.,0.,diameter))
    matrices = [rotation("z", math.radians(roll)), rotation("y", math.radians(steer)),
                rotation("z", camber * degree_factor * (1 if side == "left" else -1)),
                rotation("x", math.radians(spin)), rotation("y", RIGHT_YAW if side == "right" else 0.), scale]
    out = matrices[0]
    for matrix in matrices[1:]:
        out = matmul(out, matrix)
    return out


def support(positions, matrix, normal=(0.,1.,0.), translation=(0.,0.,0.), plane_distance=0.):
    """Exact minimum over the supplied referenced vertices for an explicitly supplied affine transform/plane."""
    if len(matrix) != 3 or any(len(row) != 3 for row in matrix) or len(normal) != 3 or len(translation) != 3:
        raise ValueError("expected a 3x3 matrix and three-component vectors")
    scalars = [*normal, *translation, plane_distance, *(x for row in matrix for x in row)]
    if not all(math.isfinite(x) for x in scalars):
        raise ValueError("nonfinite transform/plane")
    magnitude = max(abs(x) for x in normal)
    if not magnitude:
        raise ValueError("zero plane normal")
    # Rescale first: hypot can overflow for three finite components near DBL_MAX. This also keeps
    # subnormal normals usable without treating their length as zero.
    scaled = tuple(x / magnitude for x in normal)
    length = math.hypot(*scaled)
    n = tuple(x / length for x in scaled)
    # plane_distance is a distance along the normalized normal, in metres.
    best = None
    for i, p in enumerate(positions):
        if len(p) != 3 or not all(math.isfinite(x) for x in p):
            raise ValueError("invalid position")
        transformed = tuple(sum(matrix[row][k] * p[k] for k in range(3)) for row in range(3))
        projection = sum(n[k] * transformed[k] for k in range(3))
        if not math.isfinite(projection):
            raise ValueError("transformed position overflow")
        if best is None or projection < best[0]:
            best = (projection, i, transformed)
    if best is None:
        raise ValueError("empty position set")
    hub_distance = sum(n[k] * translation[k] for k in range(3)) - plane_distance
    if not math.isfinite(hub_distance) or not math.isfinite(hub_distance + best[0]):
        raise ValueError("plane/translation overflow")
    return {"support_from_hub_m": -best[0], "clearance_m": hub_distance + best[0],
            "witness_used_vertex": best[1], "witness_relative_position_m": best[2], "unit_normal": n}


def xbe_constants(path: Path):
    b = path.read_bytes()
    if b[:4] != b"XBEH":
        raise FormatError("not an XBE")
    base, count, sections = u32(b, 0x104), u32(b, 0x11C), u32(b, 0x120)
    checked(b, sections - base, count * 56, "XBE section table")
    def f32(va):
        for i in range(count):
            v, _, offset, raw_size = struct.unpack_from("<IIII", b, sections - base + i * 56 + 4)
            if v <= va and va + 4 <= v + raw_size:
                checked(b, offset + va - v, 4, "XBE constant")
                return struct.unpack_from("<f", b, offset + va - v)[0]
        raise FormatError(f"XBE constant {va:#x} is not in a raw section")
    width_factor, angle_factor = f32(0x449004), f32(0x44B090)
    if width_factor != 2. or not 0.0174 < angle_factor < 0.0175:
        raise FormatError("unsupported source constants for verified F7170 normalization")
    return {"sha256": hashlib.sha256(b).hexdigest(), "width_factor_at_449004": width_factor,
            "degree_factor_at_44B090": angle_factor}


def create_report(pool_path, xbe_path, widths, diameters, cambers, sides, normal, roll=0., steer=0., spin=0.):
    data, records, map_data = load_pool(pool_path)
    constants = xbe_constants(xbe_path)
    models = []
    for record in records:
        if record.kind != "drwShaderModel":
            continue
        textures = [r.name for r in records if r.page == record.page and r.kind == "mip0"]
        model_bytes = data[record.start:record.start + record.size]
        embedded = [name for name in textures if name.encode("ascii") + b"\0" in model_bytes]
        ordinary = bool(textures and textures[0].startswith("whl_tir_"))
        row = {"page": record.page, "name": record.name, "record_file_offset": record.start,
               "record_bytes": record.size, "record_sha256": hashlib.sha256(model_bytes).hexdigest(),
               "page_texture_records": textures, "embedded_matching_texture_names": embedded,
               "ordinary_car_scale_scenario": ordinary, "draw_slots": []}
        for g in decode_record(data, record):
            used = g.used_positions
            geometry = {"slot": g.slot, "geometry": g.geometry_index, "vertex_stride": g.stride,
                        "vertex_count": len(g.positions), "referenced_vertices": len(used),
                        "index_count": len(g.indices), "vertex_record_offset": g.vertex_offset,
                        "index_record_offset": g.index_offset,
                        "position_sha256": hashlib.sha256(b"".join(struct.pack("<3f", *p) for p in used)).hexdigest(),
                        "bounds": [[min(p[k] for p in used), max(p[k] for p in used)] for k in range(3)],
                        "maximum_radial_yz": max(math.hypot(p[1],p[2]) for p in used), "samples": []}
            if ordinary:
                for w in widths:
                    for d in diameters:
                        for c in cambers:
                            for side in sides:
                                matrix = wheel_matrix(w,d,c,side,constants["degree_factor_at_44B090"],roll,steer,spin)
                                result = support(used,matrix,normal)
                                result.update(width_m=w, diameter_m=d, camber_degrees=c, side=side,
                                              witness_stream_vertex=g.used_indices[result["witness_used_vertex"]],
                                              scalar_radius_m=d/2,
                                              excess_beyond_scalar_radius_m=result["support_from_hub_m"]-d/2)
                                geometry["samples"].append(result)
            row["draw_slots"].append(geometry)
        models.append(row)
    return {"format": "mc3-tire-support-v1", "pool": str(pool_path.resolve()),
            "pool_sha256": hashlib.sha256(data).hexdigest(), "map_sha256": hashlib.sha256(map_data).hexdigest(),
            "source_xbe": constants, "models": models,
            "assumptions": {"matrix_order_column_vectors": "bodyRz * steerRy * signedCamberRz * spinRx * sideRy * diag(2w,D,D)",
                            "body_roll_degrees": roll, "steer_degrees": steer, "spin_degrees": spin,
                            "hub_translation_m": [0,0,0], "plane_distance_m": 0, "normal": normal,
                            "camber_uses_original_float32_degree_factor": True,
                            "right_yaw_radians_float32": RIGHT_YAW,
                            "draw_slots_reported_separately_no_concurrent_or_lod_selection_assumed": True,
                            "car_scenario_classification": "first page mip0 record starts with whl_tir_; no live renderer branch captured",
                            "support_is_over_referenced_vertex_cloud_no_primitive_assembly_or_visibility_test": True,
                            "arithmetic": "float32 stored positions/constants promoted to Python binary64; not a bit-exact guest transform",
                            "special_bike_meshes_metadata_only_no_ordinary_car_scaling_claim": True,
                            "final_live_wheel_matrix_and_road_normal_not_captured": True}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pool", type=Path, default=ROOT/"templates/game/assets/resources/vehicle/tire.xbpf")
    parser.add_argument("--xbe", type=Path, default=ROOT/"templates/game/assets/default.xbe")
    parser.add_argument("--width", type=float, nargs="+", default=[.195,.355,.755], help="metres")
    parser.add_argument("--diameter", type=float, nargs="+", default=[.65], help="full diameter, metres")
    parser.add_argument("--camber", type=float, nargs="+", default=[-15.,0.,15.])
    parser.add_argument("--side", choices=["left","right"], nargs="+", default=["left","right"])
    parser.add_argument("--normal", type=float, nargs=3, default=[0.,1.,0.])
    parser.add_argument("--roll", type=float, default=0.)
    parser.add_argument("--steer", type=float, default=0.)
    parser.add_argument("--spin", type=float, default=0.)
    parser.add_argument("--output", type=Path, required=True, help="new JSON report; existing files are refused")
    args = parser.parse_args()
    try:
        if args.output.exists() or args.output.suffix.lower() != ".json":
            raise ValueError("output must be a new .json file")
        if not args.width or not args.diameter or not args.camber or not args.side:
            raise ValueError("empty scenario list")
        report = create_report(args.pool,args.xbe,args.width,args.diameter,args.camber,args.side,args.normal,
                               args.roll,args.steer,args.spin)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report,stream,indent=2,allow_nan=False)
            stream.write("\n")
        geometries = sum(len(m["draw_slots"]) for m in report["models"])
        print(f"Read-only tire support report: {len(report['models'])} models, {geometries} geometries -> {args.output}")
        return 0
    except (OSError, ValueError, ImportError) as error:
        print(f"tire support analysis failed: {error}",file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
