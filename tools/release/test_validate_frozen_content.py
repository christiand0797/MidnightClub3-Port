"""Tiny synthetic frozen cohorts and real CPU H264 decoding; no actual game/media/proof inputs are used."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

from PIL import Image

import validate_frozen_content as check


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2)+"\n").encode()


def png(color, mode="RGBA", size=(2, 2)):
    output = io.BytesIO()
    Image.new(mode, size, color).save(output, format="PNG")
    return output.getvalue()


class FrozenValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api, cls.ff, cls.decoder = check.video_api()
        cls.workspace = check.ROOT / "out/frozen-validator-fixtures"
        cls.workspace.mkdir(parents=True, exist_ok=True)
        cls.cache = tempfile.TemporaryDirectory(dir=cls.workspace)
        cls.encoded_movies = {}
        for label, fps, frames, offset in (("good", "2997/100", 3, None), ("short", "2997/100", 2, None),
                                           ("rate", "30", 3, None), ("offset", "2997/100", 3, "1")):
            path = Path(cls.cache.name) / (label+".mp4")
            num, den = fps.split("/") if "/" in fps else (fps, "1")
            command = [str(cls.ff/"ffmpeg.exe"), "-v", "error", "-y", "-f", "lavfi", "-i", f"color=c=blue:s=12x12:r={fps}",
                "-frames:v", str(frames), "-an", "-c:v", "libx264", "-threads", "2", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p",
                "-enc_time_base", f"{den}:{num}", "-video_track_timescale", num, "-movflags", "+faststart"]
            if offset:
                command += ["-output_ts_offset", offset]
            subprocess.run(command+[str(path)], check=True, capture_output=True)
            cls.encoded_movies[label] = path.read_bytes()
        cls.good_info = cls.api.probe(cls.ff, Path(cls.cache.name)/"good.mp4", count_frames=True)
        cls.good_output = cls.api.validate_timeline(cls.good_info, "2997/100", 3)

    @classmethod
    def tearDownClass(cls):
        path = Path(cls.cache.name).resolve()
        if path.parent != cls.workspace.resolve():
            raise RuntimeError("Refusing fixture cleanup outside workspace")
        cls.cache.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=self.workspace)
        self.root = Path(self.temp.name).resolve()
        self.addCleanup(self.cleanup)
        self.snapshot, self.proofs = self.root/"snapshot", self.root/"proof"
        self.snapshot.mkdir(); self.proofs.mkdir()
        self.identity = {"name": "realesrgan-x4plus", "weights": {"fixture": "b"*64},
                         "executable_sha256": "c"*64, "inference_scale": 4, "unsharp": .25}
        self.kind, self.clip, self.marker = "strict-original-record", "good", None
        self.names = ["0000000000000001.png", "0000000000000002.png"]
        self.before, self.after = png((20, 30, 40, 255)), png((50, 60, 70, 255))
        self.unchanged = png((10, 20, 30), "RGB")
        self.iso = b"tiny ISO fixture"
        self.build()

    def cleanup(self):
        if self.root.parent != self.workspace.resolve():
            raise RuntimeError("Refusing cleanup outside fixture workspace")
        self.temp.cleanup()

    def put(self, relative, data):
        path = self.snapshot/relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        return {"bytes": len(data), "sha256": sha(data)}

    def build(self):
        # Rebuild only this fixture's files. Existing source/sidecar rows are removed explicitly, never recursively.
        for relative in ("evidence/video-records/story/tiny.upscale.json", "evidence/video-records/story/tiny.model.txt"):
            (self.snapshot/relative).unlink(missing_ok=True)
        context = {"model": "ultrasharp-4x", "pipeline_sha256": "a"*64}
        completed = {self.names[0]: {"output_sha256": sha(self.before)}, self.names[1]: {"output_sha256": sha(self.unchanged)}}
        archive_name = check.TEXTURES+"reviewed_archive/fixture/"+self.names[0]
        self.ledger = {"format": "mc3-reviewed-texture-fixes-v1", "base_model": "ultrasharp-4x", "base_pass_id": check.PASS_ID,
            "base_context": context, "entries": {self.names[0]: {"before_sha256": sha(self.before), "after_sha256": sha(self.after), "base_archive": archive_name}}}
        ledger_bytes = encoded(self.ledger)
        proof_files = {check.TEXTURES+"reviewed_fixes.json": ledger_bytes, check.TEXTURES+"upscale_passes.json":
            encoded({"passes": {check.PASS_ID: {"context": context, "completed": completed}}}),
            archive_name: self.before, check.TEXTURES+"replace/"+self.names[0]: self.after}
        zipped = self.proofs/"proof.zip"
        with zipfile.ZipFile(zipped, "w", zipfile.ZIP_DEFLATED) as target:
            for name in sorted(proof_files): target.writestr(name, proof_files[name])
        package = {"format": "mc3-reviewed-proof-package-v1", "complete": True,
            "ledger": check.TEXTURES+"reviewed_fixes.json", "ledger_sha256": sha(ledger_bytes),
            "archive": {"path": zipped.name, "bytes": zipped.stat().st_size, "sha256": sha(zipped.read_bytes())},
            "files": [{"path": n, "bytes": len(data), "sha256": sha(data)} for n, data in sorted(proof_files.items())]}
        proof_manifest = self.proofs/"manifest.json"; proof_manifest.write_bytes(encoded(package))
        self.put("game/game.iso", self.iso)
        self.put("textures/replace/"+self.names[0], self.after)
        self.put("textures/replace/"+self.names[1], self.unchanged)
        self.put("evidence/texture-records/reviewed_fixes.json", ledger_bytes)
        audit = {"format": "mc3-reviewed-texture-audit-v1", "complete": True, "base_complete": True,
            "base_model": "ultrasharp-4x", "base_pass_id": check.PASS_ID, "current_model": "mixed-reviewed",
            "ledger_sha256": sha(ledger_bytes), "issues": {"reviewed_fix_errors": {}},
            "counts": {"eligible": 2, "base_named_pass_verified": 2, "current_original_named_pass": 1, "reviewed_fixes": 1, "accounted_changed_pass_records": 1}}
        audit_row = self.put("evidence/texture-records/texture-audit.json", encoded(audit))
        self.put("evidence/current-model-identity.json", encoded(self.identity))
        source_bytes = struct.pack("<4s8I", b"BIKi", 100, 3, 50, 3, 4, 4, 2997, 100)
        source_name = "evidence/source-video/story/tiny.bik"
        source = self.put(source_name, source_bytes); source["header"] = {"width": 4, "height": 4, "frames": 3, "fps": "2997/100"}
        output = self.put("video_hd/story/tiny.mp4", self.encoded_movies[self.clip])
        recipe = {"pipeline_version": 1, "source_sha256": source["sha256"], "source_bytes": source["bytes"],
            "source": source["header"] | {"codec": "binkvideo", "pix_fmt": "yuv420p"}, "model": self.identity, "output_scale": 3,
            "encoder": {"codec": "libx264", "preset": "slow", "crf": 16, "pix_fmt": "yuv420p", "time_base": "100/2997", "video_track_timescale": 2997}}
        saved = {"recipe": recipe, "output": self.good_output, "output_sha256": output["sha256"],
                 "origin": "original-bink", "historical_source_and_weights_verified": True} if self.kind == "strict-original-record" else None
        anchors = {}
        for key, suffix, data in (("json", ".upscale.json", encoded(saved) if saved else None), ("marker", ".model.txt", self.marker)):
            relative = "evidence/video-records/story/tiny"+suffix
            if data is not None: self.put(relative, data)
            anchors[key] = {"present": data is not None, "path": relative if data is not None else None, "sha256": sha(data) if data is not None else None}
        row = output | {"classification": self.kind, "strict_recorded_provenance": self.kind == "strict-original-record",
                        "source_bink": source_name, "anchors": anchors, "recorded_metadata": saved}
        payload = {}
        for p in ["game/game.iso", *["textures/replace/"+n for n in self.names], "video_hd/story/tiny.mp4"]:
            payload[p] = {"bytes": (self.snapshot/p).stat().st_size, "sha256": sha((self.snapshot/p).read_bytes())}
        self.manifest = {"format": "mc3-frozen-content-cohort-v1", "status": "complete", "payload_files": 4,
            "texture_count": 2, "movie_count": 1, "source_bink_count": 1, "payload": payload,
            "movies": {"story/tiny.mp4": row}, "all_copied_sources": {source_name: source},
            "movie_cohort_counts": {self.kind: 1}, "model_identity": self.identity, "ledger_sha256": sha(ledger_bytes),
            "texture_audit_sha256": audit_row["sha256"]}
        self.seal()
        self.config = check.Config(self.snapshot, sha((self.snapshot/"snapshot-manifest.json").read_bytes()), proof_manifest,
            self.root/"report/result.json", sha(proof_manifest.read_bytes()), sha(zipped.read_bytes()), sha(ledger_bytes),
            textures=2, movies=1, reviewed=1, iso_sha256=sha(self.iso), external_g=False)

    def seal(self):
        self.manifest["inventory"] = {p.relative_to(self.snapshot).as_posix(): {"bytes": p.stat().st_size, "sha256": sha(p.read_bytes())}
            for p in self.snapshot.rglob("*") if p.is_file() and p.name != "snapshot-manifest.json"}
        (self.snapshot/"snapshot-manifest.json").write_bytes(encoded(self.manifest))
        if hasattr(self, "config"):
            self.config.snapshot_sha256 = sha((self.snapshot/"snapshot-manifest.json").read_bytes())

    def input_state(self):
        return {str(p): sha(p.read_bytes()) for base in (self.snapshot, self.proofs) for p in base.rglob("*") if p.is_file()}

    def test_default_is_a_readonly_plan(self):
        before = self.input_state(); result = check.validate(self.config)
        self.assertEqual(result["mode"], "plan"); self.assertFalse(self.config.report.parent.exists())
        self.assertEqual(self.input_state(), before)

    def test_real_cpu_decode_and_exact_timing_complete_without_mutation(self):
        before = self.input_state(); result = check.validate(self.config, execute=True)
        self.assertTrue(result["complete"], result)
        self.assertTrue(result["all_outputs_have_strict_recorded_provenance"])
        self.assertTrue(result["movies"]["story/tiny.mp4"]["exact_source_timing"]["exact_source_timing"])
        self.assertEqual(result["textures"]["png_chunks_verified_and_pixels_decoded"], 2)
        self.assertEqual(result["textures"]["modes"], {"RGBA": 1, "RGB": 1})
        self.assertEqual(self.input_state(), before)
        self.assertEqual(json.loads(self.config.report.read_text()), result)
        self.assertIs(result["inputs_modified"], False)
        self.assertTrue((self.config.report.parent/"progress.jsonl").read_text().count('"phase": "movie"') == 1)

    def test_historical_marker_exact_decode_never_gains_provenance(self):
        self.kind, self.marker = "historical-marker-only", b"realesrgan-x4plus\n"; self.build()
        before = self.input_state(); result = check.validate(self.config, execute=True)
        self.assertTrue(result["complete"], result)
        self.assertFalse(result["all_outputs_have_strict_recorded_provenance"])
        self.assertFalse(result["movies"]["story/tiny.mp4"]["strict_recorded_provenance"])
        self.assertEqual(self.input_state(), before)

    def test_wrong_frame_count_is_fatal_for_historical_output(self):
        self.kind, self.clip = "historical-unrecorded", "short"; self.build()
        result = check.validate(self.config, execute=True)
        self.assertEqual(result["status"], "failed"); self.assertIn("frame count", result["errors"]["story/tiny.mp4"])

    def test_wrong_rate_and_nonzero_pts_fail_strict_timing(self):
        for clip in ("rate", "offset"):
            with self.subTest(clip=clip):
                self.clip = clip; self.build(); self.config.report = self.root/("report-"+clip)/"result.json"
                result = check.validate(self.config, execute=True)
                self.assertEqual(result["status"], "failed", result)
                self.assertEqual(result["movies"]["story/tiny.mp4"]["compatibility"], "failed-strict-timing")

    def test_legacy_timing_is_separate_numeric_warning_and_no_retime(self):
        self.kind, self.clip = "historical-unrecorded", "rate"; self.build()
        before = self.input_state(); result = check.validate(self.config, execute=True)
        self.assertEqual(result["status"], "compatibility-pending", result)
        self.assertFalse(result["complete"]); self.assertFalse(result["errors"])
        self.assertTrue(result["all_outputs_decoded_and_frame_checked"])
        self.assertEqual(result["movies"]["story/tiny.mp4"]["timing_metrics"]["output_fps"], "30")
        self.assertEqual(self.input_state(), before)

    def test_real_corrupt_decoder_failure_is_not_a_legacy_warning(self):
        self.kind = "historical-unrecorded"; self.build()
        path = self.snapshot/"video_hd/story/tiny.mp4"
        path.write_bytes(path.read_bytes()[:len(path.read_bytes())//2])
        record = {"bytes": path.stat().st_size, "sha256": sha(path.read_bytes())}
        self.manifest["payload"]["video_hd/story/tiny.mp4"] = record
        self.manifest["movies"]["story/tiny.mp4"].update(record); self.seal()
        result = check.validate(self.config, execute=True)
        self.assertEqual(result["status"], "failed"); self.assertFalse(result["warnings"])

    def test_rehashed_png_cannot_disagree_with_original_pass_or_reviewed_proof(self):
        path = self.snapshot/("textures/replace/"+self.names[1]); path.write_bytes(b"foreign PNG")
        self.manifest["payload"][path.relative_to(self.snapshot).as_posix()] = {"bytes": path.stat().st_size, "sha256": sha(path.read_bytes())}
        self.seal(); result = check.validate(self.config, execute=True)
        self.assertIn("Frozen PNG differs", result["errors"]["global"])

    def test_recorded_metadata_mismatch_is_fatal_not_downgraded(self):
        self.manifest["movies"]["story/tiny.mp4"]["recorded_metadata"]["origin"] = "invented"
        self.seal(); result = check.validate(self.config, execute=True)
        self.assertIn("recorded metadata", result["errors"]["story/tiny.mp4"])

    def test_foreign_report_directory_and_unlisted_file_are_preserved(self):
        self.config.report.parent.mkdir(); foreign = self.config.report.parent/"foreign.txt"; foreign.write_text("keep")
        with self.assertRaises(check.ValidationError): check.validate(self.config, execute=True)
        self.assertEqual(foreign.read_text(), "keep")
        self.config.report = self.root/"another-report/result.json"
        (self.snapshot/"unlisted.txt").write_text("foreign evidence")
        result = check.validate(self.config, execute=True)
        self.assertIn("Unlisted/missing", result["errors"]["global"])

    def test_probe_command_is_cpu_bounded_and_decoder_stderr_rejects(self):
        with mock.patch.object(check.subprocess, "run", return_value=type("Result", (), {"stderr": "decode error", "stdout": "{}"})()) as run:
            api, ff, _ = check.video_api()
            with self.assertRaisesRegex(RuntimeError, "decode errors"): api.probe(ff, self.snapshot/"video_hd/story/tiny.mp4", count_frames=True)
            command = run.call_args.args[0]
            self.assertEqual(command[1:3], ["-threads", "2"])
            self.assertNotIn("-hwaccel", command)

    def test_frozen_ledger_disagrees_with_proof_even_after_rehash(self):
        path = self.snapshot/"evidence/texture-records/reviewed_fixes.json"
        path.write_bytes(encoded({"format": "foreign"}))
        self.manifest["ledger_sha256"] = sha(path.read_bytes()); self.seal()
        result = check.validate(self.config, execute=True)
        self.assertIn("Frozen ledger differs", result["errors"]["global"])

    def test_input_mutation_during_decode_prevents_completion(self):
        original = self.api.probe
        api, ff, decoder = check.video_api()
        def mutate(*args, **kwargs):
            value = original(*args, **kwargs)
            path = self.snapshot/("textures/replace/"+self.names[1]); path.write_bytes(b"changed during decode")
            return value
        api.probe = mutate
        with mock.patch.object(check, "video_api", return_value=(api, ff, decoder)):
            result = check.validate(self.config, execute=True)
        self.assertFalse(result["complete"]); self.assertIn("Frozen inventory mismatch", result["errors"]["global"])
        self.assertIsNone(result["inputs_modified"])

    def test_interruption_retains_incomplete_report_and_append_progress(self):
        api, ff, decoder = check.video_api()
        api.probe = mock.Mock(side_effect=KeyboardInterrupt)
        with mock.patch.object(check, "video_api", return_value=(api, ff, decoder)):
            result = check.validate(self.config, execute=True)
        self.assertEqual(result["status"], "interrupted"); self.assertFalse(result["complete"])
        self.assertIsNone(result["inputs_modified"])
        self.assertTrue(self.config.report.exists())
        self.assertIn('"phase": "started"', (self.config.report.parent/"progress.jsonl").read_text())

    def test_historical_classification_cannot_ignore_frozen_marker(self):
        self.kind, self.marker = "historical-marker-only", b"historical model"; self.build()
        self.manifest["movies"]["story/tiny.mp4"]["classification"] = "historical-unrecorded"
        self.manifest["movie_cohort_counts"] = {"historical-unrecorded": 1}; self.seal()
        result = check.validate(self.config, execute=True)
        self.assertIn("Historical classification disagrees", result["errors"]["story/tiny.mp4"])

    def test_strict_reuse_requires_frozen_transitive_record(self):
        row = self.manifest["movies"]["story/tiny.mp4"]
        row["recorded_metadata"]["identical_source_reuse"] = {"method": "byte-identical-original-and-exact-picture-recipe-v1",
            "original_movie": "missing.bik", "original_output_sha256": row["sha256"],
            "original_source_sha256": self.manifest["all_copied_sources"][row["source_bink"]]["sha256"],
            "original_record_sha256": "a"*64}
        relative = row["anchors"]["json"]["path"]
        (self.snapshot/relative).write_bytes(encoded(row["recorded_metadata"]))
        row["anchors"]["json"]["sha256"] = sha((self.snapshot/relative).read_bytes()); self.seal()
        result = check.validate(self.config, execute=True)
        self.assertIn("Strict identical-source proof", result["errors"]["story/tiny.mp4"])

    def test_original_bink_directory_and_basename_case_are_preserved(self):
        old = self.snapshot/"evidence/source-video/story"
        temporary, renamed = old.parent/"case-change-fixture", old.parent/"Story"
        self.assertEqual(old.resolve().parent, temporary.absolute().parent)
        self.assertEqual(old.resolve().parent, renamed.absolute().parent)
        old.rename(temporary); temporary.rename(renamed)
        (renamed/"tiny.bik").rename(renamed/"Tiny.BIK")
        old_relative, new_relative = "evidence/source-video/story/tiny.bik", "evidence/source-video/Story/Tiny.BIK"
        self.manifest["all_copied_sources"][new_relative] = self.manifest["all_copied_sources"].pop(old_relative)
        self.manifest["movies"]["story/tiny.mp4"]["source_bink"] = new_relative
        self.seal(); result = check.validate(self.config, execute=True)
        self.assertTrue(result["complete"], result)

    def test_truncated_png_fails_after_consistent_hash_repinning(self):
        self.unchanged = self.unchanged[:len(self.unchanged)//2]
        self.build(); result = check.validate(self.config, execute=True)
        self.assertEqual(result["status"], "failed")
        self.assertIn("PNG integrity failure", result["errors"]["global"])

    def test_png_format_and_dimension_bounds_are_enforced(self):
        jpeg = io.BytesIO(); Image.new("RGB", (2, 2)).save(jpeg, format="JPEG")
        for number, data in enumerate((jpeg.getvalue(), png((0, 0, 0, 255), size=(2049, 1)))):
            with self.subTest(number=number):
                self.unchanged = data; self.build(); self.config.report = self.root/("png-report-"+str(number))/"result.json"
                result = check.validate(self.config, execute=True)
                self.assertIn("PNG format/dimensions", result["errors"]["global"])

    def test_rehashed_model_evidence_cannot_disagree_with_manifest(self):
        (self.snapshot/"evidence/current-model-identity.json").write_bytes(encoded({"name": "foreign"}))
        self.seal(); result = check.validate(self.config, execute=True)
        self.assertIn("Frozen model identity differs", result["errors"]["global"])

    def add_alias(self, *, historical_parent=False):
        parent = self.manifest["movies"]["story/tiny.mp4"]
        source_rel = "evidence/source-video/tutorial/alias.bik"
        output_rel = "video_hd/tutorial/alias.mp4"
        original = self.manifest["all_copied_sources"][parent["source_bink"]]
        source = self.put(source_rel, (self.snapshot/parent["source_bink"]).read_bytes()); source["header"] = original["header"]
        output = self.put(output_rel, (self.snapshot/"video_hd/story/tiny.mp4").read_bytes())
        saved = json.loads(json.dumps(parent["recorded_metadata"]))
        saved["identical_source_reuse"] = {"method": "byte-identical-original-and-exact-picture-recipe-v1", "original_movie": "story/tiny.bik",
            "original_output_sha256": parent["sha256"], "original_source_sha256": original["sha256"],
            "original_record_sha256": parent["anchors"]["json"]["sha256"]}
        relative = "evidence/video-records/tutorial/alias.upscale.json"
        record = self.put(relative, encoded(saved))
        row = output | {"classification": "strict-original-record", "strict_recorded_provenance": True,
            "source_bink": source_rel, "recorded_metadata": saved,
            "anchors": {"json": {"present": True, "path": relative, "sha256": record["sha256"]},
                        "marker": {"present": False, "path": None, "sha256": None}}}
        self.manifest["movies"]["tutorial/alias.mp4"] = row
        self.manifest["all_copied_sources"][source_rel] = source
        self.manifest["payload"][output_rel] = output
        self.manifest.update(movie_count=2, source_bink_count=2, payload_files=5, movie_cohort_counts={"strict-original-record": 2})
        self.config.movies = 2
        if historical_parent:
            parent["classification"] = "historical-json-unverified"; parent["strict_recorded_provenance"] = False
            parent["recorded_metadata"]["origin"] = "validated-legacy-model-marker"
            relative = parent["anchors"]["json"]["path"]
            (self.snapshot/relative).write_bytes(encoded(parent["recorded_metadata"]))
            parent["anchors"]["json"]["sha256"] = sha((self.snapshot/relative).read_bytes())
            saved["identical_source_reuse"]["original_record_sha256"] = parent["anchors"]["json"]["sha256"]
            relative = row["anchors"]["json"]["path"]
            (self.snapshot/relative).write_bytes(encoded(saved))
            row["anchors"]["json"]["sha256"] = sha((self.snapshot/relative).read_bytes())
            self.manifest["movie_cohort_counts"] = {"strict-original-record": 1, "historical-json-unverified": 1}
        self.seal()

    def test_real_identical_output_alias_closes_through_strict_parent(self):
        self.add_alias()
        result = check.validate(self.config, execute=True)
        self.assertTrue(result["complete"], result)
        self.assertEqual(len(result["movies"]), 2)

    def test_historical_parent_cannot_confer_strict_alias_provenance(self):
        self.add_alias(historical_parent=True)
        result = check.validate(self.config, execute=True)
        self.assertEqual(result["status"], "failed")
        self.assertIn("different strict-origin parent", result["errors"]["tutorial/alias.mp4"])

    def test_alias_cycle_guard_rejects_revisited_actor(self):
        with self.assertRaisesRegex(check.ValidationError, "Circular identical-source"):
            check.movie_evidence(self.config, self.manifest, "story/tiny.mp4", self.api, {"story/tiny.mp4"})


if __name__ == "__main__": unittest.main()
