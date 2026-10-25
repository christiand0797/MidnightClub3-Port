"""Cooperative movie scheduling tests: fake decode/inference/encoder, no GPU or real child process."""
import contextlib
import copy
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from PIL import Image

import test_upscale_movies as existing

video = existing.video


class FakeInput:
    def __init__(self, encoder):
        self.encoder, self.closed = encoder, False
    def write(self, data):
        if self.closed:
            raise RuntimeError("write to closed fake encoder")
        self.encoder.frames.append(data[0])
        return len(data)
    def close(self): self.closed = True


class FakeEncoder:
    def __init__(self, part):
        self.part, self.frames, self.returncode, self.terminations = Path(part), [], None, 0
        self.stdin = FakeInput(self)
    def poll(self): return self.returncode
    def wait(self, timeout=None):
        if self.returncode is None:
            if not self.stdin.closed:
                raise RuntimeError("fake wait before input close")
            self.part.write_bytes(bytes(self.frames)+b"complete fake encode")
            self.returncode = 0
        return self.returncode
    def terminate(self):
        self.terminations += 1
        self.returncode = -15
    def kill(self): self.returncode = -9


class MoviePauseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.src, self.dst = self.folder/"source.bik", self.folder/"published.mp4"
        self.src.write_bytes(b"original source fixture")
        self.dst.write_bytes(b"previous complete published movie")
        self.dst.with_suffix(".upscale.json").write_bytes(b"previous provenance")
        self.dst.with_suffix(".model.txt").write_bytes(b"previous model")
        self.before = {path.name: path.read_bytes() for path in self.folder.iterdir() if path.is_file()}
        self.pause, self.stop = self.folder/"pause", self.folder/"stop"
        self.source = {"width": 4, "height": 4, "fps": "30", "frames": 19, "codec": "binkvideo", "pix_fmt": "yuv420p"}
        self.identity = {"name": "realesrgan-x4plus", "weights": {"fixture.bin": "pinned"}, "executable_sha256": "fixed",
                         "inference_scale": 4, "unsharp": .25}
        self.recipe = copy.deepcopy(video.recipe_for(self.src, self.source, self.identity))
        self.encoders, self.batches, self.decode_calls, self.temp_roots = [], [], 0, []
        self.after_decode = self.after_inference = None
        self.real_temp = tempfile.TemporaryDirectory
        stack = contextlib.ExitStack(); self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(video.subprocess, "run", side_effect=self.fake_run))
        stack.enter_context(mock.patch.object(video.subprocess, "Popen", side_effect=self.fake_encoder))
        stack.enter_context(mock.patch.object(video, "validate_movie", return_value={"frames": 19, "timing": {"exact_source_timing": True}}))
        self.identity_mock = stack.enter_context(mock.patch.object(video, "model_identity", side_effect=lambda _model: copy.deepcopy(self.identity)))
        self.sleep = stack.enter_context(mock.patch.object(video.time, "sleep", side_effect=AssertionError("unexpected pause wait")))
        stack.enter_context(mock.patch.object(video.tempfile, "TemporaryDirectory", side_effect=self.fake_temp))
        stack.enter_context(mock.patch.object(video, "QUARANTINE", self.folder/"quarantine"))

    def fake_temp(self, **kwargs):
        temp = self.real_temp(dir=self.folder, **kwargs)
        self.temp_roots.append(Path(temp.name))
        return temp

    def fake_encoder(self, command, **kwargs):
        encoder = FakeEncoder(command[-1]); self.encoders.append(encoder)
        self.encoder_command = command
        return encoder

    def fake_run(self, command, **kwargs):
        if Path(command[0]).name == "realesrgan-ncnn-vulkan.exe":
            cin, cout = Path(command[command.index("-i")+1]), Path(command[command.index("-o")+1])
            frames = sorted(cin.glob("*.png")); self.batches.append([frame.name for frame in frames])
            for frame in frames:
                with Image.open(frame) as image:
                    image.resize((16, 16), Image.Resampling.NEAREST).save(cout/frame.name)
            if self.after_inference: self.after_inference()
        else:
            self.decode_calls += 1
            destination = Path(command[-1]).parent
            for index in range(self.source["frames"]):
                Image.new("RGB", (4, 4), (index, index, index)).save(destination/f"f{index+1:06}.png")
            if self.after_decode: self.after_decode()
        return subprocess.CompletedProcess(command, 0, b"", b"")

    def run_movie(self, **kwargs):
        return video.upscale_movie(self.folder, self.src, self.dst, "realesrgan-x4plus", self.source, self.recipe, **kwargs)

    def unchanged(self):
        for name, data in self.before.items(): self.assertEqual((self.folder/name).read_bytes(), data)
        self.assertFalse(list(self.folder.glob(".*.part.mp4")))
        self.assertFalse(any(path.exists() for path in self.temp_roots))

    def test_pause_resume_holds_one_encoder_and_original_frames_in_exact_order(self):
        def pause_after_first():
            if len(self.batches) == 1: self.pause.write_text("owned game pause")
        self.after_inference = pause_after_first
        def resume(_seconds):
            self.assertEqual(len(self.encoders), 1)
            self.assertFalse(self.encoders[0].stdin.closed)
            self.assertIsNone(self.encoders[0].poll())
            self.assertEqual(len(self.batches), 1)
            tmp = self.temp_roots[0]
            self.assertEqual(len(list((tmp/"lo").glob("*.png")))+len(list((tmp/"cin").glob("*.png"))), 19)
            self.pause.unlink()  # Simulate only the coordinator removing its test-owned marker.
        self.sleep.side_effect = resume
        self.assertEqual(self.run_movie(pause_file=self.pause, stop_file=self.stop), 19)
        self.assertEqual([len(batch) for batch in self.batches], [8, 8, 3])
        self.assertEqual(self.encoders[0].frames, list(range(19)))
        self.assertEqual(self.decode_calls, 1)
        self.assertEqual(len(self.encoders), 1)
        self.assertEqual(self.encoders[0].terminations, 0)
        self.assertEqual(json.loads(self.dst.with_suffix(".upscale.json").read_text())["recipe"], self.recipe)

    def test_pause_before_decode_and_during_decode_does_not_start_gpu_or_early_encoder(self):
        self.pause.write_text("pause before decode")
        self.after_decode = lambda: self.pause.write_text("pause before encoder")
        waits = []
        def resume(_seconds):
            waits.append(self.decode_calls)
            self.assertFalse(self.encoders)
            self.assertFalse(self.batches)
            self.pause.unlink()
        self.sleep.side_effect = resume
        self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.assertEqual(waits, [0, 1])
        self.assertEqual(self.encoders[0].frames, list(range(19)))

    def test_stop_while_paused_cleans_only_owned_inflight_encoder_and_preserves_markers(self):
        self.after_inference = lambda: self.pause.write_text("foreign or owned pause is still presence")
        self.sleep.side_effect = lambda _seconds: self.stop.write_text("explicit user stop")
        with self.assertRaises(video.MovieStopped):
            self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.assertEqual(len(self.batches), 1)
        self.assertEqual(self.encoders[0].terminations, 1)
        self.assertTrue(self.encoders[0].stdin.closed)
        self.assertTrue(self.pause.exists()); self.assertTrue(self.stop.exists())
        self.unchanged()

    def test_stop_presence_wins_without_starting_decode_encoder_or_owner_check(self):
        self.pause.touch(); self.stop.touch()
        owner = mock.Mock()
        with self.assertRaises(video.MovieStopped):
            self.run_movie(pause_file=self.pause, stop_file=self.stop, pause_owner=owner)
        owner.check.assert_not_called()
        self.assertEqual(self.decode_calls, 0)
        self.assertFalse(self.encoders); self.assertFalse(self.batches)
        self.unchanged()

    def test_stop_after_final_inference_or_completed_validation_never_publishes(self):
        self.source["frames"] = 3
        self.after_inference = lambda: self.stop.touch()
        with self.assertRaises(video.MovieStopped): self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.unchanged()
        self.stop.unlink(); self.after_inference = None
        with mock.patch.object(video, "validate_movie", side_effect=lambda *_args: self.stop.touch() or {}):
            with self.assertRaises(video.MovieStopped): self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.unchanged()

    def test_owner_exit_during_pause_leaves_private_marker_and_old_movie(self):
        alive = [True]
        owner = mock.Mock()
        def check():
            if not alive[0]: raise video.MovieStopped("owner ended")
        owner.check.side_effect = check
        self.after_inference = lambda: self.pause.write_text("private orphan marker")
        self.sleep.side_effect = lambda _seconds: alive.__setitem__(0, False)
        with self.assertRaises(video.MovieStopped):
            self.run_movie(pause_file=self.pause, stop_file=self.stop, pause_owner=owner)
        self.assertEqual(len(self.batches), 1)
        self.assertTrue(self.pause.exists())
        self.unchanged()

    def test_changed_weights_during_pause_cannot_receive_old_recipe(self):
        self.after_inference = lambda: self.pause.touch() if len(self.batches) == 1 else None
        def change_and_resume(_seconds):
            self.identity["weights"] = {"fixture.bin": "changed"}
            self.pause.unlink()
        self.sleep.side_effect = change_and_resume
        with self.assertRaisesRegex(ValueError, "weights/executable changed"):
            self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.assertEqual(len(self.batches), 1)
        self.unchanged()

    def test_pause_arriving_during_identity_hash_rechecks_after_resume_before_first_gpu(self):
        checks = []
        def identity(_model):
            checks.append(True)
            if len(checks) == 1: self.pause.touch()
            return copy.deepcopy(self.identity)
        self.identity_mock.side_effect = identity
        def change_and_resume(_seconds):
            self.identity["weights"] = {"fixture.bin": "changed during hash pause"}
            self.pause.unlink()
        self.sleep.side_effect = change_and_resume
        with self.assertRaisesRegex(ValueError, "weights/executable changed"):
            self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.assertFalse(self.batches)
        self.unchanged()

    def test_publication_retry_rechecks_inputs_after_pause(self):
        self.source["frames"] = 3
        real_replace = video.os.replace
        attempts = []
        def replace(source, destination):
            if Path(destination) == self.dst and not attempts:
                attempts.append(True)
                self.pause.touch()
                raise PermissionError("fixture picture is still held")
            return real_replace(source, destination)
        def change_and_resume(_seconds):
            self.identity["executable_sha256"] = "changed while publication waited"
            self.pause.unlink()
        self.sleep.side_effect = change_and_resume
        with mock.patch.object(video.os, "replace", side_effect=replace):
            with self.assertRaisesRegex(ValueError, "weights/executable changed"):
                self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.assertEqual(len(attempts), 1)
        self.unchanged()

    def test_default_chunk_and_picture_recipe_are_unchanged_without_pause_option(self):
        original_recipe = copy.deepcopy(self.recipe)
        self.run_movie()
        self.assertEqual(video.CHUNK, 400); self.assertEqual(video.PIPELINE_VERSION, 1)
        self.assertEqual([len(batch) for batch in self.batches], [19])
        self.identity_mock.assert_not_called()
        self.assertEqual(json.loads(self.dst.with_suffix(".upscale.json").read_text())["recipe"], original_recipe)
        self.assertEqual(self.encoder_command[self.encoder_command.index("-preset")+1], "slow")
        self.assertEqual(self.encoder_command[self.encoder_command.index("-crf")+1], "16")

    def test_dangling_pause_marker_is_presence_and_explicit_stop_wins(self):
        try:
            self.pause.symlink_to(self.folder/"missing-marker-target")
        except OSError as error:
            self.skipTest("symlink creation unavailable: "+str(error))
        self.assertFalse(self.pause.exists()); self.assertTrue(os.path.lexists(self.pause))
        self.sleep.side_effect = lambda _seconds: self.stop.touch()
        with self.assertRaises(video.MovieStopped): self.run_movie(pause_file=self.pause, stop_file=self.stop)
        self.assertTrue(self.pause.is_symlink())
        self.assertEqual(self.decode_calls, 0); self.assertFalse(self.batches)
        self.unchanged()

    def test_cli_cooperative_stop_returns_pending_report_exit_two_not_failure(self):
        report = self.folder/"progress.json"
        header = bytearray(36); header[:4] = b"BIKi"
        import struct
        for offset, value in ((8,19), (20,4), (24,4), (28,30), (32,1)): struct.pack_into("<I", header, offset, value)
        self.src.write_bytes(header)
        with (mock.patch.object(video.sys, "argv", ["upscale_movies.py", "--pause-file", str(self.pause), "--stop-file", str(self.stop), "--report", str(report)]),
              mock.patch.object(video, "ffmpeg_bin", return_value=self.folder), mock.patch.object(video, "movie_paths", return_value=[self.src]),
              mock.patch.object(video, "ASSETS", self.folder), mock.patch.object(video, "OUT", self.folder/"out"),
              mock.patch.object(video, "worker_lock", return_value=contextlib.nullcontext()),
              mock.patch.object(video, "probe", return_value=self.source), mock.patch.object(video, "current_output", return_value=False),
              mock.patch.object(video, "upscale_movie", side_effect=video.MovieStopped("fixture user cancellation")) as movie):
            self.assertEqual(video.main(), 2)
        value = json.loads(report.read_text())
        self.assertEqual(value["failed"], 0); self.assertEqual(value["details"][0]["status"], "pending")
        self.assertTrue(value["details"][0]["restart_inflight_on_resume"])
        self.assertEqual(movie.call_args.kwargs["pause_file"], self.pause)


