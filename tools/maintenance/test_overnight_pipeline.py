"""Fixture-only backup/media-gate tests. No GPU, robocopy or PBR process is started."""
import copy
from contextlib import nullcontext
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import overnight_pipeline as pipeline


def valid_reports():
    texture = {"complete": True, "model": "ultrasharp-4x", "pass_id": pipeline.PASS_ID,
               "counts": {"eligible": 2, "complete_named_pass": 2}, "issues": {"missing": []}}
    video = {"mode": "audit", "requested_model": "realesrgan-x4plus", "covers_all_sources": True,
             "movies": 106, "all_source_movies_validated": True, "all_requested_model_validated": True,
             "failed": 0, "details": [{"movie": f"movie{number}.bik", "status": "verified",
                                        "output_validated": True, "recorded_model": "realesrgan-x4plus"}
                                       for number in range(106)]}
    return texture, video


class CoordinatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.source, self.backup = self.base / "source", self.base / "backup"
        self.source.mkdir()
        self.backup.mkdir()
        (self.source / "assets").mkdir()
        (self.source / "assets/data.bin").write_bytes(b"original bytes")
        (self.source / "code.cpp").write_text("int original = 1;\n")
        self.manifest = self.base / "verification/manifest.json"

    def copy_fixture(self):
        shutil.copytree(self.source, self.backup, dirs_exist_ok=True)

    def test_dry_run_is_read_only_and_records_exact_live_exclusions(self):
        live = self.source / pipeline.LIVE
        live.mkdir(parents=True)
        (live / "state.json").write_text("changing")
        note = self.source / pipeline.LIVE_NOTE
        note.parent.mkdir(parents=True)
        note.write_text("changing note")
        before = sorted(path.relative_to(self.source).as_posix() for path in self.source.rglob("*"))
        report = pipeline.dry_run(self.source, self.backup)
        self.assertFalse(report["actions_started"])
        self.assertEqual(report["files"], 2)
        self.assertEqual({item["path"] for item in report["exclusions"]}, {pipeline.LIVE, pipeline.LIVE_NOTE})
        self.assertEqual(before, sorted(path.relative_to(self.source).as_posix() for path in self.source.rglob("*")))

    def test_media_gate_rejects_inventory_partial_wrong_model_and_duplicate_rows(self):
        texture, video = valid_reports()
        pipeline.validate_media_reports(texture, video)
        changes = [("mode", "marker-inventory"), ("movies", 105), ("failed", 1),
                   ("all_source_movies_validated", False), ("all_requested_model_validated", False)]
        for key, value in changes:
            with self.subTest(key=key):
                changed = copy.deepcopy(video)
                changed[key] = value
                with self.assertRaises(RuntimeError):
                    pipeline.validate_media_reports(texture, changed)
        changed = copy.deepcopy(video)
        changed["details"][0]["movie"] = changed["details"][1]["movie"]
        with self.assertRaises(RuntimeError):
            pipeline.validate_media_reports(texture, changed)
        changed = copy.deepcopy(video)
        changed["details"][0]["recorded_model"] = "realesr-animevideov3"
        with self.assertRaises(RuntimeError):
            pipeline.validate_media_reports(texture, changed)
        texture["counts"]["complete_named_pass"] = 1
        with self.assertRaises(RuntimeError):
            pipeline.validate_media_reports(texture, video)

    def test_copy_command_is_nondestructive_and_destination_cannot_overlap(self):
        command = pipeline.robocopy_command(self.source, self.backup, pipeline.inventory(self.source))
        for option in ("/E", "/COPY:DAT", "/DCOPY:DAT", "/XJ"):
            self.assertIn(option, command)
        for option in ("/MIR", "/PURGE", "/MOVE", "/MOV"):
            self.assertNotIn(option, command)
        for unsafe in (self.source, self.source / "inside", self.base):
            with self.assertRaises(ValueError):
                pipeline.safe_backup_destination(self.source, unsafe)

    def test_backup_hashes_every_file_then_reuses_unchanged_hashes(self):
        self.copy_fixture()
        snapshot = pipeline.inventory(self.source)
        with patch.object(pipeline, "sha256", wraps=pipeline.sha256) as digest:
            manifest = pipeline.verify_backup(self.source, self.backup, snapshot, self.manifest)
            self.assertEqual(digest.call_count, 4)  # source + backup for both files
        self.assertTrue(manifest["verified"])
        self.assertEqual(manifest["file_count"], 2)
        with patch.object(pipeline, "sha256", wraps=pipeline.sha256) as digest:
            pipeline.verify_backup(self.source, self.backup, snapshot, self.manifest)
            digest.assert_not_called()

    def test_equal_length_corruption_is_rejected(self):
        self.copy_fixture()
        (self.backup / "assets/data.bin").write_bytes(b"corrupt! bytes")
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            pipeline.verify_backup(self.source, self.backup, pipeline.inventory(self.source), self.manifest)

    def test_extra_missing_and_changed_during_hashing_are_rejected(self):
        self.copy_fixture()
        (self.backup / "extra.bin").write_bytes(b"extra")
        with self.assertRaisesRegex(RuntimeError, "relative file list"):
            pipeline.verify_backup(self.source, self.backup, pipeline.inventory(self.source), self.manifest)
        (self.backup / "extra.bin").unlink()
        (self.backup / "code.cpp").unlink()
        with self.assertRaisesRegex(RuntimeError, "relative file list"):
            pipeline.verify_backup(self.source, self.backup, pipeline.inventory(self.source), self.manifest)
        self.copy_fixture()
        snapshot = pipeline.inventory(self.source)
        def change_after_hash(_number, _total):
            (self.source / "late.bin").write_bytes(b"created during hashing")
        with self.assertRaisesRegex(RuntimeError, "inventory changed"):
            pipeline.verify_backup(self.source, self.backup, snapshot, self.manifest, change_after_hash)
        self.assertFalse(json.loads(self.manifest.read_text())["verified"])

    def test_backup_gate_checks_manifest_and_copied_file_metadata(self):
        self.copy_fixture()
        pipeline.verify_backup(self.source, self.backup, pipeline.inventory(self.source), self.manifest)
        marker = self.base / "verified.json"
        pipeline.atomic_json(marker, {"status": "verified", "source": str(self.source.resolve()),
                             "destination": str(self.backup), "media_signature": "media",
                             "manifest": str(self.manifest), "manifest_sha256": pipeline.sha256(self.manifest)})
        pipeline.verify_gate(marker, self.source, "media")
        with self.assertRaisesRegex(RuntimeError, "different media"):
            pipeline.verify_gate(marker, self.source, "changed-media")
        source_before = (self.source / "code.cpp").read_bytes()
        (self.source / "code.cpp").write_bytes(b"new source version")
        with self.assertRaisesRegex(RuntimeError, "project changed"):
            pipeline.verify_gate(marker, self.source, "media")
        (self.source / "code.cpp").write_bytes(source_before)
        (self.backup / "code.cpp").write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "backup file changed"):
            pipeline.verify_gate(marker, self.source, "media")

    def test_pbr_cannot_start_without_a_verified_backup_marker(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        marker = self.base / "bad-marker.json"
        marker.write_text('{"status":"not-verified"}')
        with (patch.object(coordinator, "media_gate", return_value="media"),
              patch.object(coordinator, "backup", return_value=marker),
              patch.object(pipeline, "keep_awake", return_value=nullcontext()),
              patch.object(coordinator, "command") as launch):
            with self.assertRaisesRegex(RuntimeError, "verified full-backup gate"):
                coordinator.run()
            launch.assert_not_called()
        self.assertEqual(json.loads(coordinator.state_path.read_text())["phase"], "failed")

    def test_os_lock_excludes_a_second_owner(self):
        path = self.base / "coordinator.lock"
        with pipeline.exclusive_lock(path):
            with self.assertRaisesRegex(RuntimeError, "another overnight coordinator"):
                with pipeline.exclusive_lock(path):
                    self.fail("second owner acquired the same lock")
        with pipeline.exclusive_lock(path):
            pass

    def test_keep_awake_lifetime_restores_main_thread_even_after_failure(self):
        calls, messages = [], []
        def api(flags):
            calls.append(flags)
            return 0x80000000
        with self.assertRaisesRegex(ValueError, "simulated work failure"):
            with pipeline.keep_awake(api, messages.append):
                self.assertEqual(calls, [0x80000001])
                raise ValueError("simulated work failure")
        self.assertEqual(calls, [0x80000001, 0x80000000])
        self.assertIn("acquired", messages[0])
        self.assertIn("restored", messages[-1])
        with self.assertRaisesRegex(RuntimeError, "refused"):
            with pipeline.keep_awake(lambda _flags: 0):
                self.fail("work started despite a failed wake request")


if __name__ == "__main__":
    unittest.main()
