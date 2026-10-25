#!/usr/bin/env python3
"""Prepare layout-proven original garage composites and reversibly split staged inference outputs.

CPU only: this script never launches a GPU worker or modifies the active texture pack. The durable coordinator
must execute the recorded command later, after games and prior GPU workers have drained.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct

import numpy as np
from PIL import Image

import loading_art_audit as audit
import loading_art_worker as candidates
import upscale
import xbtex

ROOT = Path(__file__).resolve().parents[2]


def panel_layout(data, textures):
    """Resolve photo bitmap fills to quads, retaining the actual stored geometry and UV transforms."""
    if len(textures) != 2 or any(t.fmt != 0x0F or (t.width, t.height) != (256, 256) for t in textures):
        raise ValueError("expected exactly two original 256-square DXT5 photo textures")
    base, = struct.unpack_from("<I", data)
    words = np.frombuffer(data, dtype="<u4", offset=0x80, count=(len(data)-0x80)//4)

    def offset(pointer, extent):
        value = pointer-base+0x80
        if value < 0x80 or value+extent > len(data):
            raise ValueError("Flash pointer outside memory image")
        return value

    fills = []
    for texture in textures:
        bitmap = texture.header-0x20
        if bitmap < 0x80 or struct.unpack_from("<HH", data, bitmap+0xC) != (256, 256):
            raise ValueError("unexpected photo bitmap dimensions")
        if struct.unpack_from("<2f", data, bitmap+0x10) != (1/256, 1/256):
            raise ValueError("unexpected bitmap UV inverse-size fields")
        pointer = base+bitmap-0x80
        matches = []
        for reference in np.nonzero(words == pointer)[0]:
            at = 0x80+4*int(reference)-32
            if at < 0x80 or at+40 > len(data):
                continue
            kind, color = struct.unpack_from("<II", data, at)
            matrix = struct.unpack_from("<6f", data, at+8)
            if (kind == 0x6B617440 and color == 0xFFFFFFFF and all(math.isfinite(x) for x in matrix)
                    and .0001 < matrix[0] < .1 and .0001 < matrix[3] < .1
                    and abs(matrix[1])+abs(matrix[2]) < 1.e-8):
                matches.append({"fill_offset": at, "bitmap_offset": bitmap, "texture_header": texture.header,
                                "texture_hash": f"{texture.name:016X}.png", "matrix": list(matrix)})
        if len(matches) != 1:
            raise ValueError("photo fill binding is absent or ambiguous")
        fills.extend(matches)
    fills.sort(key=lambda item: item["fill_offset"])
    # Tokyo has thirteen fill styles: its photos are entries 5 and 13, rather than an adjacent pair.
    # Resolve the owning fill array through its original pointers instead of assuming an array of two.
    fill_start = fills[0]["fill_offset"]
    while fill_start >= 0x80+40 and struct.unpack_from("<I", data, fill_start-40)[0] == 0x6B617440:
        fill_start -= 40
    references = []
    for reference in np.nonzero(words == base+fill_start-0x80)[0]:
        at = 0x80+4*int(reference)
        if at+16 > len(data):
            continue
        end, _commands, count = struct.unpack_from("<3I", data, at+4)
        if not 2 <= count <= 64 or end != base+fill_start-0x80+40*count:
            continue
        if any((item["fill_offset"]-fill_start) % 40 or
               not 0 <= (item["fill_offset"]-fill_start)//40 < count for item in fills):
            continue
        try:
            commands = offset(struct.unpack_from("<I", data, at+8)[0], 8)
        except ValueError:
            continue
        if commands >= fill_start:
            continue
        references.append((at-8, commands, count))
    if len(references) != 1:
        raise ValueError("photo shape/fill-array owner is absent or ambiguous")
    shape, cursor, fill_count = references[0]
    photo_fills = {1+(item["fill_offset"]-fill_start)//40: item for item in fills}
    polygons = []
    while cursor+2 <= fill_start:
        opcode, = struct.unpack_from("<H", data, cursor)
        if opcode == 0:
            break
        if opcode == 5:
            if cursor+4 > fill_start or struct.unpack_from("<H", data, cursor+2)[0] != 0:
                raise ValueError("unexpected photo path delimiter")
            cursor += 4
            continue
        if opcode != 4 or cursor+24 > fill_start:
            raise ValueError("unrecognized photo polygon command")
        _command, fill, primitive, count = struct.unpack_from("<4H", data, cursor)
        if not 1 <= fill <= fill_count or primitive != 2 or count != 4:
            raise ValueError("unexpected photo polygon binding/vertex count")
        raw = struct.unpack_from("<8h", data, cursor+8)
        points = [list(raw[i:i+2]) for i in range(0, 8, 2)]
        if fill not in photo_fills:
            cursor += 24
            continue
        polygon = {**photo_fills[fill], "fill_index": fill, "quad_command_offset": cursor, "quad_twips": points}
        polygon["bounds_twips"] = [min(p[0] for p in points), min(p[1] for p in points),
                                   max(p[0] for p in points), max(p[1] for p in points)]
        polygons.append(polygon)
        cursor += 24
    if len(polygons) != 2:
        raise ValueError("expected two photo quads")
    left, right = sorted(polygons, key=lambda item: item["bounds_twips"][0])
    geometry_overlap = left["bounds_twips"][2]-right["bounds_twips"][0]
    if (not 0 <= geometry_overlap <= 100
            or left["bounds_twips"][1::2] != right["bounds_twips"][1::2]):
        raise ValueError("photo quads do not have the measured adjacent/overlapping vertical bounds")
    ml, mr = left["matrix"], right["matrix"]
    stored_pitch = (ml[4]-mr[4])*256
    if not 245 < stored_pitch < 255:
        raise ValueError("stored photo UV origins are not the measured approximately 250-pixel pitch")
    return {"shape_offset": shape, "fill_array_offset": fill_start, "fill_count": fill_count,
            "left": left, "right": right, "geometry_overlap_twips": geometry_overlap,
            "stored_uv_origin_delta_times_texture_width": stored_pitch,
            "uv_linear_parts_identical": all(ml[i] == mr[i] for i in (0, 1, 2, 3, 5)),
            "limits": "Original Flash bindings prove panel order. Quads may overlap and Tokyo has slightly different scales; source pixel registration proves the six-column image overlap. Native sampling still requires A/B validation."}


def overlap_errors(left, right):
    left, right = np.asarray(left), np.asarray(right)
    results = []
    for overlap in range(1, 17):
        a, b = left[:, -overlap:], right[:, :overlap]
        mask = (a[..., 3] > 250) & (b[..., 3] > 250)
        if not np.any(mask):
            raise ValueError("no opaque photo pixels to register")
        difference = np.abs(a[..., :3].astype(np.float64)-b[..., :3].astype(np.float64))[mask]
        results.append({"overlap": overlap, "mean_rgb_error": float(difference.mean())})
    return sorted(results, key=lambda value: value["mean_rgb_error"])


def join_rgb(left, right, overlap):
    left, right = np.asarray(left), np.asarray(right)
    if left.shape != right.shape or left.ndim != 3 or left.shape[2] != 4 or not 0 < overlap < left.shape[1]:
        raise ValueError("incompatible photo panels/overlap")
    width = left.shape[1]
    joined = np.empty((left.shape[0], width*2-overlap, 3), np.uint8)
    joined[:, :width-overlap] = left[:, :width-overlap, :3]
    # The six shared original columns differ slightly because the two source panels were compressed
    # independently. Feather only that already-overlapping source area, before any neural inference.
    weight = np.linspace(0, 1, overlap)[None, :, None]
    shared = left[:, -overlap:, :3]*(1-weight)+right[:, :overlap, :3]*weight
    joined[:, width-overlap:width] = np.rint(shared).astype(np.uint8)
    joined[:, width:] = right[:, overlap:, :3]
    return joined


def split_panels(joined, left_alpha, right_alpha, scale, overlap):
    joined = joined.convert("RGB")
    width, height = left_alpha.size
    if right_alpha.size != (width, height) or joined.size != ((2*width-overlap)*scale, height*scale):
        raise ValueError("joined output dimensions disagree with original panels")
    left = joined.crop((0, 0, width*scale, height*scale)).convert("RGBA")
    right = joined.crop(((width-overlap)*scale, 0, (2*width-overlap)*scale, height*scale)).convert("RGBA")
    left.putalpha(left_alpha.resize((width*scale, height*scale), Image.Resampling.LANCZOS))
    right.putalpha(right_alpha.resize((width*scale, height*scale), Image.Resampling.LANCZOS))
    return left, right


def prepare(assets, textures, staging, model):
    staging = candidates.safe_staging(staging)
    if staging.exists():
        raise ValueError("joined staging directory already exists")
    staging.mkdir(parents=True)
    for name in ("joined-source", "network-input", "network-output", "cpu-source-fidelity"):
        (staging/name).mkdir()
    jobs = {}
    for city in "adst":
        path = Path(assets)/"flash"/f"loadscreen_garage_{city}.xbck"
        raw = path.read_bytes()
        photo = [t for t in xbtex.xbck_textures(raw) if t.fmt == 0x0F and (t.width, t.height) == (256, 256)]
        layout = panel_layout(raw, photo)
        panels, images = {}, []
        for side in ("left", "right"):
            name = layout[side]["texture_hash"]
            source = Path(textures)/"source"/name
            image = audit.pixels(source)
            original = next(texture for texture in photo if texture.header == layout[side]["texture_header"])
            decoded = audit.extract_all.decode(original, raw[original.offset:original.offset+original.size], None)
            if decoded is None or not np.array_equal(image, decoded):
                raise ValueError("photo source PNG does not match its original Flash pixels")
            images.append(image)
            panels[side] = {"name": name, "source": str(source.resolve()), "source_sha256": upscale.file_digest(source),
                            "source_alpha_sha256": audit.digest(image[..., 3].tobytes()),
                            "base_replacement_sha256": upscale.file_digest(Path(textures)/"replace"/name)}
        errors = overlap_errors(*images)
        if errors[0]["overlap"] != 6 or errors[0]["mean_rgb_error"] >= 4:
            raise ValueError("original pixel registration does not confirm the stored overlapping layout")
        joined = join_rgb(*images, 6)
        name = f"garage_{city}.png"
        Image.fromarray(joined).save(staging/"joined-source"/name)
        pad = 32
        Image.fromarray(upscale.edge_pad(joined, pad, False, False)).save(staging/"network-input"/name)
        fallback = Image.fromarray(joined).resize((joined.shape[1]*4, joined.shape[0]*4), Image.Resampling.LANCZOS)
        left, right = split_panels(fallback, Image.fromarray(images[0][..., 3]), Image.fromarray(images[1][..., 3]), 4, 6)
        for side, image in (("left", left), ("right", right)):
            upscale.publish_png(image, staging/"cpu-source-fidelity"/panels[side]["name"])
            panels[side]["cpu_fallback_sha256"] = upscale.file_digest(staging/"cpu-source-fidelity"/panels[side]["name"])
        jobs[city] = {"bundle": str(path.resolve()), "bundle_sha256": upscale.file_digest(path), "layout": layout,
                     "panels": panels, "pixel_registration": errors, "overlap_pixels": 6, "integer_pitch_pixels": 250,
                     "joined_size": [506, 256], "scale": 4, "pad": pad,
                     "joined_source_rgb_sha256": audit.digest(joined.tobytes()),
                     "network_input_sha256": upscale.file_digest(staging/"network-input"/name)}
    command = [str(upscale.ESRGAN), "-i", str(staging/"network-input"), "-o", str(staging/"network-output"),
               "-n", model, "-s", "4", "-m", str(upscale.MODELS[model]), "-f", "png", "-j", "1:2:2"]
    result = {"format": "mc3-garage-photo-joined-plan-v1", "worker_sha256": upscale.file_digest(Path(__file__)),
              "planned_ncnn_context": upscale.pass_context(model), "jobs": jobs, "planned_ncnn_command": command,
              "gpu_started": False, "active_replacements_modified": False, "base_named_pass_records_modified": False}
    audit.write_new(staging/"joined-plan.json", result)
    return result


def split_network(staging, inference_record=None):
    staging = candidates.safe_staging(staging)
    plan = json.loads((staging/"joined-plan.json").read_text())
    if plan.get("format") != "mc3-garage-photo-joined-plan-v1" or plan["worker_sha256"] != upscale.file_digest(Path(__file__)):
        raise ValueError("joined worker/plan changed")
    actual = None
    if inference_record is not None:
        actual = json.loads(Path(inference_record).read_text())
        if (actual.get("format") != "mc3-garage-photo-inference-v1" or actual.get("phase") != "complete"
                or actual.get("plan_sha256") != upscale.file_digest(staging/"joined-plan.json")
                or set(actual.get("records", {})) != {f"garage_{city}.png" for city in plan["jobs"]}):
            raise ValueError("inference record does not cover the exact joined outputs")
    destination = staging/"neural-candidates"
    if destination.exists():
        raise ValueError("neural candidate directory already exists")
    prepared = []
    for city, job in plan["jobs"].items():
        for side in ("left", "right"):
            panel = job["panels"][side]
            if upscale.file_digest(Path(panel["source"])) != panel["source_sha256"]:
                raise ValueError("original photo panel changed")
        input_path = staging/"network-input"/f"garage_{city}.png"
        output_path = staging/"network-output"/input_path.name
        if upscale.file_digest(input_path) != job["network_input_sha256"]:
            raise ValueError("prepared original composite changed")
        if actual is not None:
            import cpu_x4plus
            record = actual["records"][input_path.name]
            if (record.get("source_sha256") != job["network_input_sha256"]
                    or record.get("output_sha256") != upscale.file_digest(output_path)
                    or record.get("device") != "cpu"
                    or any(not 1 <= record.get(key, 0) <= 2 for key in ("intra_op_threads", "inter_op_threads"))
                    or record.get("backend") != "pytorch-cpu-x4plus-v1"
                    or record.get("architecture") != cpu_x4plus.ARCHITECTURE
                    or record.get("weight_key_validation") != {"strict": True, "tensor_keys": 702, "dtype": "float32", "device": "cpu"}
                    or any(len(record.get(key, "")) != 64 for key in ("model_sha256", "module_sha256"))):
                raise ValueError("CPU inference provenance/digests do not match the prepared outputs")
            for prefix in ("model", "module"):
                path = Path(record.get(prefix+"_path", ""))
                if not path.is_file() or upscale.file_digest(path) != record[prefix+"_sha256"]:
                    raise ValueError("CPU inference model/module files disagree with recorded digests")
            if Path(record["module_path"]).resolve() != Path(cpu_x4plus.__file__).resolve():
                raise ValueError("CPU inference record does not identify the validated local backend")
        with Image.open(output_path) as image:
            expected = ((job["joined_size"][0]+2*job["pad"])*4, (job["joined_size"][1]+2*job["pad"])*4)
            if image.size != expected:
                raise ValueError("network output dimensions do not match padded original composite")
            p = job["pad"]*4
            joined = image.convert("RGB").crop((p, p, p+job["joined_size"][0]*4, p+job["joined_size"][1]*4))
        alphas = [Image.open(Path(job["panels"][side]["source"])).convert("RGBA").getchannel("A") for side in ("left", "right")]
        prepared.append((city, job, split_panels(joined, *alphas, 4, 6)))
    destination.mkdir()
    records = {}
    for city, job, panels in prepared:
        for side, image in zip(("left", "right"), panels):
            panel = job["panels"][side]
            upscale.publish_png(image, destination/panel["name"])
            records[panel["name"]] = {"city": city, "side": side, **panel,
                                      "output_sha256": upscale.file_digest(destination/panel["name"]),
                                      "alpha_preserved_from_original": True}
    audit.write_new(staging/"neural-candidate-records.json", {"format": "mc3-garage-photo-joined-candidates-v1",
        "plan_sha256": upscale.file_digest(staging/"joined-plan.json"),
        "planned_ncnn_context": plan["planned_ncnn_context"],
        "inference_provenance_validated": actual is not None,
        "actual_inference": actual, "inference_record_sha256": upscale.file_digest(Path(inference_record)) if actual else None,
        "records": records, "active_replacements_modified": False})
    return records


def run_cpu(staging, stop_file=None):
    """Run only four pinned composites with the separately validated CPU backend; no GPU scheduling."""
    import cpu_x4plus
    staging = candidates.safe_staging(staging)
    plan_path = staging/"joined-plan.json"
    plan = json.loads(plan_path.read_text())
    if (plan.get("format") != "mc3-garage-photo-joined-plan-v1" or set(plan.get("jobs", {})) != set("adst")
            or plan["worker_sha256"] != upscale.file_digest(Path(__file__))):
        raise ValueError("CPU joined plan must pin this helper and exactly four original city composites")
    stop_file = Path(stop_file) if stop_file else ROOT/".local/stop-loading-cpu"
    record_path = staging/"inference-record.json"
    with candidates.exclusive_candidate_lock(ROOT/".local/loading-art-candidate-worker.lock"):
        if stop_file.exists():
            return 2
        engine = cpu_x4plus.CpuX4Plus(threads=2)
        record = {"format": "mc3-garage-photo-inference-v1", "phase": "running",
                  "plan_sha256": upscale.file_digest(plan_path), "engine": engine.metadata, "records": {}}
        if record_path.exists():
            record = json.loads(record_path.read_text())
            if record.get("plan_sha256") != upscale.file_digest(plan_path) or record.get("engine") != engine.metadata:
                raise ValueError("CPU partial records belong to another plan/backend; preserve and use a new stage")
        upscale.publish_json(record, record_path)
        # The GPU stop request remains untouched. Parent explicitly authorized this bounded CPU operation;
        # its separate stop marker is checked between every inference tile.
        for city in "stad":
            name = f"garage_{city}.png"
            source, destination = staging/"network-input"/name, staging/"network-output"/name
            pinned = plan["jobs"][city]["network_input_sha256"]
            previous = record["records"].get(name)
            if previous:
                if upscale.file_digest(source) != pinned or upscale.file_digest(destination) != previous["output_sha256"]:
                    raise ValueError("completed CPU composite source/output changed")
                continue
            def progress(done, total):
                if done == 1 or done % 8 == 0 or done == total:
                    print(f"CPU garage {city}: tile {done}/{total}", flush=True)
            try:
                result = cpu_x4plus.upscale_original(source, destination, original_root=staging/"network-input",
                    expected_source_sha256=pinned, tile=96, tile_pad=32, engine=engine,
                    max_pixels=570*320, stop_file=stop_file, progress=progress)
            except InterruptedError:
                record["phase"] = "stopped"
                upscale.publish_json(record, record_path)
                return 2
            record["records"][name] = result
            upscale.publish_json(record, record_path)
            print(f"CPU garage {city}: complete in {result['elapsed_seconds']:.1f}s", flush=True)
        record["phase"] = "complete"
        upscale.publish_json(record, record_path)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staging", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=ROOT/"templates/game/assets")
    parser.add_argument("--textures", type=Path, default=upscale.TEXTURES)
    parser.add_argument("--model", choices=sorted(upscale.MODELS), default=upscale.DEFAULT_MODEL)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--split-network", action="store_true")
    action.add_argument("--run-cpu", action="store_true", help="Exactly four pinned original composites; CPU-only, two threads")
    parser.add_argument("--cpu-stop-file", type=Path)
    parser.add_argument("--inference-record", type=Path, help="Exact CPU backend records; absent means unauthenticated diagnostic outputs")
    args = parser.parse_args()
    if args.run_cpu:
        return run_cpu(args.staging, args.cpu_stop_file)
    result = split_network(args.staging, args.inference_record) if args.split_network else prepare(args.assets, args.textures, args.staging, args.model)
    print(json.dumps({"staging": str(args.staging), "groups": len(result.get("jobs", result)), "gpu_started": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