@unittest.skipUnless(os.name == "nt", "Windows read-only process handle contract")
class PauseOwnerTests(unittest.TestCase):
    def kernel(self, wait):
        api = mock.Mock()
        api.OpenProcess.return_value = 0x123456789ABC
        api.WaitForSingleObject.side_effect = wait
        api.CloseHandle.return_value = True
        return api

    def test_pointer_width_handle_is_pinned_read_only_and_owner_death_fails_closed(self):
        api = self.kernel([0x102, 0])
        with mock.patch.object(video.os, "getppid", return_value=77), mock.patch.object(video.ctypes, "WinDLL", return_value=api):
            owner = video.PauseOwner(77)
            self.assertEqual(owner.handle, 0x123456789ABC)
            self.assertEqual(api.OpenProcess.restype, wintypes.HANDLE)
            api.OpenProcess.assert_called_once_with(0x00100000, False, 77)
            with self.assertRaises(video.MovieStopped): owner.check()
            owner.close()
            api.CloseHandle.assert_called_once_with(0x123456789ABC)

    def test_missing_parent_wrong_pid_or_uncertain_wait_never_authorizes_inference(self):
        for wait in (0, 0xFFFFFFFF):
            api = self.kernel([wait])
            with self.subTest(wait=wait), mock.patch.object(video.os, "getppid", return_value=77), \
                    mock.patch.object(video.ctypes, "WinDLL", return_value=api):
                with self.assertRaises(video.MovieStopped): video.PauseOwner(77)
                api.CloseHandle.assert_called_once()
        api = self.kernel([]); api.OpenProcess.return_value = 0
        with mock.patch.object(video.os, "getppid", return_value=77), mock.patch.object(video.ctypes, "WinDLL", return_value=api):
            with self.assertRaises(video.MovieStopped): video.PauseOwner(77)
            with self.assertRaises(video.MovieStopped): video.PauseOwner(78)


if __name__ == "__main__": unittest.main()
