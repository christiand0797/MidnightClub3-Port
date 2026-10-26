"""Authenticated identical-original reuse fixtures; no GPU, real movie worker or game."""
import contextlib
import copy
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import upscale_movies as video


class IdenticalMoviesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.assets, self.output = self.root/"assets", self.root/"out"
        self.assets.mkdir(); self.output.mkdir()
        self.first, self.second = self.assets/"first.bik", self.assets/"second.bik"
        data = struct.pack("<4s8I", b"BIKi", 100, 3, 50, 3, 4, 4, 2997, 100)
        self.first.write_bytes(data); self.second.write_bytes(data)
        self.original, self.target = self.output/"first.mp4", self.output/"second.mp4"
        self.original.write_bytes(b"strict original inferred picture")
        self.target.write_bytes(b"old published picture")
        self.target.with_suffix(".upscale.json").write_bytes(b"old provenance")
        self.target.with_suffix(".model.txt").write_bytes(b"old model")
        self.source = {"width": 4, "height": 4, "fps": "2997/100", "frames": 3,
                       "codec": "binkvideo", "pix_fmt": "yuv420p"}
        self.identity = {"name": "realesrgan-x4plus", "weights": {"fixture.bin": "known"},
                         "executable_sha256": "known", "inference_scale": 4, "unsharp": .25}
        self.recipe = video.recipe_for(self.first, self.source, copy.deepcopy(self.identity))
        self.write_original_record()
        self.before = {p.name: p.read_bytes() for p in self.output.iterdir() if p.name.startswith("second")}
        stack = contextlib.ExitStack(); self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(video, "ASSETS", self.assets))
        stack.enter_context(mock.patch.object(video, "OUT", self.output))
        stack.enter_context(mock.patch.object(video, "model_identity", side_effect=lambda _model: copy.deepcopy(self.identity)))
        self.real_validate, self.real_run = video.validate_movie, video.subprocess.run
        self.validate = stack.enter_context(mock.patch.object(video, "validate_movie", return_value={
            "frames": 3, "timing": {"exact_source_timing": True}}))
        self.inference = stack.enter_context(mock.patch.object(video, "upscale_movie", side_effect=AssertionError("unexpected inference")))
        self.process = stack.enter_context(mock.patch.object(video.subprocess, "run", side_effect=AssertionError("unexpected subprocess")))

    def write_original_record(self, **changes):
        value = {"recipe": self.recipe, "output_sha256": video.sha256(self.original),
                 "origin": "original-bink", "historical_source_and_weights_verified": True}
        value.update(changes)
        self.original.with_suffix(".upscale.json").write_text(json.dumps(value), encoding="utf-8")

    def reuse(self, **kwargs):
        return video.reuse_identical_movie(self.root, self.second, self.target, self.source, self.recipe,
                                           (self.first, self.original), **kwargs)

    def unchanged_target(self):
        for name, value in self.before.items(): self.assertEqual((self.output/name).read_bytes(), value)
        self.assertFalse(list(self.output.glob(".*.part.mp4")))
        self.process.assert_not_called(); self.inference.assert_not_called()

    def test_independent_copy_has_exact_recipe_and_explicit_proof_without_inference(self):
        evidence_sha = video.sha256(self.original.with_suffix(".upscale.json"))
        self.assertEqual(self.reuse(), 3)
        self.assertEqual(self.target.read_bytes(), self.original.read_bytes())
        self.assertFalse(os.path.samefile(self.target, self.original))
        self.assertEqual(self.target.stat().st_nlink, 1)
        record = json.loads(self.target.with_suffix(".upscale.json").read_text())
        self.assertEqual(record["recipe"], self.recipe)
        self.assertTrue(video.original_provenance(record))
        proof = record["identical_source_reuse"]
        self.assertEqual(proof["original_movie"], "first.bik")
        self.assertEqual(proof["original_record_sha256"], evidence_sha)
        self.assertEqual(proof["original_source_sha256"], video.sha256(self.second))
        self.assertEqual(proof["original_output_sha256"], video.sha256(self.target))
        self.process.assert_not_called(); self.inference.assert_not_called()

    def test_different_original_bytes_are_never_reused(self):
        self.second.write_bytes(self.second.read_bytes()+b"different")
        with self.assertRaisesRegex(ValueError, "original bytes changed"): self.reuse()
        self.unchanged_target()

    def test_legacy_or_different_picture_recipe_is_never_adopted(self):
        for changes in ({"origin": "validated-legacy-model-marker"},
                        {"historical_source_and_weights_verified": False},
                        {"recipe": dict(self.recipe, output_scale=99)}):
            with self.subTest(changes=changes):
                self.write_original_record(**changes)
                with self.assertRaisesRegex(ValueError, "exact original/model/picture"): self.reuse()
                self.unchanged_target()

    def test_changed_weights_or_original_output_is_rejected(self):
        self.identity["weights"]["fixture.bin"] = "changed"
        with self.assertRaisesRegex(ValueError, "weights/executable changed"): self.reuse()
        self.identity["weights"]["fixture.bin"] = "known"
        self.original.write_bytes(b"changed output")
        with self.assertRaisesRegex(ValueError, "picture/proof changed"): self.reuse()
        self.unchanged_target()

    def test_full_decode_failure_preserves_published_movie(self):
        self.validate.side_effect = ValueError("bad complete timeline")
        with self.assertRaisesRegex(ValueError, "timeline"): self.reuse()
        self.unchanged_target()

    def test_prepared_copy_or_source_record_changed_during_validation_is_rejected(self):
        for mutate in (lambda part: part.write_bytes(b"modified temporary"),
                       lambda _part: self.original.with_suffix(".upscale.json").write_text("changed proof")):
            with self.subTest(mutate=mutate):
                self.write_original_record()
                def validate(_ff, part, _source, _frames):
                    mutate(part)
                    return {"frames": 3}
                self.validate.side_effect = validate
                with self.assertRaises(ValueError): self.reuse()
                self.unchanged_target()

    def test_explicit_stop_wins_over_pause_and_preserves_both_markers(self):
        pause, stop = self.root/"pause", self.root/"stop"
        pause.write_text("pause"); stop.write_text("stop")
        with self.assertRaises(video.MovieStopped): self.reuse(pause_file=pause, stop_file=stop)
        self.assertEqual(pause.read_text(), "pause"); self.assertEqual(stop.read_text(), "stop")
        self.unchanged_target()

    def test_pause_during_validation_rechecks_proof_after_resume(self):
        pause = self.root/"pause"
        def validate(*_args):
            pause.write_text("pause")
            return {"frames": 3}
        def resume(_seconds):
            self.original.with_suffix(".upscale.json").write_text("changed while paused")
            pause.unlink()
        self.validate.side_effect = validate
        with mock.patch.object(video.time, "sleep", side_effect=resume):
            with self.assertRaisesRegex(ValueError, "picture/proof changed"): self.reuse(pause_file=pause)
        self.unchanged_target()

    def test_replacing_old_hardlinked_destination_does_not_modify_original(self):
        self.target.unlink(); os.link(self.original, self.target)
        original = self.original.read_bytes()
        self.assertEqual(self.reuse(), 3)
        self.assertEqual(self.original.read_bytes(), original)
        self.assertFalse(os.path.samefile(self.target, self.original))
        self.assertEqual(self.target.stat().st_nlink, 1)

    def test_destination_edit_during_validation_is_preserved(self):
        def validate(*_args):
            self.target.write_bytes(b"new external edit")
            return {"frames": 3}
        self.validate.side_effect = validate
        with self.assertRaisesRegex(ValueError, "destination changed"): self.reuse()
        self.assertEqual(self.target.read_bytes(), b"new external edit")
        self.assertEqual(self.target.with_suffix(".upscale.json").read_bytes(), self.before["second.upscale.json"])
        self.assertFalse(list(self.output.glob(".*.part.mp4")))

    def test_real_small_cpu_stream_copy_passes_full_frame_and_exact_timeline_validation(self):
        ff = video.ffmpeg_bin()
        self.real_run([video.binary(ff, "ffmpeg"), "-v", "error", "-y", "-f", "lavfi", "-i",
            "color=c=white:s=12x12:r=2997/100", "-frames:v", "3", "-an", "-c:v", "libx264",
            "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p", "-enc_time_base", "100:2997",
            "-video_track_timescale", "2997", "-movflags", "+faststart", str(self.original)], check=True)
        self.write_original_record()
        with (mock.patch.object(video.subprocess, "run", side_effect=self.real_run),
              mock.patch.object(video, "validate_movie", side_effect=self.real_validate)):
            self.assertEqual(video.reuse_identical_movie(ff, self.second, self.target, self.source, self.recipe,
                                                         (self.first, self.original)), 3)
        value = json.loads(self.target.with_suffix(".upscale.json").read_text())
        self.assertTrue(value["output"]["timing"]["exact_source_timing"])
        self.assertEqual(value["output"]["frames"], 3)
        self.assertEqual(video.sha256(self.target), video.sha256(self.original))
        self.inference.assert_not_called()

    def cli(self, *extra):
        report = self.root/"report.json"
        with (mock.patch.object(video.sys, "argv", ["upscale_movies.py", "--require-original-provenance",
              "--report", str(report), "--stop-file", str(self.root/"stop"), *extra]),
              mock.patch.object(video, "ffmpeg_bin", return_value=self.root),
              mock.patch.object(video, "movie_paths", return_value=[self.first, self.second]),
              mock.patch.object(video, "probe", return_value=self.source),
              mock.patch.object(video, "worker_lock", return_value=contextlib.nullcontext())):
            result = video.main()
        return result, json.loads(report.read_text())

    def test_cli_reuses_only_a_previous_strict_exact_recipe_and_counts_complete_output(self):
        status, report = self.cli()
        self.assertEqual(status, 0); self.assertEqual(report["verified"], 2)
        self.assertTrue(report["all_original_source_and_weights_verified"])
        self.assertIn("identical_source_reuse", json.loads(self.target.with_suffix(".upscale.json").read_text()))
        self.process.assert_not_called(); self.inference.assert_not_called()

    def test_force_explicitly_bypasses_reuse(self):
        def fresh(_ff, src, dst, _model, source, recipe, **_kwargs):
            dst.write_bytes(b"new force result")
            dst.with_suffix(".upscale.json").write_text(json.dumps({"recipe": recipe,
                "output_sha256": video.sha256(dst), "origin": "original-bink",
                "historical_source_and_weights_verified": True}))
            return source["frames"]
        self.inference.side_effect = fresh
        with mock.patch.object(video, "reuse_identical_movie", side_effect=AssertionError("force must infer")):
            status, report = self.cli("--force")
        self.assertEqual(status, 0); self.assertEqual(report["verified"], 2)
        self.assertEqual(self.inference.call_count, 2)


if __name__ == "__main__": unittest.main()
