"""Small CPU/Windows fixtures; never touch real game/media files or launch a game/worker."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from fractions import Fraction
from unittest import mock

import freeze_content as freeze


def digest(data):
    return hashlib.sha256(data).hexdigest()


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def bink(frames=3, fps=30):
    value = bytearray(128)
    value[:4] = b"BIKi"
    for at, number in [(8, frames), (20, 8), (24, 4), (28, fps), (32, 1)]:
        struct.pack_into("<I", value, at, number)
    return bytes(value)


def strict_record(cfg, src, output):
    header = freeze.bink_header(src)
    fps = Fraction(header["fps"])
    source = {**header, "codec": "binkvideo", "pix_fmt": "yuv420p"}
    pts = "".join(f"{i}\n" for i in range(header["frames"])).encode("ascii")
    return {"origin": "original-bink", "historical_source_and_weights_verified": True,
            "output_sha256": digest(output.read_bytes()),
            "recipe": {"source_sha256": digest(src.read_bytes()), "source_bytes": src.stat().st_size,
                       "source": source, "model": freeze.model_identity(cfg), "pipeline_version": 1,
                       "output_scale": 3, "encoder": {"codec": "libx264", "preset": "slow", "crf": 16,
                       "pix_fmt": "yuv420p", "time_base": str(1 / fps), "video_track_timescale": fps.numerator}},
            "output": {"width": 24, "height": 12, "fps": header["fps"], "frames": header["frames"],
                       "codec": "h264", "pix_fmt": "yuv420p", "timing": {"time_base": str(1 / fps),
                       "frame_step_ts": 1, "duration_ts": header["frames"], "frames_checked": header["frames"],
                       "exact_source_timing": True, "presentation_timestamps_sha256": digest(pts)}}}


@unittest.skipUnless(os.name == "nt", "Real Windows sharing/identity fixtures")
class FreezeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mc3-freeze-fixture-")
        self.base = Path(self.temp.name)
        self.root = self.base / "project"
        self.root.mkdir()
        self.output = self.base / "new-snapshot"
        iso = put(self.root / "original.iso", b"tiny original ISO fixture" * 13)
        textures = self.root / "textures"
        put(textures / "0123456789ABCDEF.png", b"tiny PNG fixture one")
        put(textures / "FEDCBA9876543210.png", b"tiny PNG fixture two")
        source = self.root / "original-video"
        video = self.root / "video"
        self.source = put(source / "Story/Clip0.bik", bink())
        put(source / "Story/Clip1.bik", bink(frames=4))
        put(source / "Tutorial/Clip2.bik", bink(frames=5))
        self.movie = put(video / "story/clip0.mp4", b"old pixels" * (freeze.BLOCK // 10 + 7))
        put(video / "story/clip1.mp4", b"marker-only fixture")
        put(video / "tutorial/clip2.mp4", b"unrecorded fixture")
        self.partial = put(video / "story/.clip0.owned.part.mp4", b"never select owned partial")
        self.extra = put(video / "exports/unrelated.mp4", b"never select unrelated export")
        model_files = {"executable": put(self.root / "models/engine.exe", b"fixture exe"),
                       "realesrgan-x4plus.param": put(self.root / "models/realesrgan-x4plus.param", b"fixture param"),
                       "realesrgan-x4plus.bin": put(self.root / "models/realesrgan-x4plus.bin", b"fixture weights")}
        ledger = put(self.root / "reviewed_fixes.json", b'{"entries":{}}\n')
        audit = put(self.root / "texture-audit.json", b'{"complete":true,"counts":{"eligible":2}}\n')
        self.cfg = freeze.SnapshotConfig(self.root, self.output, iso, textures, source, video, ledger, audit,
                                         model_files, expected_textures=2, expected_movies=3,
                                         expected_iso_sha256=digest(iso.read_bytes()), source_revision="a" * 40)
        self.stop = put(self.root / ".local/stop-overnight-pipeline", b'{"owner":"tiny fixture root"}\n')
        self.cfg.held_stop_sha256 = digest(self.stop.read_bytes())
        self.record = strict_record(self.cfg, self.source, self.movie)
        self.meta = self.movie.with_suffix(".upscale.json")
        put(self.meta, freeze.json_bytes(self.record))
        put(self.movie.with_suffix(".model.txt"), b"realesrgan-x4plus\n")
        put(video / "story/clip1.model.txt", b"realesrgan-x4plus\n")
        put(self.root / "settings.toml", b"private settings must not ship")
        put(self.root / "hdd/player.save", b"private save must not ship")

    def execute(self, **kwargs):
        return freeze.freeze(self.cfg, execute=True, mode="quiescent", worker_query=lambda: [], **kwargs)

    def tearDown(self):
        # TemporaryDirectory owns this exact isolated fixture; junction tests remove entries before this call.
        assert self.base.parent == Path(tempfile.gettempdir()) and self.base.name.startswith("mc3-freeze-fixture-")
        self.temp.cleanup()

    def test_plan_is_inert_and_exact(self):
        before = {p: digest(p.read_bytes()) for p in (self.partial, self.extra)}
        plan = freeze.freeze(self.cfg)
        self.assertEqual((plan["textures"], plan["movies"], plan["payload_files"]), (2, 3, 6))
        self.assertEqual(plan["mode"], "plan")
        self.assertFalse(self.output.exists())
        self.assertEqual(before, {p: digest(p.read_bytes()) for p in before})

    def test_complete_independent_cohort_and_honest_provenance(self):
        manifest = self.execute()
        self.assertEqual(manifest["status"], "complete")
        self.assertEqual(manifest["payload_files"], 6)
        self.assertEqual(manifest["source_bink_count"], 3)
        self.assertEqual(manifest["movie_cohort_counts"], {"strict-original-record": 1,
                         "historical-marker-only": 1, "historical-unrecorded": 1, "historical-json-unverified": 0})
        self.assertEqual(manifest["decode_and_full_timing_validation"], "PENDING")
        self.assertTrue((self.output / "snapshot-manifest.json").is_file())
        self.assertFalse((self.output / "snapshot-failed.json").exists())
        self.assertFalse(any("settings.toml" in p or "player.save" in p or ".part.mp4" in p for p in manifest["inventory"]))
        for relative, row in manifest["payload"].items():
            p = self.output / relative
            self.assertEqual(digest(p.read_bytes()), row["sha256"])
            self.assertEqual(p.stat().st_nlink, 1)
            self.assertFalse(os.path.samefile(p, row["source"]))
        absent = manifest["movies"]["tutorial/clip2.mp4"]["anchors"]
        self.assertFalse(absent["json"]["present"])
        self.assertFalse(absent["marker"]["present"])

    def test_actual_os_replace_is_unsupported_while_held_and_concurrent_mode_refuses(self):
        capability = freeze.publication_capability(self.base)
        self.assertFalse(capability["supported"])
        self.assertEqual(capability["winerror"], 5)
        with self.assertRaisesRegex(freeze.SnapshotError, "Concurrent snapshot is unsafe"):
            freeze.freeze(self.cfg, execute=True, mode="concurrent")
        self.assertFalse(self.output.exists())
        # Delete sharing still permits renaming an open object to a NEW unused name; it does not
        # make the actual worker's os.replace onto an already-open destination succeed on this host.
        for p in (self.movie, self.meta):
            original = p.read_bytes()
            retired = p.with_name(p.name + ".owned-retired")
            with freeze.SharedReader(p) as held:
                old_id = held.fingerprint()
                os.rename(p, retired)
                self.assertEqual(held.file.read(), original)
                self.assertEqual(old_id["file_id"], freeze.fingerprint(retired)["file_id"])
            os.rename(retired, p)

    def test_anchor_rename_does_not_block_but_rejects_changed_observation(self):
        called = False
        def hook(name, **v):
            nonlocal called
            if name == "anchor-open" and not called:
                called = True
                os.rename(self.meta, self.meta.with_name("retained-meta.fixture"))
                os.rename(put(self.meta.with_name("new-meta.tmp"), b"new JSON"), self.meta)
        with self.assertRaises(freeze.CohortChanged):
            freeze.read_anchor(self.meta, hook=hook)
        self.assertTrue(called)
        self.assertEqual(self.meta.read_bytes(), b"new JSON")

    def test_movie_publication_and_sidecar_change_retry_without_accepting_mixed_copy(self):
        changed = False
        def hook(name, **v):
            nonlocal changed
            if name == "movie-before-after-anchors" and v["source"] == self.movie and not changed:
                changed = True
                os.replace(put(self.movie.with_name("new-output.tmp"), b"newly published pixels"), self.movie)
                updated = strict_record(self.cfg, self.source, self.movie)
                os.replace(put(self.meta.with_name("new-sidecar.tmp"), freeze.json_bytes(updated)), self.meta)
        result = self.execute(hook=hook)
        row = result["movies"]["story/clip0.mp4"]
        self.assertEqual(row["attempts"], 2)
        self.assertTrue(row["strict_recorded_provenance"])
        self.assertEqual((self.output / "video_hd/story/clip0.mp4").read_bytes(), b"newly published pixels")
        self.assertTrue(list((self.output / "evidence/attempts").rglob("rejected.json")))

    def test_rename_during_source_copy_retries_same_source_handle_safely(self):
        changed = False
        def hook(name, **v):
            nonlocal changed
            if name == "source-open" and v["source"] == self.movie and not changed:
                changed = True
                os.rename(self.movie, self.movie.with_name("retained-output.fixture"))
                os.rename(put(self.movie.with_name("new-output.tmp"), b"published during held source copy"), self.movie)
                updated = strict_record(self.cfg, self.source, self.movie)
                os.replace(put(self.meta.with_name("new-sidecar.tmp"), freeze.json_bytes(updated)), self.meta)
        result = self.execute(hook=hook)
        self.assertEqual(result["movies"]["story/clip0.mp4"]["attempts"], 2)

    def test_in_place_mutation_is_detected_even_when_length_is_unchanged(self):
        source = put(self.root / "in-place.bin", b"A" * (freeze.BLOCK + 31))
        before = source.stat()
        def hook(name, **v):
            if name == "after-first-copy":
                source.write_bytes(b"B" * (freeze.BLOCK + 31))
                os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
        with self.assertRaises(freeze.CohortChanged):
            freeze.copy_verified(source, self.base / "unaccepted-copy", hook=hook)

    def test_mismatched_authoritative_json_exhausts_bounded_retries_and_never_seals(self):
        bad = copy.deepcopy(self.record)
        bad["output_sha256"] = "0" * 64
        self.meta.write_bytes(freeze.json_bytes(bad))
        self.cfg.attempts = 2
        with self.assertRaisesRegex(freeze.CohortChanged, "after 2 attempts"):
            self.execute()
        self.assertEqual(len(list((self.output / "evidence/attempts").rglob("rejected.json"))), 2)
        self.assertFalse((self.output / "snapshot-manifest.json").exists())
        self.assertEqual(json.loads((self.output / "snapshot-failed.json").read_text())["status"], "failed")
        with self.assertRaisesRegex(freeze.SnapshotError, "NEW"):
            self.execute()

    def test_null_authoritative_json_is_not_silently_legacy(self):
        self.meta.write_bytes(b"null\n")
        self.cfg.attempts = 1
        with self.assertRaises(freeze.CohortChanged):
            self.execute()
        self.assertFalse((self.output / "snapshot-manifest.json").exists())

    def test_matching_output_with_wrong_source_or_model_is_explicitly_unverified(self):
        bad = copy.deepcopy(self.record)
        bad["recipe"]["source_sha256"] = "0" * 64
        self.meta.write_bytes(freeze.json_bytes(bad))
        result = self.execute()
        self.assertEqual(result["movies"]["story/clip0.mp4"]["classification"], "historical-json-unverified")
        self.assertEqual(result["movie_cohort_counts"]["strict-original-record"], 0)

    def test_counts_missing_outputs_and_wrong_iso_fail_closed(self):
        self.cfg.expected_movies = 106
        with self.assertRaisesRegex(freeze.SnapshotError, "movie count"):
            freeze.freeze(self.cfg)
        self.cfg.expected_movies = 3
        self.cfg.expected_textures = 7081
        with self.assertRaisesRegex(freeze.SnapshotError, "texture count"):
            freeze.freeze(self.cfg)
        self.cfg.expected_textures = 2
        self.cfg.expected_iso_sha256 = "0" * 64
        with self.assertRaisesRegex(freeze.SnapshotError, "SHA-256"):
            self.execute()
        self.assertFalse((self.output / "snapshot-manifest.json").exists())

    def test_missing_expected_generated_movie_cannot_be_replaced_by_part(self):
        self.movie.unlink()
        with self.assertRaisesRegex(freeze.SnapshotError, "Missing expected"):
            freeze.freeze(self.cfg)
        self.assertTrue(self.partial.is_file())

    def test_unsafe_paths_existing_destination_and_project_output_are_rejected(self):
        for value in ("../escape.bik", "story//bad.bik", "C:/absolute.bik", "story\\bad.bik", "/absolute.bik",
                      "story/CON.bik", "AUX/movie.bik", "story/trailing./movie.bik", "story/trailing /movie.bik"):
            with self.subTest(value=value), self.assertRaises(freeze.SnapshotError):
                freeze.safe_relative(value)
        self.output.mkdir()
        with self.assertRaisesRegex(freeze.SnapshotError, "NEW"):
            freeze.freeze(self.cfg)
        self.cfg.output = self.root / "snapshot"
        with self.assertRaisesRegex(freeze.SnapshotError, "outside"):
            freeze.freeze(self.cfg)
        old = put(self.base / "existing-copy", b"preserve existing")
        with self.assertRaises(FileExistsError):
            freeze.copy_verified(self.source, old)
        self.assertEqual(old.read_bytes(), b"preserve existing")

    def test_duplicate_case_insensitive_source_catalogue_rejected(self):
        real = freeze.walk_files
        def duplicate(path):
            result = real(path)
            return result + [self.source.parent / "CLIP0.BIK"] if Path(path) == self.cfg.source_video else result
        with mock.patch.object(freeze, "walk_files", side_effect=duplicate):
            with self.assertRaisesRegex(freeze.SnapshotError, "duplicate original"):
                freeze.freeze(self.cfg)

    def test_linked_root_ancestor_is_rejected_and_only_fixture_junction_is_retired(self):
        link = self.base / "linked-project"
        quote = lambda p: "'" + str(p).replace("'", "''") + "'"
        command = f"New-Item -ItemType Junction -Path {quote(link)} -Target {quote(self.root)} | Out-Null"
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                       check=True, capture_output=True, text=True)
        try:
            with self.assertRaisesRegex(freeze.SnapshotError, "Linked path"):
                freeze.plain(link / "video/story/clip0.mp4", directory=False)
            self.cfg.video = link / "video"
            with self.assertRaisesRegex(freeze.SnapshotError, "Linked path"):
                freeze.freeze(self.cfg)
        finally:
            self.assertTrue(os.lstat(link).st_file_attributes & 0x400)
            os.rmdir(link)  # Never recurse through the junction or mutate its target.
        self.assertTrue(self.movie.is_file())

    def test_model_change_before_seal_retains_failed_snapshot(self):
        def hook(name, **v):
            if name == "before-complete":
                self.cfg.model_files["realesrgan-x4plus.bin"].write_bytes(b"changed fixture weights")
        with self.assertRaisesRegex(freeze.SnapshotError, "changed during snapshot"):
            self.execute(hook=hook)
        self.assertFalse((self.output / "snapshot-manifest.json").exists())
        self.assertTrue((self.output / "snapshot-failed.json").exists())

    def test_quiescent_live_worker_refusal_precedes_any_payload_copy(self):
        workers = [{"ProcessId": 45678, "Name": "python.exe", "CommandLine": "tools/video/upscale_movies.py"}]
        with self.assertRaisesRegex(freeze.SnapshotError, "blocked by live"):
            freeze.freeze(self.cfg, execute=True, mode="quiescent", worker_query=lambda: workers)
        self.assertFalse(self.output.exists())
        self.assertEqual(digest(self.stop.read_bytes()), self.cfg.held_stop_sha256)

    def test_quiescent_checks_worker_arrival_during_copy_and_never_seals(self):
        arrived = False
        queries = 0
        def query():
            nonlocal queries
            queries += 1
            return [{"ProcessId": 45678, "Name": "mc3.exe"}] if arrived else []
        def hook(name, **v):
            nonlocal arrived
            if name == "after-first-copy":
                arrived = True
        with self.assertRaisesRegex(freeze.SnapshotError, "blocked by live"):
            freeze.freeze(self.cfg, execute=True, mode="quiescent", worker_query=query, hook=hook)
        self.assertGreaterEqual(queries, 3)
        self.assertFalse((self.output / "snapshot-manifest.json").exists())
        self.assertTrue((self.output / "snapshot-failed.json").exists())

    def test_all_three_exact_os_locks_exclude_snapshot(self):
        exclusion, _query = freeze.quiescent_helpers()
        with exclusion(self.root):  # Loads the same tested lock implementation against fixture paths only.
            import overnight_pipeline
        for path in (self.root / ".local/overnight_pipeline/live/coordinator.lock",
                     self.root / ".local/reviewed_texture_install/install.lock", self.cfg.video / ".upscale.lock"):
            with self.subTest(lock=str(path)):
                with overnight_pipeline.exclusive_lock(path):
                    with self.assertRaisesRegex(RuntimeError, "operating-system lock"):
                        self.execute()
                self.assertFalse(self.output.exists())

    def test_video_output_lock_held_through_copy_and_released_after_success(self):
        blocked = False
        def hook(name, **v):
            nonlocal blocked
            if name == "before-complete":
                import overnight_pipeline
                try:
                    with overnight_pipeline.exclusive_lock(self.cfg.video / ".upscale.lock"):
                        self.fail("A second normal output-lock owner entered during snapshot")
                except RuntimeError:
                    blocked = True
        self.execute(hook=hook)
        self.assertTrue(blocked)
        import overnight_pipeline
        with overnight_pipeline.exclusive_lock(self.cfg.video / ".upscale.lock"):
            pass

    def test_video_output_lock_released_after_copy_failure(self):
        self.cfg.expected_iso_sha256 = "0" * 64
        with self.assertRaises(freeze.SnapshotError):
            self.execute()
        import overnight_pipeline
        with overnight_pipeline.exclusive_lock(self.cfg.video / ".upscale.lock"):
            pass

    def test_existing_hardlinked_output_lock_is_refused_without_modifying_bytes(self):
        original = put(self.root / "unrelated-fixture-lock", b"preserve shared fixture bytes")
        linked = self.cfg.video / ".upscale.lock"
        os.link(original, linked)
        with self.assertRaisesRegex(freeze.SnapshotError, "Linked existing video-output lock"):
            self.execute()
        self.assertEqual(original.read_bytes(), b"preserve shared fixture bytes")
        self.assertFalse(self.output.exists())

    def test_owned_stop_must_match_and_user_or_transaction_flags_are_preserved(self):
        self.cfg.held_stop_sha256 = "0" * 64
        with self.assertRaisesRegex(freeze.SnapshotError, "absent/changed"):
            self.execute()
        self.cfg.held_stop_sha256 = digest(self.stop.read_bytes())
        for relative in (".local/stop-video-upscale", ".local/stop-texture-upscale",
                         ".local/reviewed_texture_install/active.json"):
            p = put(self.root / relative, b"foreign/user bytes to preserve")
            try:
                with self.subTest(flag=relative), self.assertRaises(freeze.SnapshotError):
                    self.execute()
                self.assertEqual(p.read_bytes(), b"foreign/user bytes to preserve")
                self.assertFalse(self.output.exists())
            finally:
                p.unlink()  # Only this exact synthetic fixture flag is removed.
        self.assertEqual(digest(self.stop.read_bytes()), self.cfg.held_stop_sha256)

    def test_execution_requires_explicit_mode(self):
        with self.assertRaisesRegex(freeze.SnapshotError, "explicitly selected"):
            freeze.freeze(self.cfg, execute=True)
        self.assertFalse(self.output.exists())

    def test_cli_cannot_override_production_counts_or_execute_to_project_drive(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            freeze.main(["--output", str(self.output), "--expected-movies", "3"])
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            freeze.main(["--execute", "--output", str(self.output)])
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
