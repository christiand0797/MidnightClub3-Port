"""Read-only audit of an immutable original pass plus explicitly reviewed current texture corrections.

No ledger means exact delegation to audit.py. This tool cannot install, adopt, repair or generate a ledger.
The initial supported correction recipe is the separately authenticated joined-garage CPU x4plus candidate.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re

import numpy as np
from PIL import Image

import audit as original_audit
import cpu_x4plus
import upscale

ROOT = Path(__file__).resolve().parents[2]
FORMAT = "mc3-reviewed-texture-fixes-v1"
RECIPE = "garage-joined-cpu-x4plus-v1"
NAME = re.compile(r"[0-9A-F]{16}\.png\Z")
SHA = re.compile(r"[0-9a-f]{64}\Z")


def relative_path(value):
    if not isinstance(value,str) or not value or "\\" in value or ":" in value:
        raise ValueError("dependency path must be normalized project-relative POSIX text")
    path=PurePosixPath(value)
    if path.is_absolute() or any(part in (".","..") for part in value.split("/")) or path.as_posix()!=value:
        raise ValueError("absolute/traversing/non-normalized dependency path")
    return path


class Files:
    def __init__(self,root,dependencies):
        self.root=Path(root).resolve()
        if not isinstance(dependencies,dict) or not dependencies:
            raise ValueError("reviewed ledger needs pinned dependencies")
        self.dependencies=dependencies
        self.verify_all()

    def path(self,value):
        path=(self.root/relative_path(value)).resolve()
        if self.root not in path.parents:
            raise ValueError("dependency escapes project through a link")
        return path

    def checked(self,value,expected=None):
        path=self.path(value)
        pinned=self.dependencies.get(value)
        if not isinstance(pinned,str) or not SHA.fullmatch(pinned) or (expected is not None and pinned!=expected):
            raise ValueError(f"dependency/hash not pinned: {value}")
        if not path.is_file() or upscale.file_digest(path)!=pinned:
            raise ValueError(f"dependency missing/changed: {value}")
        return path

    def verify_all(self):
        for value in self.dependencies:
            self.checked(value)

    def json(self,value):
        return json.loads(self.checked(value).read_text(encoding="utf-8"))

    def rel(self,path):
        return Path(path).resolve().relative_to(self.root).as_posix()


def recorded_path_matches(value,expected_relative):
    """Historical proof files contain absolute paths; never open them. Compare only their safe relative suffix."""
    if not isinstance(value,str):
        return False
    path=PureWindowsPath(value) if "\\" in value or PureWindowsPath(value).drive else PurePosixPath(value)
    expected=relative_path(expected_relative).parts
    return ".." not in path.parts and tuple(part.casefold() for part in path.parts[-len(expected):])==tuple(
        part.casefold() for part in expected)


def rgba(path):
    with Image.open(path) as image:
        image.load()
        return np.asarray(image.convert("RGBA"))


def verify_alpha(source,current,size):
    pixels=rgba(current)
    if list(pixels.shape[1::-1])!=size:
        raise ValueError("reviewed image dimensions differ from original pass scale")
    expected=np.asarray(Image.fromarray(rgba(source)[...,3]).resize(tuple(size),Image.Resampling.LANCZOS))
    if not np.array_equal(pixels[...,3],expected):
        raise ValueError("reviewed/base alpha is not exact original Lanczos alpha")
    return hashlib.sha256(expected.tobytes()).hexdigest()


def verify_cpu_record(files,record,engine,input_rel,output_rel,source_hash):
    if (not isinstance(record,dict) or record.get("source_sha256")!=source_hash
            or record.get("device")!="cpu" or record.get("backend")!="pytorch-cpu-x4plus-v1"
            or record.get("architecture")!=cpu_x4plus.ARCHITECTURE
            or record.get("weight_key_validation")!={"strict":True,"tensor_keys":702,"dtype":"float32","device":"cpu"}
            or record.get("validated_state_keys")!=702 or record.get("parameters")!=16697987
            or record.get("precision")!="float32" or record.get("state_key") not in ("params_ema","params")
            or any(not 1<=record.get(k,0)<=2 for k in ("intra_op_threads","inter_op_threads"))
            or not 8<=record.get("tile",0)<=96 or not 0<=record.get("tile_pad",-1)<=32
            or record.get("ncnn_equivalence_claimed") is not False):
        raise ValueError("CPU x4plus inference is incomplete/unrecognized")
    if any(record.get(key)!=value for key,value in engine.items()):
        raise ValueError("per-image inference disagrees with validated engine recipe")
    for prefix,expected in (("module","tools/texture_pack/cpu_x4plus.py"),
                            ("model",files.rel(cpu_x4plus.DEFAULT_WEIGHTS))):
        if not recorded_path_matches(record.get(prefix+"_path"),expected):
            raise ValueError("inference dependency is not the supported project file")
        files.checked(expected,record.get(prefix+"_sha256"))
    files.checked(input_rel,source_hash)
    files.checked(output_rel,record.get("output_sha256"))


def garage_candidate(files,stage,name,textures_rel,cache):
    relative_path(stage)
    key=(stage,name)
    if key in cache:
        return cache[key]
    plan_rel,inference_rel,split_rel=(stage+"/"+file for file in
        ("joined-plan.json","inference-record.json","neural-candidate-records.json"))
    plan,inference,split=(files.json(path) for path in (plan_rel,inference_rel,split_rel))
    if (plan.get("format")!="mc3-garage-photo-joined-plan-v1" or set(plan.get("jobs",{}))!=set("adst")
            or plan.get("active_replacements_modified") is not False or plan.get("base_named_pass_records_modified") is not False
            or inference.get("format")!="mc3-garage-photo-inference-v1" or inference.get("phase")!="complete"
            or inference.get("plan_sha256")!=files.dependencies[plan_rel]
            or set(inference.get("records",{}))!={f"garage_{city}.png" for city in "adst"}
            or split.get("format")!="mc3-garage-photo-joined-candidates-v1"
            or split.get("plan_sha256")!=files.dependencies[plan_rel]
            or split.get("inference_provenance_validated") is not True or split.get("actual_inference")!=inference
            or split.get("inference_record_sha256")!=files.dependencies[inference_rel]
            or split.get("active_replacements_modified") is not False):
        raise ValueError("joined photo proof is incomplete or disagrees with pinned plan/inference")
    files.checked("tools/texture_pack/loading_garage_join.py",plan.get("worker_sha256"))
    panel_names=[p["name"] for job in plan["jobs"].values() for p in job["panels"].values()]
    if len(panel_names)!=8 or len(set(panel_names))!=8 or set(split.get("records",{}))!=set(panel_names):
        raise ValueError("split proof does not cover exactly the original panels")
    record=split["records"].get(name)
    if not record or record.get("alpha_preserved_from_original") is not True:
        raise ValueError("reviewed texture has no authenticated joined candidate")
    city,side=record["city"],record["side"]
    if city not in set("adst") or side not in ("left","right"):
        raise ValueError("unknown photo city/side")
    job=plan["jobs"][city]
    if (job.get("joined_size")!=[506,256] or job.get("scale")!=4 or job.get("pad")!=32
            or job.get("overlap_pixels")!=6 or job.get("integer_pitch_pixels")!=250):
        raise ValueError("unsupported joined original geometry recipe")
    # The original bundle is an explicit pinned dependency; source pixels remain anchored by their original-pass hashes.
    bundle_rel=next((value for value in files.dependencies if recorded_path_matches(job.get("bundle"),value)),None)
    if bundle_rel is None:
        raise ValueError("original Flash bundle is not pinned")
    files.checked(bundle_rel,job.get("bundle_sha256"))
    original=[]
    for panel_side in ("left","right"):
        panel=job["panels"][panel_side]
        source_rel=textures_rel+"/source/"+panel["name"]
        if not NAME.fullmatch(panel["name"]) or not recorded_path_matches(panel.get("source"),source_rel):
            raise ValueError("candidate source is not its original named texture")
        path=files.checked(source_rel,panel.get("source_sha256"));pixels=rgba(path)
        if pixels.shape!=(256,256,4) or hashlib.sha256(pixels[...,3].tobytes()).hexdigest()!=panel.get("source_alpha_sha256"):
            raise ValueError("original panel/alpha changed")
        original.append(pixels)
    panel=job["panels"][side]
    if panel["name"]!=name or any(record.get(key)!=value for key,value in panel.items()):
        raise ValueError("split record disagrees with original panel plan")
    # Recompute original-derived composite and expected split pixels; no neural inference is repeated by an audit.
    import loading_garage_join as joined
    composite=joined.join_rgb(*original,6)
    if hashlib.sha256(composite.tobytes()).hexdigest()!=job.get("joined_source_rgb_sha256"):
        raise ValueError("original composite recipe/hash changed")
    input_rel=stage+f"/network-input/garage_{city}.png"
    output_rel=stage+f"/network-output/garage_{city}.png"
    cpu=inference["records"][f"garage_{city}.png"]
    verify_cpu_record(files,cpu,inference.get("engine",{}),input_rel,output_rel,job.get("network_input_sha256"))
    expected_input=upscale.edge_pad(composite,32,False,False)
    with Image.open(files.checked(input_rel)) as image:
        if not np.array_equal(np.asarray(image.convert("RGB")),expected_input):
            raise ValueError("network input is not the pinned original-derived reflected composite")
    with Image.open(files.checked(output_rel)) as image:
        if image.size!=(2280,1280):
            raise ValueError("joined inference dimensions changed")
        value=image.convert("RGB").crop((128,128,2152,1152))
    outputs=joined.split_panels(value,*(Image.fromarray(p[...,3]) for p in original),4,6)
    expected=outputs[0 if side=="left" else 1]
    candidate_rel=stage+"/neural-candidates/"+name
    candidate=files.checked(candidate_rel,record.get("output_sha256"))
    if not np.array_equal(rgba(candidate),np.asarray(expected)):
        raise ValueError("candidate pixels are not the exact authenticated network split")
    result={"candidate":candidate,"record":record,"recipe":RECIPE,"model_sha256":cpu["model_sha256"],
            "proofs":[plan_rel,inference_rel,split_rel]}
    cache[key]=result
    return result


def verify_entry(files,textures,model,selected,name,entry,excluded,found,cache):
    if not NAME.fullmatch(name) or name in excluded or name not in found:
        raise ValueError("unknown texture, malformed name or categorical mask cannot be reviewed away")
    source_rel=files.rel(found[name]);current_rel=files.rel(textures/"replace"/name)
    source=files.checked(source_rel,entry["source_sha256"])
    current=files.checked(current_rel,entry["after_sha256"])
    archive_rel=entry["base_archive"]
    archive_prefix=files.rel(textures)+"/reviewed_archive/"
    if not archive_rel.startswith(archive_prefix):
        raise ValueError("base archive must be outside active files in textures/reviewed_archive")
    archive=files.checked(archive_rel,entry["before_sha256"])
    if entry["before_sha256"]==entry["after_sha256"]:
        raise ValueError("reviewed correction must identify distinct before/after bytes")
    base=selected.get("completed",{}).get(name)
    if not upscale.pass_complete(base,source,archive):
        raise ValueError("archive/source do not match the immutable original named-pass record")
    job,_pixels,reason=upscale.inspect_source(source)
    if job is None:
        raise ValueError("ineligible original texture: "+str(reason))
    size=[value*job["scale"] for value in job["size"]]
    verify_alpha(source,archive,size);alpha=verify_alpha(source,current,size)
    proof=files.json(entry["review_proof"])
    review=proof.get("entries",{}).get(name,{})
    if (proof.get("format")!="mc3-texture-review-v1" or proof.get("decision")!="accepted"
            or not isinstance(proof.get("reviewer"),str) or not proof["reviewer"].strip()
            or review.get("before_sha256")!=entry["before_sha256"]
            or review.get("after_sha256")!=entry["after_sha256"] or review.get("source_sha256")!=entry["source_sha256"]):
        raise ValueError("explicit reviewed before/after/source evidence is missing")
    comparison=files.checked(proof["comparison"])
    if comparison in (source,current,archive):
        raise ValueError("review comparison must be its own artifact")
    with Image.open(comparison) as image:
        image.load()
        if min(image.size)<16:
            raise ValueError("review comparison is not a readable image")
    candidate=entry.get("candidate",{})
    if candidate.get("kind")!=RECIPE:
        raise ValueError("unsupported reviewed correction recipe")
    verified=garage_candidate(files,candidate["stage"],name,files.rel(textures),cache)
    if (upscale.file_digest(verified["candidate"])!=entry["after_sha256"]
            or verified["record"]["base_replacement_sha256"]!=entry["before_sha256"]):
        raise ValueError("current output is not the reviewed authenticated candidate")
    return {"recipe":RECIPE,"current_backend":"pytorch-cpu-x4plus-v1","model_sha256":verified["model_sha256"],
            "source_sha256":entry["source_sha256"],"before_sha256":entry["before_sha256"],
            "after_sha256":entry["after_sha256"],"alpha_sha256":alpha,"base_archive":archive_rel,
            "review_proof":entry["review_proof"],"candidate_proofs":verified["proofs"]}


def audit(textures,model,pass_id=None,*,root=ROOT):
    textures=Path(textures)
    ledger_path=textures/"reviewed_fixes.json"
    base=original_audit.audit(textures,model,pass_id)
    if not ledger_path.exists():
        return base
    effective=copy.deepcopy(base["issues"]);verified={};errors={};entries={};ledger_sha=None
    try:
        data=ledger_path.read_bytes();ledger_sha=hashlib.sha256(data).hexdigest()
        ledger=json.loads(data.decode("utf-8"))
        if (ledger.get("format")!=FORMAT or not pass_id or ledger.get("base_model")!=model
                or ledger.get("base_pass_id")!=pass_id):
            raise ValueError("reviewed ledger requires the exact selected original named pass/model")
        files=Files(root,ledger.get("dependencies"));textures_rel=files.rel(textures)
        passes=files.json(textures_rel+"/upscale_passes.json")
        selected=passes.get("passes",{}).get(pass_id,{})
        context=upscale.pass_context(model)
        if selected.get("context")!=context or ledger.get("base_context")!=context:
            raise ValueError("original pipeline/model dependency context changed")
        files.checked(files.rel(Path(upscale.__file__)),context["pipeline_sha256"])
        for suffix,value in context["models"].items():
            files.checked(files.rel(upscale.MODELS[model]/(model+suffix)),value)
        for filename in ("upscale_state.json","source_index.json","vinyl_hashes.txt"):
            if (textures/filename).exists():files.checked(textures_rel+"/"+filename)
        raw_entries=ledger.get("entries")
        if not isinstance(raw_entries,dict) or not 1<=len(raw_entries)<=8:
            raise ValueError("reviewed ledger must contain explicit correction entries")
        entries=raw_entries
        excluded=upscale.excluded_masks(textures);found=upscale.source_paths(textures);cache={}
        for name,entry in entries.items():
            try:
                verified[name]=verify_entry(files,textures,model,selected,name,entry,excluded,found,cache)
            except (OSError,ValueError,TypeError,KeyError,IndexError,AttributeError) as error:
                errors[name]=str(error)
        files.verify_all()
        if upscale.file_digest(ledger_path)!=ledger_sha:
            raise ValueError("reviewed ledger changed during audit")
    except (OSError,ValueError,TypeError,KeyError,IndexError,AttributeError) as error:
        errors["ledger"]=str(error)
        verified={}  # A global dependency failure invalidates every otherwise matching entry.
    missing=effective.get("missing_or_changed_pass_record",[])
    accounted=[name for name in missing if name in verified]
    effective["missing_or_changed_pass_record"]=[name for name in missing if name not in verified]
    base_verified=base["counts"].get("complete_named_pass",0)+len(accounted)
    base_complete=(base_verified==base["counts"].get("eligible",0) and not any(effective.values()))
    effective["reviewed_fix_errors"]=errors
    return {"format":"mc3-reviewed-texture-audit-v1","base_model":model,"base_pass_id":pass_id,
            "current_model":"mixed-reviewed" if verified else "unverified-reviewed-state","base_complete":base_complete,
            "complete":base_complete and not errors and len(verified)==len(entries),
            "counts":{"eligible":base["counts"].get("eligible",0),"base_named_pass_verified":base_verified,
                      "current_original_named_pass":base["counts"].get("complete_named_pass",0),
                      "reviewed_fixes":len(verified),"accounted_changed_pass_records":len(accounted)},
            "issues":effective,"base_audit":base,"reviewed_fixes":verified,
            "ledger_sha256":ledger_sha,
            "provenance_limit":"Checksums authenticate the pinned recorded chain, not who reviewed it; neural inference is not rerun."}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--textures",type=Path,default=upscale.TEXTURES)
    parser.add_argument("--model",choices=sorted(upscale.MODELS),default=upscale.DEFAULT_MODEL)
    parser.add_argument("--pass-id")
    parser.add_argument("--output",type=Path)
    args=parser.parse_args();report=audit(args.textures,args.model,args.pass_id)
    if args.output:
        if (ROOT/".local").resolve() not in args.output.resolve().parents or args.output.suffix.lower()!=".json":
            parser.error("audit reports must be new JSON files under .local")
        if args.output.exists():parser.error("audit output already exists; no overwrite")
        args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open("x",encoding="utf-8") as stream:json.dump(report,stream,indent=2,allow_nan=False)
    print(json.dumps({key:report[key] for key in ("complete","counts")},indent=2))
    return 0 if report["complete"] else 1


if __name__=="__main__":raise SystemExit(main())
