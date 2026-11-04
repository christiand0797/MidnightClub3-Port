#!/usr/bin/env python3
"""Create an exclusive, digest-pinned VEHICLE material proposal from explicit inputs.

This CPU tool never generates maps, scans generator outputs, installs a pack, changes
settings, or approves its own manifest. The game still applies the exact car-program
bridge and material gates. An output is a proposal, not native rendering acceptance.

Required gate receipt (independently reviewed; its SHA is a separate CLI argument)::

  {"schema":"mc3-offline-pbr-gate-v1", "offline_pbr_adoption_allowed":true,
   "strict_movies":{"completed":106,"total":106,"failed":0,
                    "final_audit_sha256":"<64 hex>"},
   "full_project_backup":{"all_files_verified":true,
                          "manifest_sha256":"<64 hex>",
                          "verification_sha256":"<64 hex>"}}

This declaration does not perform or replace those audits. The caller must independently
review their receipts before supplying the gate SHA; this tool cannot authenticate a human.

Explicit maps inventory (also SHA-pinned, paths relative to --maps-root)::

  {"schema":"mc3-vehicle-map-inputs-v1", "textures":{"0123456789ABCDEF":{
    "normal":{"file":"selected_n.png","sha256":"<64 hex>"},
    "ao":{"file":"selected_ao.png","sha256":"<64 hex>"},
    "roughness":{"file":"selected_r.png","sha256":"<64 hex>"},
    "metallic":{"file":"selected_m.png","sha256":"<64 hex>"}}}}

Metallic is optional (zero when absent). All maps must have the same dimensions;
there is no resizing or invented height/emission. Normal convention is explicit.
Capture vehicle-keys bridge is a stage mask: bit 0 = stage 0, bit 1 = stage 1.
Only these proven stage outputs at UV set 0 are eligible; targets are excluded.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import io
import json
from pathlib import Path
import re
import sys


MAX_FILE_BYTES = 128 << 20
MAX_FRAME_BYTES = 64 << 20
MAX_DIMENSION = 8192
MAX_MATERIALS = 256
MAX_DECODED_BYTES = 512 << 20
HEX64 = re.compile(r"[0-9a-fA-F]{64}\Z")
HEX16 = re.compile(r"[0-9a-fA-F]{16}\Z")
DRAW = re.compile(r"^\d{4,}\s+draw\s")
EVENT = re.compile(r"^\d{4,}\s")
KEYS = re.compile(r"^\s+vehicle-keys vs ([0-9A-Fa-f]{16}) ps ([0-9A-Fa-f]{16}) bridge ([0-9]+)\s*$")
STAGE = re.compile(r"\|\s+t([0-3])\s+[^|]*?\bhash ([0-9A-Fa-f]{16})\s+tci ([0-9]+)(?=\s|$)")
UNUSED_GAME_ALPHA = hashlib.sha256(b"").hexdigest()


class ProposalError(ValueError):
    pass


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ProposalError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def valid_digest(value) -> bool:
    return isinstance(value, str) and HEX64.fullmatch(value) is not None


def read_bounded(path: Path, limit: int) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, f"{path}: exceeds {limit} encoded bytes")
    return data


def read_pinned_json(path: Path, expected: str) -> tuple[dict, str]:
    require(valid_digest(expected), f"{path}: an independent SHA-256 is required")
    encoded = read_bounded(path, MAX_FILE_BYTES)
    actual = digest(encoded)
    require(actual == expected.lower(), f"{path}: SHA-256 mismatch")
    def unique_fields(pairs):
        obj = {}
        for key, value in pairs:
            require(key not in obj, f"{path}: duplicate JSON field {key}")
            obj[key] = value
        return obj

    try:
        value = json.loads(encoded, object_pairs_hook=unique_fields)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProposalError(f"{path}: invalid JSON") from error
    require(isinstance(value, dict), f"{path}: JSON object required")
    return value, actual


def validate_gate(path: Path, expected: str) -> str:
    gate, actual = read_pinned_json(path, expected)
    require(gate.get("schema") == "mc3-offline-pbr-gate-v1", "unrecognized offline PBR gate receipt")
    require(gate.get("offline_pbr_adoption_allowed") is True, "offline PBR adoption gate is not open")
    media, backup = gate.get("strict_movies"), gate.get("full_project_backup")
    require(isinstance(media, dict) and isinstance(backup, dict), "media and full-project backup receipts required")
    # bool is an int in Python; it is not a completed movie count.
    require(all(type(media.get(k)) is int for k in ("completed", "total", "failed")), "integer strict movie counts required")
    require((media["completed"], media["total"], media["failed"]) == (106, 106, 0),
            "all 106 strict movies must pass before original-derived PBR adoption")
    require(valid_digest(media.get("final_audit_sha256")), "final media audit digest required")
    require(backup.get("all_files_verified") is True and valid_digest(backup.get("manifest_sha256")) and
            valid_digest(backup.get("verification_sha256")), "all-file verified full-project backup proof required")
    return actual


@dataclass(frozen=True, order=True)
class Assignment:
    texture: str
    vertex: str
    pixel: str
    stage: int
    uv_set: int = 0


def capture_candidates(encoded: bytes, selected: set[str]) -> Counter:
    """Capture evidence is input to a proposal; only the renderer can prove a live draw."""
    require(len(encoded) <= MAX_FRAME_BYTES, "capture frame exceeds byte ceiling")
    try:
        lines = encoded.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise ProposalError("capture frame is not UTF-8") from error
    found = Counter()
    current = None
    consumed = False
    for line in lines:
        if EVENT.match(line):
            current = line if DRAW.match(line) else None
            consumed = False
            continue
        keys = KEYS.match(line)
        if not keys or current is None or consumed:
            continue
        consumed = True
        vertex, pixel, mask_text = keys.groups()
        mask = int(mask_text)
        if mask == 0 or mask & ~3 or int(vertex, 16) in (0, (1 << 64) - 1) or int(pixel, 16) in (0, (1 << 64) - 1):
            continue
        # Look at each exact stage segment: a target marker on another stage must
        # not reject a genuine vinyl stage beside reflection maps.
        segments = re.split(r"(?=\|\s+t[0-3]\s)", current)[1:]
        stage_counts = Counter(int(m.group(1)) for segment in segments if (m := STAGE.search(segment)))
        for segment in segments:
            stage_match = STAGE.search(segment)
            if not stage_match:
                continue
            stage_text, texture, tci_text = stage_match.groups()
            stage, texture = int(stage_text), texture.upper()
            if stage_counts[stage] != 1 or stage not in (0, 1) or not mask & (1 << stage) or int(tci_text) != 0 or texture not in selected:
                continue
            if re.search(r"\b(target|unbound|shadow|rect)\b", segment) or texture == "0" * 16:
                continue
            found[Assignment(texture, vertex.upper(), pixel.upper(), stage)] += 1
    return found


def contained_file(root: Path, relative: str) -> Path:
    require(isinstance(relative, str) and relative and "\\" not in relative,
            "map files must be explicit relative slash paths")
    p = Path(relative)
    require(not p.is_absolute() and ":" not in relative and all(part not in ("..", ".") for part in p.parts),
            "map path must stay inside maps root")
    resolved_root, resolved = root.resolve(strict=True), (root / p).resolve(strict=True)
    require(resolved.is_relative_to(resolved_root) and resolved.is_file(), "map path escapes maps root or is not a file")
    return resolved


def load_map(root: Path, record: dict, role: str, *, expected_size=None, max_mip_bytes=MAX_DECODED_BYTES):
    from PIL import Image
    require(isinstance(record, dict) and set(record) == {"file", "sha256"}, f"{role}: file and SHA-256 required")
    require(valid_digest(record["sha256"]), f"{role}: invalid map SHA-256")
    path = contained_file(root, record["file"])
    encoded = read_bounded(path, MAX_FILE_BYTES)
    actual = digest(encoded)
    require(actual == record["sha256"].lower(), f"{role}: input map SHA-256 mismatch")
    # Decode the same verified bytes, never a second read racing the generator.
    with Image.open(io.BytesIO(encoded)) as image:
        require(image.format == "PNG" and getattr(image, "n_frames", 1) == 1, f"{role}: single-frame PNG required")
        require(0 < image.width <= MAX_DIMENSION and 0 < image.height <= MAX_DIMENSION, f"{role}: invalid dimensions")
        require(expected_size is None or image.size == expected_size,
                f"{role}: map dimensions differ; no implicit resampling")
        require(mip_bytes(*image.size) <= max_mip_bytes, "proposal exceeds decoded map/mip budget")
        converted = image.convert("RGB" if role == "normal" else "L")
        converted.load()
    return converted, {"file": record["file"], "sha256": actual}


def mip_bytes(width: int, height: int, channels: int = 4) -> int:
    total = 0
    while True:
        total += width * height * channels
        if width == height == 1:
            return total
        width, height = max(1, width // 2), max(1, height // 2)


def encode_png(image) -> bytes:
    output = io.BytesIO()
    image.save(output, format="PNG", optimize=False)
    return output.getvalue()


def build(*, captures: list[Path], textures: list[str], maps_root: Path, maps_manifest: Path,
          maps_sha256: str, output: Path, review_sha256: str, gate_receipt: Path,
          gate_sha256: str, normal_convention: str) -> dict:
    require(valid_digest(review_sha256), "independent material review SHA-256 required")
    require(normal_convention in ("opengl", "directx"), "explicit normal convention required")
    require(textures and all(HEX16.fullmatch(h) and int(h, 16) != 0 for h in textures),
            "select one or more explicit 16-hex texture hashes")
    selected = {h.upper() for h in textures}
    gate_digest = validate_gate(gate_receipt, gate_sha256)
    inventory, inventory_digest = read_pinned_json(maps_manifest, maps_sha256)
    require(inventory.get("schema") == "mc3-vehicle-map-inputs-v1" and isinstance(inventory.get("textures"), dict),
            "unrecognized explicit vehicle map inventory")
    candidates, evidence = Counter(), []
    require(bool(captures), "explicit capture directories required")
    for capture in captures:
        encoded = read_bounded(capture / "frame.txt", MAX_FRAME_BYTES)
        candidates.update(capture_candidates(encoded, selected))
        evidence.append({"frame": str((capture / "frame.txt").resolve()), "sha256": digest(encoded)})
    require(candidates, "no selected texture has a supported exact vehicle bridge/stage/UV capture")
    require({a.texture for a in candidates} == selected, "every selected texture needs supported capture evidence")
    require(len(candidates) <= MAX_MATERIALS, "vehicle proposal exceeds material limit")
    images, source_maps, decoded_total = {}, {}, 0
    assignment_counts = Counter(a.texture for a in candidates)
    from PIL import Image
    for texture in sorted(selected):
        records = inventory["textures"].get(texture)
        require(isinstance(records, dict) and set(records) in ({"normal", "ao", "roughness"},
                {"normal", "ao", "roughness", "metallic"}), f"{texture}: explicit normal/AO/roughness maps required")
        # Establish dimensions and account both resulting maps for every material
        # BEFORE decoding pixels. The loader retains a separate mip chain per row.
        normal, normal_proof = load_map(maps_root, records["normal"], "normal",
            max_mip_bytes=(MAX_DECODED_BYTES - decoded_total) // (2 * assignment_counts[texture]))
        maps, proofs = {"normal": normal}, {"normal": normal_proof}
        size = maps["normal"].size
        decoded_total += 2 * mip_bytes(*size) * assignment_counts[texture]
        for role, record in records.items():
            if role != "normal":
                maps[role], proofs[role] = load_map(maps_root, record, role, expected_size=size)
        normal_rgb = maps["normal"].tobytes()
        # The loader also rejects zero/back-facing tangent normals. Preserve the
        # selected map's RGB bytes and convention; do not synthesize a direction.
        require(all(normal_rgb[i + 2] > 127 for i in range(0, len(normal_rgb), 3)),
                f"{texture}: zero/back-facing tangent normals are unsupported")
        metal = maps.get("metallic") or Image.new("L", size, 0)
        orm = Image.merge("RGBA", (maps["ao"], maps["roughness"], metal, Image.new("L", size, 255)))
        images[texture] = (encode_png(maps["normal"].convert("RGBA")), encode_png(orm), size)
        source_maps[texture] = proofs

    # Validate everything before the first write. Existing destinations, including
    # empty directories and symlinks, are never reused or removed.
    require(not output.exists() and not output.is_symlink(), "output must be a new exclusive directory")
    output.mkdir(exist_ok=False)
    names = {}
    for texture, (normal, orm, _) in images.items():
        names[texture] = {}
        for role, encoded in (("normal", normal), ("orm", orm)):
            name = f"{texture}_{role}.png"
            with (output / name).open("xb") as stream:
                stream.write(encoded)
            names[texture][role] = (name, digest(encoded))
    lines = ["# VEHICLE MATERIAL PROPOSAL: not approved or installed; native acceptance is separate.",
             "# Game albedo retains its own alpha. baseline_alpha_sha256 is unused for albedo=game.",
             "schema = 1", ""]
    for assignment in sorted(candidates):
        lines.extend(["[[materials]]", f"# {candidates[assignment]} exact captured draw(s)",
                      f'texture = "{assignment.texture}"', f'vertex_shader = "{assignment.vertex}"',
                      f'pixel_shader = "{assignment.pixel}"', f"stage = {assignment.stage}", "uv_set = 0",
                      'surface = "vehicle"', "cutout = false", f'normal_convention = "{normal_convention}"',
                      f'baseline_alpha_sha256 = "{UNUSED_GAME_ALPHA}"', f'review_sha256 = "{review_sha256.lower()}"',
                      'albedo = "game"'])
        for role, (name, sha) in names[assignment.texture].items():
            lines.append(f'{role} = {{ file = "{name}", sha256 = "{sha}", color_space = "linear" }}')
        lines.append("")
    manifest = "\n".join(lines).encode("utf-8")
    with (output / "materials-v1.toml").open("xb") as stream:
        stream.write(manifest)
    receipt = {"schema": "mc3-vehicle-material-proposal-v1", "status": "proposal", "installed": False,
               "native_rendering_accepted": False, "manifest_sha256": digest(manifest),
               "material_review_sha256": review_sha256.lower(), "gate_receipt_sha256": gate_digest,
               "maps_inventory_sha256": inventory_digest, "captures": evidence, "source_maps": source_maps,
               "assignments": len(candidates), "decoded_mip_bytes": decoded_total,
               "height_mode": "none", "emission": False}
    with (output / "proposal.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(receipt, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return receipt


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--capture", type=Path, action="append", required=True)
    parser.add_argument("--texture", action="append", required=True)
    parser.add_argument("--maps-root", type=Path, required=True)
    parser.add_argument("--maps-manifest", type=Path, required=True)
    parser.add_argument("--maps-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--review-sha256", required=True)
    parser.add_argument("--gate-receipt", type=Path, required=True)
    parser.add_argument("--gate-sha256", required=True)
    parser.add_argument("--normal-convention", choices=("opengl", "directx"), required=True)
    args = vars(parser.parse_args(argv))
    args["captures"], args["textures"] = args.pop("capture"), args.pop("texture")
    try:
        result = build(**args)
    except (ProposalError, OSError, ValueError) as error:
        print(f"vehicle material proposal rejected: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    print("Proposal only. Independently review the manifest and run Check-PbrMaterials before selecting it in F1.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
