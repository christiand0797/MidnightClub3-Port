"""Publication, resume and timing regressions; the integration test uses no GPU."""
import importlib.util
import json
import subprocess
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from PIL import Image

_spec = importlib.util.spec_from_file_location("mc3_upscale_movies", Path(__file__).with_name("upscale_movies.py"))
video = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(video)


class ValidationTests(unittest.TestCase):
    def setUp(self):
        self.source = {"width": 640, "height": 480, "fps": "2997/100", "frames": 30}
        self.hd = {"width": 1920, "height": 1440, "fps": "2997/100", "frames": 30,
                   "codec": "h264", "pix_fmt": "yuv420p"}

    def test_timing_and_native_decode_contract(self):
        video.validate_info(self.hd, self.source, 30)
        for change in ({"width": 2560}, {"frames": 29}, {"fps": "30"}, {"codec": "hevc"}, {"pix_fmt": "yuv444p"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                video.validate_info(dict(self.hd, **change), self.source, 30)

    def test_probe_rejects_decode_error_even_when_process_exits_zero(self):
        result = subprocess.CompletedProcess([], 0, '{"streams": []}', "invalid NAL unit\n")
        with mock.patch.object(video.subprocess, "run", return_value=result), self.assertRaises(RuntimeError):
            video.probe(Path("ff"), Path("movie.mp4"), True)

    def test_bink_frame_count_is_header_duration_not_missing_nb_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "source.bik"
            path.write_bytes(struct.pack("<4s8I", b"BIKi", 100, 2136, 50, 2136, 640, 480, 2997, 100))
            header = video.bink_header(path)
            self.assertEqual(header, dict(self.source, frames=2136))
            stream = {"width": 640, "height": 480, "r_frame_rate": "2997/100", "codec_name": "binkvideo"}
            result = subprocess.CompletedProcess([], 0, json.dumps({"streams": [stream]}), "")
            with mock.patch.object(video.subprocess, "run", return_value=result):
                self.assertEqual(video.probe(Path("ff"), path)["frames"], 2136)
            path.write_bytes(b"BIKi")
            with self.assertRaises(ValueError):
                video.bink_header(path)

    def test_atomic_metadata_failure_keeps_previous_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "movie.upscale.json"
            target.write_text("old verified metadata", encoding="utf-8")
            with mock.patch.object(video.os, "replace", side_effect=PermissionError("busy")), self.assertRaises(PermissionError):
                video.atomic_write(target, "new metadata")
            self.assertEqual(target.read_text(), "old verified metadata")
            self.assertEqual(list(Path(folder).iterdir()), [target])

    def test_locked_published_movie_keeps_old_picture_and_candidate(self):
        with tempfile.TemporaryDirectory() as folder:
            target, part = Path(folder) / "movie.mp4", Path(folder) / "candidate.mp4"
            target.write_bytes(b"previous HD picture")
            part.write_bytes(b"validated new HD picture")
            with mock.patch.object(video.os, "replace", side_effect=PermissionError("open in game")), self.assertRaises(PermissionError):
                video.publish(part, target, attempts=2, delay=0)
            self.assertEqual(target.read_bytes(), b"previous HD picture")
            self.assertEqual(part.read_bytes(), b"validated new HD picture")

    def test_resume_requires_recipe_and_output_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "movie.mp4"
            target.write_bytes(b"completed picture")
            recipe = {"model": {"name": "test"}, "source_sha256": "original"}
            video.atomic_write(target.with_suffix(".upscale.json"), json.dumps({"recipe": recipe, "output_sha256": video.sha256(target)}))
            self.assertTrue(video.current_output(None, None, target, None, recipe))
            self.assertFalse(video.current_output(None, None, target, None, dict(recipe, source_sha256="changed")))
            target.write_bytes(b"broken picture")
            self.assertFalse(video.current_output(None, None, target, None, recipe))

    def test_legacy_marker_needs_complete_decode_before_adoption(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "movie.mp4"
            target.write_bytes(b"unverified picture")
            target.with_suffix(".model.txt").write_text("test", encoding="utf-8")
            recipe = {"model": {"name": "test"}}
            with mock.patch.object(video, "validate_movie", side_effect=ValueError("truncated")):
                self.assertFalse(video.current_output(None, None, target, self.source, recipe))
            self.assertFalse(target.with_suffix(".upscale.json").exists())
            with mock.patch.object(video, "validate_movie", return_value=self.hd):
                self.assertTrue(video.current_output(None, None, target, self.source, recipe))
            saved = json.loads(target.with_suffix(".upscale.json").read_text())
            self.assertFalse(saved["historical_source_and_weights_verified"])
            self.assertEqual(saved["origin"], "validated-legacy-model-marker")

    def test_rejects_legacy_worker_without_entering_gpu_work(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(video, "legacy_workers", return_value=[12345]):
            with self.assertRaisesRegex(RuntimeError, "12345"):
                with video.worker_lock(Path(folder)):
                    self.fail("must not enter the worker body")
            # OS lock was released after the refusal.
            with mock.patch.object(video, "legacy_workers", return_value=[]):
                with video.worker_lock(Path(folder)):
                    pass

    def test_coverage_gate_requires_every_original_movie_and_requested_model(self):
        args = SimpleNamespace(model="realesrgan-x4plus", report=None)
        rows = [{"movie": "one.bik", "frames": 10, "status": "verified"},
                {"movie": "two.bik", "frames": 20, "status": "model-pending"}]
        with mock.patch.object(video, "movie_paths", return_value=["one", "two"]):
            report = video.run_report(args, rows, "audit")
            self.assertTrue(report["covers_all_sources"])
            self.assertFalse(report["all_source_movies_validated"])
            self.assertEqual(report["remaining_frames"], 20)
            rows[1]["status"] = "verified"
            self.assertTrue(video.run_report(args, rows, "audit")["all_source_movies_validated"])
            partial = video.run_report(args, rows[:1], "audit")
            self.assertTrue(partial["all_requested_model_validated"])
            self.assertFalse(partial["all_source_movies_validated"])

    def test_stop_file_stops_new_worker_before_touching_movie(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            stop = folder / "stop"
            stop.touch()
            src = folder / "source.bik"
            src.write_bytes(struct.pack("<4s8I", b"BIKi", 100, 30, 50, 30, 640, 480, 2997, 100))
            with mock.patch.object(video.sys, "argv", ["upscale_movies.py", "--stop-file", str(stop)]), \
                 mock.patch.object(video, "ffmpeg_bin", return_value=folder), \
                 mock.patch.object(video, "movie_paths", return_value=[src]), \
                 mock.patch.object(video, "ASSETS", folder), mock.patch.object(video, "OUT", folder / "out"), \
                 mock.patch.object(video, "model_identity", return_value={}), \
                 mock.patch.object(video, "legacy_workers", return_value=[]), \
                 mock.patch.object(video, "upscale_movie") as upscale:
                self.assertEqual(video.main(), 2)
                upscale.assert_not_called()


class EncodeIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.ff = video.ffmpeg_bin()
        except RuntimeError as e:
            raise unittest.SkipTest(str(e))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.src = self.folder / "original.mp4"
        self.dst = self.folder / "published.mp4"
        subprocess.run([video.binary(self.ff, "ffmpeg"), "-v", "error", "-f", "lavfi", "-i", "testsrc=size=16x16:rate=2997/100",
                        "-frames:v", "3", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(self.src)], check=True)
        self.source = video.probe(self.ff, self.src)
        self.recipe = {"source_sha256": video.sha256(self.src), "model": {"name": "realesrgan-x4plus"}}
        self.real_run = subprocess.run

    def fake_upscaler(self, command, **kwargs):
        if Path(command[0]).name != "realesrgan-ncnn-vulkan.exe":
            return self.real_run(command, **kwargs)
        cin, cout = Path(command[command.index("-i") + 1]), Path(command[command.index("-o") + 1])
        for src in cin.glob("*.png"):
            with Image.open(src) as image:
                image.resize((64, 64)).save(cout / src.name)
        return subprocess.CompletedProcess(command, 0, b"", b"")

    def test_real_encoder_keeps_every_frame_and_exact_rate_across_chunks(self):
        with mock.patch.object(video.subprocess, "run", side_effect=self.fake_upscaler):
            count = video.upscale_movie(self.ff, self.src, self.dst, "realesrgan-x4plus", self.source, self.recipe, chunk_size=2)
        self.assertEqual(count, 3)
        info = video.probe(self.ff, self.dst, True)
        self.assertEqual((info["width"], info["height"], info["fps"], info["frames"]), (48, 48, "2997/100", 3))
        saved = json.loads(self.dst.with_suffix(".upscale.json").read_text())
        self.assertEqual(saved["output_sha256"], video.sha256(self.dst))
        self.assertEqual(saved["origin"], "original-bink")
        self.assertEqual(self.dst.with_suffix(".model.txt").read_text().strip(), "realesrgan-x4plus")
        self.assertEqual(list(self.folder.glob("*.part.mp4")), [])

    def test_failed_gpu_keeps_old_picture_and_stops_encoder(self):
        self.dst.write_bytes(b"existing picture")
        def failed(command, **kwargs):
            if Path(command[0]).name == "realesrgan-ncnn-vulkan.exe":
                return subprocess.CompletedProcess(command, 1, b"", b"GPU unavailable")
            return self.real_run(command, **kwargs)
        with mock.patch.object(video.subprocess, "run", side_effect=failed), self.assertRaisesRegex(RuntimeError, "GPU unavailable"):
            video.upscale_movie(self.ff, self.src, self.dst, "realesrgan-x4plus", self.source, self.recipe)
        self.assertEqual(self.dst.read_bytes(), b"existing picture")
        self.assertFalse(self.dst.with_suffix(".upscale.json").exists())
        self.assertEqual(list(self.folder.glob("*.part.mp4")), [])

    def test_missing_inference_frames_never_replaces_picture(self):
        self.dst.write_bytes(b"existing picture")
        def missing(command, **kwargs):
            if Path(command[0]).name == "realesrgan-ncnn-vulkan.exe":
                return subprocess.CompletedProcess(command, 0, b"", b"")
            return self.real_run(command, **kwargs)
        with mock.patch.object(video.subprocess, "run", side_effect=missing), self.assertRaisesRegex(ValueError, "missing or unexpected"):
            video.upscale_movie(self.ff, self.src, self.dst, "realesrgan-x4plus", self.source, self.recipe)
        self.assertEqual(self.dst.read_bytes(), b"existing picture")


if __name__ == "__main__":
    unittest.main()
