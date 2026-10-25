import copy
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image

import reviewed_fixes as fixes
import loading_garage_join as joined


class ReviewedFixTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="mc3-reviewed-tests-");self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.textures=self.root/"textures";self.stage=".local/review-proof"
        self.name="0000000000000001.png";self.pass_id="original-pass";self.model="ultrasharp-4x"
        for folder in ("source","replace","reviewed_archive/base"):(self.textures/folder).mkdir(parents=True)
        (self.root/self.stage).mkdir(parents=True)
        self.pipeline=self.root/"tools/texture_pack/upscale.py";self.pipeline.parent.mkdir(parents=True)
        self.pipeline.write_bytes(b"immutable original pipeline")
        self.weights=self.root/"upscale_tools/x4plus.pth";self.weights.parent.mkdir();self.weights.write_bytes(b"pinned test checkpoint")
        self.module=self.root/"tools/texture_pack/cpu_x4plus.py";self.module.write_bytes(Path(fixes.cpu_x4plus.__file__).read_bytes())
        self.worker=self.root/"tools/texture_pack/loading_garage_join.py";self.worker.write_bytes(Path(joined.__file__).read_bytes())
        models=self.root/"models";models.mkdir()
        for suffix in (".param",".bin"):(models/(self.model+suffix)).write_bytes(suffix.encode())
        for target,attribute,value in ((fixes.upscale,"__file__",str(self.pipeline)),
                (fixes.upscale,"MODELS",{self.model:models}),(fixes.cpu_x4plus,"DEFAULT_WEIGHTS",self.weights)):
            patch=mock.patch.object(target,attribute,value);patch.start();self.addCleanup(patch.stop)
        self.dependencies={};self.completed={};self.panels={}
        yy,xx=np.indices((256,256))
        for index in range(1,9):
            name=f"{index:016X}.png"
            rgb=np.stack(((xx+index*17)%256,(yy+index*11)%256,(xx+yy)%256),axis=-1).astype(np.uint8)
            alpha=np.full((256,256),255,np.uint8);alpha[0,:]=0
            image=Image.fromarray(np.dstack((rgb,alpha)))
            source=self.textures/"source"/name;image.save(source)
            original=image.resize((1024,1024),Image.Resampling.LANCZOS)
            current=self.textures/"replace"/name;original.save(current)
            self.completed[name]={"source_sha256":fixes.upscale.file_digest(source),"output_sha256":fixes.upscale.file_digest(current)}
            self.panels[name]={"name":name,"source":str(source),"source_sha256":self.completed[name]["source_sha256"],
                "source_alpha_sha256":fixes.hashlib.sha256(alpha.tobytes()).hexdigest(),
                "base_replacement_sha256":self.completed[name]["output_sha256"],"cpu_fallback_sha256":"0"*64}
        archive=self.textures/"reviewed_archive/base"/self.name
        shutil.copyfile(self.textures/"replace"/self.name,archive)
        self.context=fixes.upscale.pass_context(self.model)
        self.write(self.textures/"upscale_passes.json",{"passes":{self.pass_id:{"context":self.context,"completed":self.completed}}})
        self.write(self.textures/"upscale_state.json",{name:self.model for name in self.completed})
        self.engine={"backend":"pytorch-cpu-x4plus-v1","device":"cpu","precision":"float32",
            "architecture":copy.deepcopy(fixes.cpu_x4plus.ARCHITECTURE),"intra_op_threads":2,"inter_op_threads":2,
            "state_key":"params_ema","validated_state_keys":702,"parameters":16697987,"ncnn_equivalence_claimed":False,
            "weight_key_validation":{"strict":True,"tensor_keys":702,"dtype":"float32","device":"cpu"},
            "model_path":str(self.weights),"model_sha256":fixes.upscale.file_digest(self.weights),
            "module_path":str(self.module),"module_sha256":fixes.upscale.file_digest(self.module)}
        jobs={};inference_records={};split_records={}
        for number,city in enumerate("adst"):
            names=[f"{number*2+side:016X}.png" for side in (1,2)]
            originals=[fixes.rgba(self.textures/"source"/name) for name in names]
            composite=joined.join_rgb(*originals,6)
            bundle=self.root/f"assets/flash/loadscreen_garage_{city}.xbck";bundle.parent.mkdir(parents=True,exist_ok=True);bundle.write_bytes(city.encode())
            input_path=self.root/self.stage/f"network-input/garage_{city}.png";input_path.parent.mkdir(exist_ok=True)
            Image.fromarray(fixes.upscale.edge_pad(composite,32,False,False)).save(input_path)
            output_path=self.root/self.stage/f"network-output/garage_{city}.png";output_path.parent.mkdir(exist_ok=True)
            Image.new("RGB",(2280,1280),(17+number,90,180)).save(output_path)
            inference_records[input_path.name]={**self.engine,"source_sha256":fixes.upscale.file_digest(input_path),
                "output_sha256":fixes.upscale.file_digest(output_path),"tile":96,"tile_pad":32}
            panels={side:self.panels[name] for side,name in zip(("left","right"),names)}
            jobs[city]={"bundle":str(bundle),"bundle_sha256":fixes.upscale.file_digest(bundle),"panels":panels,
                "joined_size":[506,256],"scale":4,"pad":32,"overlap_pixels":6,"integer_pitch_pixels":250,
                "joined_source_rgb_sha256":fixes.hashlib.sha256(composite.tobytes()).hexdigest(),
                "network_input_sha256":fixes.upscale.file_digest(input_path)}
            outputs=joined.split_panels(Image.new("RGB",(2024,1024),(17+number,90,180)),
                *(Image.fromarray(p[...,3]) for p in originals),4,6)
            candidates=self.root/self.stage/"neural-candidates";candidates.mkdir(exist_ok=True)
            for side,name,image in zip(("left","right"),names,outputs):
                image.save(candidates/name)
                split_records[name]={"city":city,"side":side,**panels[side],
                    "output_sha256":fixes.upscale.file_digest(candidates/name),"alpha_preserved_from_original":True}
        self.plan={"format":"mc3-garage-photo-joined-plan-v1","jobs":jobs,"worker_sha256":fixes.upscale.file_digest(self.worker),
                   "active_replacements_modified":False,"base_named_pass_records_modified":False}
        plan_path=self.root/self.stage/"joined-plan.json";self.write(plan_path,self.plan)
        self.inference={"format":"mc3-garage-photo-inference-v1","phase":"complete","engine":self.engine,
            "plan_sha256":fixes.upscale.file_digest(plan_path),"records":inference_records}
        inference_path=self.root/self.stage/"inference-record.json";self.write(inference_path,self.inference)
        self.split={"format":"mc3-garage-photo-joined-candidates-v1","plan_sha256":fixes.upscale.file_digest(plan_path),
            "inference_provenance_validated":True,"actual_inference":self.inference,
            "inference_record_sha256":fixes.upscale.file_digest(inference_path),"records":split_records,"active_replacements_modified":False}
        self.write(self.root/self.stage/"neural-candidate-records.json",self.split)
        shutil.copyfile(self.root/self.stage/"neural-candidates"/self.name,self.textures/"replace"/self.name)
        self.entry={"source_sha256":self.completed[self.name]["source_sha256"],
            "before_sha256":self.completed[self.name]["output_sha256"],
            "after_sha256":fixes.upscale.file_digest(self.textures/"replace"/self.name),
            "base_archive":"textures/reviewed_archive/base/"+self.name,"review_proof":self.stage+"/review.json",
            "candidate":{"kind":fixes.RECIPE,"stage":self.stage}}
        comparison=self.root/self.stage/"comparison.png";Image.new("RGB",(32,32),(1,2,3)).save(comparison)
        self.review={"format":"mc3-texture-review-v1","decision":"accepted","reviewer":"fixture reviewer",
            "comparison":self.stage+"/comparison.png","entries":{self.name:{key:self.entry[key] for key in
                ("source_sha256","before_sha256","after_sha256")}}}
        self.write(self.root/self.stage/"review.json",self.review)
        self.refresh_dependencies()
        self.ledger={"format":fixes.FORMAT,"base_model":self.model,"base_pass_id":self.pass_id,
                     "base_context":self.context,"dependencies":self.dependencies,"entries":{self.name:self.entry}}
        self.write_ledger()

    def write(self,path,value):
        Path(path).write_text(json.dumps(value),encoding="utf-8")

    def refresh_dependencies(self):
        self.dependencies.update({path.relative_to(self.root).as_posix():fixes.upscale.file_digest(path)
                                  for path in self.root.rglob("*") if path.is_file() and path.name!="reviewed_fixes.json"})

    def write_ledger(self):self.write(self.textures/"reviewed_fixes.json",self.ledger)
    def audit(self):return fixes.audit(self.textures,self.model,self.pass_id,root=self.root)
    def reject(self):
        report=self.audit();self.assertFalse(report["complete"])
        self.assertTrue(report["issues"]["reviewed_fix_errors"])
        return report

    def test_full_chain_accounts_only_for_changed_named_record(self):
        before_pass=(self.textures/"upscale_passes.json").read_bytes();before_png=(self.textures/"replace"/self.name).read_bytes()
        report=self.audit();self.assertTrue(report["complete"]);self.assertTrue(report["base_complete"])
        self.assertEqual(report["counts"]["base_named_pass_verified"],8)
        self.assertEqual(report["counts"]["reviewed_fixes"],1)
        self.assertEqual(report["base_audit"]["issues"]["missing_or_changed_pass_record"],[self.name])
        self.assertEqual(report["current_model"],"mixed-reviewed")
        self.assertEqual(report["reviewed_fixes"][self.name]["current_backend"],"pytorch-cpu-x4plus-v1")
        self.assertEqual((self.textures/"upscale_passes.json").read_bytes(),before_pass)
        self.assertEqual((self.textures/"replace"/self.name).read_bytes(),before_png)

    def test_actual_verified_chain_satisfies_coordinator_texture_gate(self):
        path=Path(__file__).resolve().parents[1]/"maintenance/overnight_pipeline.py"
        spec=importlib.util.spec_from_file_location("reviewed_pipeline_gate_fixture",path)
        pipeline=importlib.util.module_from_spec(spec);spec.loader.exec_module(pipeline)
        report=self.audit()
        pipeline.validate_texture_report(report,self.pass_id)
        report["base_audit"]["issues"]["orphan_replacements"].append("UNKNOWN.png")
        with self.assertRaises(RuntimeError):pipeline.validate_texture_report(report,self.pass_id)

    def test_no_ledger_exact_original_audit_parity(self):
        (self.textures/"reviewed_fixes.json").unlink()
        original={"complete":False,"counts":{"eligible":8},"opaque":"unchanged"}
        with mock.patch.object(fixes.original_audit,"audit",return_value=original) as called:
            self.assertIs(self.audit(),original);called.assert_called_once_with(self.textures,self.model,self.pass_id)

    def test_early_global_and_malformed_entries_are_reported_without_crash(self):
        original=copy.deepcopy(self.ledger)
        for value in (None,[],"invalid",{},[self.entry]):
            with self.subTest(entries=value):
                self.ledger=copy.deepcopy(original);self.ledger["entries"]=value;self.write_ledger();self.reject()
        self.ledger=copy.deepcopy(original);self.ledger["format"]="wrong";self.write_ledger();self.reject()
        self.write(self.textures/"reviewed_fixes.json",[]);self.reject()

    def test_missing_archive_proof_and_source_fail_closed(self):
        for relative in (self.entry["base_archive"],self.entry["review_proof"],"textures/source/"+self.name):
            path=self.root/relative;data=path.read_bytes();path.unlink()
            with self.subTest(path=relative):self.reject()
            path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)

    def test_model_pipeline_and_proof_tampering_fail_closed(self):
        for path in (self.weights,self.pipeline,self.root/self.stage/"joined-plan.json"):
            data=path.read_bytes();path.write_bytes(data+b"tampered")
            with self.subTest(path=path):self.reject()
            path.write_bytes(data)

    def test_partial_installation_is_not_success(self):
        shutil.copyfile(self.root/self.entry["base_archive"],self.textures/"replace"/self.name)
        self.reject()

    def test_repinning_bad_alpha_still_fails_semantic_check(self):
        path=self.textures/"replace"/self.name
        with Image.open(path) as image:
            image.putalpha(255);image.save(path)
        self.entry["after_sha256"]=fixes.upscale.file_digest(path)
        self.dependencies["textures/replace/"+self.name]=self.entry["after_sha256"];self.write_ledger()
        self.assertIn("alpha",str(self.reject()["issues"]["reviewed_fix_errors"]))

    def test_named_mask_cannot_be_excused(self):
        (self.textures/"vinyl_hashes.txt").write_text(self.name[:16])
        self.refresh_dependencies();self.write_ledger()
        report=self.reject();self.assertIn(self.name,report["issues"]["active_masks"])

    def test_traversal_absolute_and_unpinned_paths_fail_closed(self):
        for path in ("../outside.png","/absolute.png","C:/absolute.png","a/../b","a\\b","a//b"):
            with self.subTest(path=path):
                old=copy.deepcopy(self.ledger)
                self.ledger["dependencies"][path]="0"*64;self.write_ledger();self.reject()
                self.ledger=old;self.dependencies=self.ledger["dependencies"];self.write_ledger()

    def test_review_before_after_decision_is_required(self):
        for mutation in (lambda value:value.update(decision="pending"),
                         lambda value:value["entries"][self.name].update(before_sha256="0"*64)):
            review=copy.deepcopy(self.review);mutation(review)
            path=self.root/self.entry["review_proof"];self.write(path,review)
            self.dependencies[self.entry["review_proof"]]=fixes.upscale.file_digest(path);self.write_ledger();self.reject()

    def test_incomplete_cpu_proof_is_not_authenticated(self):
        self.inference["phase"]="running"
        path=self.root/self.stage/"inference-record.json";self.write(path,self.inference)
        self.dependencies[self.stage+"/inference-record.json"]=fixes.upscale.file_digest(path);self.write_ledger();self.reject()

    def test_other_original_audit_issue_is_never_hidden(self):
        real=fixes.original_audit.audit(self.textures,self.model,self.pass_id)
        real["issues"]["orphan_replacements"].append("UNRELATED.png")
        with mock.patch.object(fixes.original_audit,"audit",return_value=real):
            result=self.audit()
        self.assertFalse(result["complete"]);self.assertEqual(result["issues"]["orphan_replacements"],["UNRELATED.png"])
        self.assertFalse(result["issues"]["missing_or_changed_pass_record"])

    def test_repinning_wrong_candidate_pixels_fails_recomputed_split(self):
        path=self.root/self.stage/"neural-candidates"/self.name
        with Image.open(path) as image:
            value=np.asarray(image).copy();value[100,100,0]^=255;Image.fromarray(value).save(path)
        self.split["records"][self.name]["output_sha256"]=fixes.upscale.file_digest(path)
        split_path=self.root/self.stage/"neural-candidate-records.json";self.write(split_path,self.split)
        for changed in (path,split_path):self.dependencies[changed.relative_to(self.root).as_posix()]=fixes.upscale.file_digest(changed)
        self.write_ledger();self.assertIn("split",str(self.reject()["issues"]["reviewed_fix_errors"]))


if __name__=="__main__":unittest.main()
