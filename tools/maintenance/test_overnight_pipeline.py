"""Fixture-only backup/media-gate tests. No GPU, robocopy or PBR process is started."""
import copy
from contextlib import contextmanager, nullcontext
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
             "require_original_provenance": True, "all_original_source_and_weights_verified": True,
             "failed": 0, "details": [{"movie": f"movie{number}.bik", "status": "verified",
                                        "output_validated": True, "recorded_model": "realesrgan-x4plus",
                                        "origin": "original-bink", "historical_source_and_weights_verified": True}
                                       for number in range(106)]}
    return texture, video


def reviewed_report():
    base, _ = valid_reports()
    name = "0000000000000001.png"
    base.update(complete=False, counts={"eligible": 2, "complete_named_pass": 1},
                issues={"missing_or_changed_pass_record": [name], "other": []})
    entry = {"recipe": "garage-joined-cpu-x4plus-v1", "current_backend": "pytorch-cpu-x4plus-v1",
             "model_sha256": "1" * 64, "source_sha256": "2" * 64, "before_sha256": "3" * 64,
             "after_sha256": "4" * 64, "alpha_sha256": "5" * 64, "base_archive": "textures/archive.png",
             "review_proof": ".local/review.json", "candidate_proofs": {"split": "authenticated"}}
    return {"format": "mc3-reviewed-texture-audit-v1", "base_model": "ultrasharp-4x",
            "base_pass_id": pipeline.PASS_ID, "current_model": "mixed-reviewed", "base_complete": True,
            "complete": True, "ledger_sha256": "6" * 64, "base_audit": base, "reviewed_fixes": {name: entry},
            "counts": {"eligible": 2, "base_named_pass_verified": 2, "current_original_named_pass": 1,
                       "reviewed_fixes": 1, "accounted_changed_pass_records": 1}, "issues": {"other": []}}


class FakeMedia:
    """A resumable child that acknowledges --stop-file only after its current item completes."""
    def __init__(self, coordinator, phase="textures", drain_polls=0, boundary_code=2, game_name="mc3.exe"):
        self.coordinator, self.phase = coordinator, phase
        self.external_stop = coordinator.texture_stop if phase == "textures" else coordinator.video_stop
        self.args = ["fixture-python", "fixture-worker", "--pass-id", "same-pass", "--stop-file", str(self.external_stop)]
        self.completed, self.launches, self.events = [], [], []
        self.game, self.clock = False, 0
        self.game_name = game_name
        self.drain_polls, self.boundary_code = drain_polls, boundary_code
        self.on_first_poll = lambda: setattr(self, "game", True)
        self.on_sleep = lambda _seconds: None

    def workers(self):
        return [{"Name": self.game_name, "ProcessId": 700, "CommandLine": self.game_name}] if self.game else []

    def sleep(self, seconds):
        self.clock += seconds
        self.events.append(("sleep", seconds))
        self.on_sleep(seconds)
        if seconds == 30 and self.game:
            self.game = False
            self.events.append(("game-closed",))

    def popen(self, args, **_kwargs):
        assert not self.game, "GPU child launched while the game was running"
        self.launches.append(list(args))
        number = len(self.launches)
        self.events.append(("launch", number))
        stop = Path(args[args.index("--stop-file") + 1])
        assert stop != self.external_stop and stop.parent == self.coordinator.live
        owner = self

        class Process:
            pid = 1000 + number
            returncode = None
            stop_polls = 0

            def __enter__(self):
                return self

            def __exit__(self, *_error):
                assert self.returncode is not None, "coordinator abandoned a live child"

            def poll(self):
                if self.returncode is not None:
                    return self.returncode
                if stop.exists():
                    self.stop_polls += 1
                    if self.stop_polls <= owner.drain_polls:
                        return None
                    self.returncode = owner.boundary_code
                    owner.events.append(("boundary-exit", number, self.returncode))
                    return self.returncode
                owner.completed.append("item" + str(len(owner.completed)))
                owner.events.append(("completed", owner.completed[-1]))
                if number == 1 and len(owner.completed) == 1:
                    owner.on_first_poll()
                if len(owner.completed) == 3:
                    self.returncode = 0
                return self.returncode

            def terminate(self):
                raise AssertionError("cooperative pause must never terminate a child")

            kill = terminate

        return Process()

    @contextmanager
    def installed(self):
        with (patch.object(pipeline.subprocess, "Popen", side_effect=self.popen),
              patch.object(pipeline, "active_workers", side_effect=self.workers),
              patch.object(pipeline.time, "sleep", side_effect=self.sleep),
              patch.object(pipeline.time, "monotonic", side_effect=lambda: self.clock)):
            yield


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

    def correction_graph(self):
        proof = self.source / ".local/review/proof.json"
        proof.parent.mkdir(parents=True); proof.write_bytes(b"original review proof")
        ledger = self.source / pipeline.TEXTURES / "reviewed_fixes.json"
        pipeline.atomic_json(ledger, {"dependencies": {proof.relative_to(self.source).as_posix(): pipeline.sha256(proof)}})
        return ledger, proof

    def test_reviewed_report_keeps_exact_original_coverage_and_movie_provenance(self):
        texture = reviewed_report(); _, video = valid_reports()
        pipeline.validate_media_reports(texture, video)
        mutations = [lambda x: x["counts"].update(reviewed_fixes=2),
                     lambda x: x["counts"].update(current_original_named_pass=2),
                     lambda x: x["counts"].update(accounted_changed_pass_records=0),
                     lambda x: x.update(base_model="realesrgan-x4plus"),
                     lambda x: x.update(current_model="ultrasharp-4x"),
                     lambda x: x["base_audit"]["issues"]["other"].append("unknown.png"),
                     lambda x: x["base_audit"]["issues"]["missing_or_changed_pass_record"].append("OTHER.png"),
                     lambda x: x["reviewed_fixes"]["0000000000000001.png"].update(review_proof=None),
                     lambda x: x.update(ledger_sha256="invalid")]
        for change in mutations:
            candidate = copy.deepcopy(texture); change(candidate)
            with self.subTest(candidate=candidate), self.assertRaises(RuntimeError):
                pipeline.validate_media_reports(candidate, video)
        video["details"][0]["historical_source_and_weights_verified"] = False
        with self.assertRaisesRegex(RuntimeError, "strict source/weights"):
            pipeline.validate_media_reports(texture, video)

    def test_reviewed_preflight_and_final_audit_never_launch_the_base_texture_worker(self):
        self.correction_graph(); coordinator = pipeline.Coordinator(self.source, self.backup)
        phases = []; texture = reviewed_report(); _, video = valid_reports()
        def complete(phase, args, **_kwargs):
            phases.append(phase)
            if phase in ("reviewed-textures-preflight", "texture-audit"):
                self.assertIn("tools/texture_pack/reviewed_fixes.py", args)
                target = Path(args[args.index("--output") + 1])
                self.assertFalse(target.exists())
                pipeline.atomic_json(target, texture)
            if phase == "video-audit": pipeline.atomic_json(coordinator.video_report, video)
        with patch.object(coordinator, "wait_idle"), patch.object(coordinator, "command", side_effect=complete):
            signature = coordinator.media_gate()
        self.assertEqual(phases, ["reviewed-textures-preflight", "videos", "texture-audit", "video-audit"])
        self.assertEqual(signature, pipeline.media_signature(self.source))
        self.assertEqual(json.loads(coordinator.texture_report.read_text()), texture)
        self.assertTrue(coordinator.cached_media_verified(signature))

    def test_bad_reviewed_preflight_preserves_last_good_report_and_stops_before_gpu_work(self):
        self.correction_graph(); coordinator = pipeline.Coordinator(self.source, self.backup)
        coordinator.work.mkdir(parents=True, exist_ok=True)
        coordinator.texture_report.write_bytes(b"preserved old report")
        phases = []; targets = []
        def bad_audit(phase, args, **_kwargs):
            phases.append(phase); targets.append(Path(args[args.index("--output") + 1]))
            candidate = reviewed_report(); candidate["complete"] = False
            pipeline.atomic_json(targets[-1], candidate)
        with patch.object(coordinator, "wait_idle"), patch.object(coordinator, "command", side_effect=bad_audit):
            with self.assertRaises(RuntimeError): coordinator.media_gate()
        self.assertEqual(phases, ["reviewed-textures-preflight"])
        self.assertEqual(coordinator.texture_report.read_bytes(), b"preserved old report")
        self.assertTrue(targets[0].exists())  # Failed evidence is retained separately.

    def test_reviewed_dependency_tampering_fails_even_with_equal_size_and_mtime(self):
        ledger, proof = self.correction_graph(); signature = pipeline.media_signature(self.source)
        before = proof.stat(); proof.write_bytes(b"modified review proof")
        self.assertEqual(before.st_size, proof.stat().st_size)
        os.utime(proof, ns=(before.st_atime_ns, before.st_mtime_ns))
        coordinator = pipeline.Coordinator(self.source, self.backup)
        with patch.object(coordinator, "command") as command:
            with self.assertRaisesRegex(RuntimeError, "changed dependency"): coordinator.media_gate()
        command.assert_not_called()
        document = json.loads(ledger.read_text()); document["dependencies"][".local/review/proof.json"] = pipeline.sha256(proof)
        pipeline.atomic_json(ledger, document)
        self.assertNotEqual(signature, pipeline.media_signature(self.source))
        proof.unlink()
        with self.assertRaisesRegex(RuntimeError, "dependency"): pipeline.media_signature(self.source)

    def test_reviewed_signature_rejects_malformed_and_escaping_dependency_graphs(self):
        ledger, _ = self.correction_graph()
        for dependencies in (None, [], {}, {"../outside": "0" * 64}, {"C:/outside": "0" * 64},
                             {"a//b": "0" * 64}, {".local/review/proof.json": "wrong"}):
            pipeline.atomic_json(ledger, {"dependencies": dependencies})
            with self.subTest(dependencies=dependencies), self.assertRaises(RuntimeError):
                pipeline.media_signature(self.source)

    def test_correction_code_changes_invalidate_the_media_signature(self):
        signature = pipeline.media_signature(self.source)
        helper = self.source / "tools/texture_pack/reviewed_fixes.py"
        helper.parent.mkdir(parents=True); helper.write_text("first audit implementation")
        first = pipeline.media_signature(self.source); self.assertNotEqual(signature, first)
        helper.write_text("changed audited implementation")
        self.assertNotEqual(first, pipeline.media_signature(self.source))

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

    def media_cache_fixture(self, coordinator, signature="media"):
        texture, video = valid_reports()
        pipeline.atomic_json(coordinator.texture_report, texture)
        pipeline.atomic_json(coordinator.video_report, video)
        coordinator.state["media_gate"] = {
            "signature": signature, "provenance_policy": pipeline.MEDIA_PROVENANCE_POLICY,
            "texture_report_sha256": pipeline.sha256(coordinator.texture_report),
            "video_report_sha256": pipeline.sha256(coordinator.video_report)}
        return texture, video

    def test_strict_movie_gate_rejects_legacy_claims_even_when_model_and_decode_pass(self):
        texture, video = valid_reports()
        for field, value in (("require_original_provenance", False),
                             ("all_original_source_and_weights_verified", False)):
            changed = copy.deepcopy(video); changed[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, "strict source/weights"):
                pipeline.validate_media_reports(texture, changed)
        for field, value in (("historical_source_and_weights_verified", False),
                             ("historical_source_and_weights_verified", 1),
                             ("origin", "validated-legacy-model-marker")):
            changed = copy.deepcopy(video); changed["details"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaisesRegex(RuntimeError, "strict source/weights"):
                pipeline.validate_media_reports(texture, changed)

    def test_strict_unchanged_media_cache_reuses_authenticated_reports(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        self.media_cache_fixture(coordinator)
        with (patch.object(pipeline, "media_signature", return_value="media"),
              patch.object(coordinator, "wait_idle") as idle,
              patch.object(coordinator, "command") as launch):
            self.assertEqual(coordinator.media_gate(), "media")
            idle.assert_not_called(); launch.assert_not_called()
        self.assertEqual(coordinator.state["phase"], "using-unchanged-verified-media-gate")

    def test_old_model_only_or_malformed_strict_gate_is_revalidated_with_strict_cli(self):
        for legacy_policy in (True, False):
            with self.subTest(legacy_policy=legacy_policy):
                coordinator = pipeline.Coordinator(self.source, self.backup)
                texture, strict_video = self.media_cache_fixture(coordinator)
                if legacy_policy:
                    coordinator.state["media_gate"].pop("provenance_policy")
                legacy_video = copy.deepcopy(strict_video)
                legacy_video["details"][0]["origin"] = "validated-legacy-model-marker"
                legacy_video["details"][0]["historical_source_and_weights_verified"] = False
                pipeline.atomic_json(coordinator.video_report, legacy_video)
                coordinator.state["media_gate"]["video_report_sha256"] = pipeline.sha256(coordinator.video_report)
                commands = []
                def complete(phase, args, **kwargs):
                    commands.append((phase, args))
                    if phase == "video-audit":
                        pipeline.atomic_json(coordinator.video_report, strict_video)
                with (patch.object(pipeline, "media_signature", return_value="media"),
                      patch.object(coordinator, "wait_idle"),
                      patch.object(coordinator, "command", side_effect=complete)):
                    self.assertEqual(coordinator.media_gate(), "media")
                self.assertEqual([phase for phase, _ in commands], ["textures", "videos", "texture-audit", "video-audit"])
                for phase, args in commands:
                    if phase in ("videos", "video-audit"):
                        self.assertEqual(args.count("--require-original-provenance"), 1)
                self.assertEqual(coordinator.state["media_gate"]["provenance_policy"], pipeline.MEDIA_PROVENANCE_POLICY)

    def test_completed_pbr_with_old_provenance_cannot_bypass_fresh_media_gate(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        marker = self.base / "old-backup.json"; marker.write_text("{}")
        pbr = self.source / "templates/game/out/native/textures/pbr/manifest.json"
        pbr.parent.mkdir(parents=True); pbr.write_text("old offline conversion")
        coordinator.state.update(backup_marker=str(marker), pbr_complete={
            "backup_marker_sha256": pipeline.sha256(marker), "manifest_sha256": pipeline.sha256(pbr)})
        for cache_valid in (False, True):
            with self.subTest(cache_valid=cache_valid):
                with (patch.object(coordinator, "cached_media_verified", return_value=cache_valid),
                      patch.object(pipeline, "verify_gate", return_value={"media_gate": {}}),
                      patch.object(coordinator, "media_gate", side_effect=RuntimeError("fresh media path reached")) as media,
                      patch.object(pipeline, "keep_awake", return_value=nullcontext())):
                    with self.assertRaisesRegex(RuntimeError, "fresh media path reached"):
                        coordinator.run()
                    media.assert_called_once()

    def test_old_backup_policy_is_preserved_but_not_reused_for_new_pbr_gate(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        marker = self.base / "old-backup.json"; marker.write_text("old preserved marker")
        coordinator.state.update(backup_marker=str(marker), backup_destination=str(self.backup / "old"),
                                 media_gate={"provenance_policy": pipeline.MEDIA_PROVENANCE_POLICY})
        snapshot = {"files": {"file.bin": {"bytes": 1, "mtime_ns": 1}}, "exclusions": []}
        with (patch.object(coordinator, "wait_idle"), patch.object(coordinator, "command") as launch,
              patch.object(pipeline, "verify_gate", return_value={"media_gate": {}}),
              patch.object(pipeline, "inventory", return_value=snapshot),
              patch.object(pipeline, "verify_backup", return_value={"file_count": 1, "byte_count": 1}),
              patch.object(pipeline, "sha256", return_value="manifest-hash"),
              patch.object(pipeline, "media_signature", return_value="media")):
            fresh = coordinator.backup("media")
        self.assertNotEqual(fresh, marker)
        self.assertEqual(marker.read_text(), "old preserved marker")
        self.assertEqual(launch.call_args.args[0], "backup-copy")
        self.assertEqual(json.loads(fresh.read_text())["media_gate"]["provenance_policy"], pipeline.MEDIA_PROVENANCE_POLICY)

    def test_completed_pbr_with_changed_media_checkpoint_reenters_gates(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        marker = self.base / "old-backup.json"; marker.write_text("{}")
        pbr = self.source / "templates/game/out/native/textures/pbr/manifest.json"
        pbr.parent.mkdir(parents=True); pbr.write_text("earlier conversion")
        coordinator.state.update(backup_marker=str(marker), pbr_complete={
            "backup_marker_sha256": pipeline.sha256(marker), "manifest_sha256": pipeline.sha256(pbr)})
        with (patch.object(coordinator, "cached_media_verified", return_value=True),
              patch.object(pipeline, "verify_gate", side_effect=RuntimeError("different media")),
              patch.object(coordinator, "media_gate", side_effect=RuntimeError("fresh media path reached")) as media,
              patch.object(pipeline, "keep_awake", return_value=nullcontext())):
            with self.assertRaisesRegex(RuntimeError, "fresh media path reached"):
                coordinator.run()
            media.assert_called_once()

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

    def test_registered_texture_transaction_blocks_startup_media_and_pbr_without_mutation(self):
        marker = self.source / pipeline.ACTIVE_TEXTURE_TRANSACTION
        marker.parent.mkdir(parents=True)
        for content in (b'{"status":"applying"}', b'{"status":"committed-before-marker-retirement"}',
                        b'foreign registration', b'{truncated'):
            marker.write_bytes(content)
            coordinator = pipeline.Coordinator(self.source, self.backup)
            with (patch.object(pipeline, "keep_awake", return_value=nullcontext()),
                  patch.object(coordinator, "cached_media_verified") as cached,
                  patch.object(coordinator, "media_gate") as media,
                  patch.object(coordinator, "backup") as backup,
                  patch.object(coordinator, "command") as launch):
                self.assertEqual(coordinator.run(), 2)
                cached.assert_not_called(); media.assert_not_called(); backup.assert_not_called(); launch.assert_not_called()
            self.assertEqual(marker.read_bytes(), content)
            self.assertEqual(coordinator.state["phase"], "paused")
            self.assertIn("unfinished reviewed texture transaction", coordinator.state["reason"])
            with self.assertRaises(pipeline.Paused): coordinator.media_gate()

    def test_registered_transaction_prevents_child_launch_and_is_not_treated_as_a_game_pause(self):
        marker = self.source / pipeline.ACTIVE_TEXTURE_TRANSACTION
        marker.parent.mkdir(parents=True); marker.write_bytes(b'preserve exact registration')
        coordinator = pipeline.Coordinator(self.source, self.backup)
        with patch.object(pipeline.subprocess, "Popen") as process:
            with self.assertRaisesRegex(pipeline.Paused, "unfinished reviewed texture transaction"):
                coordinator.command("textures", ["fixture-python", "fixture-worker"])
            process.assert_not_called()
        self.assertEqual(marker.read_bytes(), b'preserve exact registration')

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

    def test_game_arrival_drains_media_then_waits_and_resumes_remaining_items(self):
        for phase in ("textures", "videos"):
            with self.subTest(phase=phase):
                coordinator = pipeline.Coordinator(self.source, self.backup)
                media = FakeMedia(coordinator, phase)
                with media.installed():
                    coordinator.command(phase, media.args, child_stop=media.external_stop)
                self.assertEqual(media.completed, ["item0", "item1", "item2"])
                self.assertEqual(len(media.launches), 2)
                self.assertLess(media.events.index(("boundary-exit", 1, 2)), media.events.index(("game-closed",)))
                self.assertLess(media.events.index(("game-closed",)), media.events.index(("launch", 2)))
                markers = [Path(args[-1]) for args in media.launches]
                self.assertNotEqual(markers[0], markers[1])
                self.assertTrue(all(not marker.exists() for marker in markers))
                self.assertFalse(media.external_stop.exists())
                self.assertFalse(coordinator.stop_file.exists())
                self.assertEqual(media.launches[0][:-1], media.launches[1][:-1])

    def test_other_installed_game_drains_and_prevents_gpu_resume_until_closed(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator, game_name="FortniteClient-Win64-Shipping.exe")
        with media.installed():
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(media.completed, ["item0", "item1", "item2"])
        self.assertEqual(len(media.launches), 2)
        self.assertLess(media.events.index(("boundary-exit", 1, 2)), media.events.index(("game-closed",)))
        self.assertLess(media.events.index(("game-closed",)), media.events.index(("launch", 2)))
        self.assertFalse(media.external_stop.exists())

    def test_existing_user_stop_flags_prevent_launch_and_are_preserved(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator)
        media.external_stop.parent.mkdir(parents=True)
        media.external_stop.write_text("user texture cancellation")
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "child stop flag"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(media.launches, [])
        self.assertEqual(media.external_stop.read_text(), "user texture cancellation")
        coordinator.stop_file.write_text("root cancellation")
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "coordinator stop"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(media.launches, [])
        self.assertEqual(coordinator.stop_file.read_text(), "root cancellation")
        self.assertEqual(media.external_stop.read_text(), "user texture cancellation")

    def test_user_stop_has_priority_when_game_arrives_and_is_not_deleted(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator)
        def arrivals():
            media.game = True
            coordinator.stop_file.write_text("user requested stop")
            media.external_stop.write_text("external child stop")
        media.on_first_poll = arrivals
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "coordinator stop"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(len(media.launches), 1)
        self.assertNotIn(("game-closed",), media.events)
        self.assertEqual(coordinator.stop_file.read_text(), "user requested stop")
        self.assertEqual(media.external_stop.read_text(), "external child stop")
        self.assertFalse(Path(media.launches[0][-1]).exists())

    def test_cancel_observed_while_game_pause_drains_is_latched_after_flag_removal(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator, drain_polls=1)
        sleeps = []
        def cancellation(seconds):
            sleeps.append(seconds)
            if len(sleeps) == 1:
                coordinator.stop_file.write_text("cancel while draining")
            elif len(sleeps) == 2:
                coordinator.stop_file.unlink()
        media.on_sleep = cancellation
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "coordinator stop"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertFalse(coordinator.stop_file.exists())
        self.assertEqual(len(media.launches), 1)
        self.assertEqual(media.completed, ["item0"])
        self.assertNotIn(30, sleeps) # An observed cancel cannot become wait-for-game/automatic resume.
        self.assertFalse(Path(media.launches[0][-1]).exists())

    def test_user_stop_while_waiting_for_game_prevents_resume(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator)
        def stop_during_wait(seconds):
            if seconds == 30:
                media.external_stop.write_text("user stopped during game")
        media.on_sleep = stop_during_wait
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "child stop flag"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(len(media.launches), 1)
        self.assertEqual(media.external_stop.read_text(), "user stopped during game")

    def test_replaced_private_marker_is_preserved_and_does_not_auto_resume(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator)
        def replace_marker(_seconds):
            Path(media.launches[0][-1]).write_text("foreign stop request")
        media.on_sleep = replace_marker
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "replaced; preserved"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(len(media.launches), 1)
        self.assertEqual(Path(media.launches[0][-1]).read_text(), "foreign stop request")

    def test_child_failure_during_game_pause_is_not_retried_as_a_pause(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator, boundary_code=1)
        with media.installed(), self.assertRaisesRegex(RuntimeError, "textures exited 1"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(len(media.launches), 1)
        self.assertFalse(Path(media.launches[0][-1]).exists())

    def test_unrequested_boundary_exit_is_not_automatically_retried(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator)
        media.on_first_poll = lambda: Path(media.launches[0][-1]).write_text("external boundary request")
        with media.installed(), self.assertRaisesRegex(pipeline.Paused, "without a game-pause request"):
            coordinator.command("textures", media.args, child_stop=media.external_stop)
        self.assertEqual(len(media.launches), 1)
        self.assertEqual(Path(media.launches[0][-1]).read_text(), "external boundary request")

    def test_interrupt_during_monitoring_drains_owned_child_and_never_auto_resumes(self):
        for location in ("process-inventory", "sleep"):
            with self.subTest(location=location):
                coordinator = pipeline.Coordinator(self.source, self.backup)
                media = FakeMedia(coordinator)
                interrupted = []
                def interrupt_once():
                    if not interrupted:
                        interrupted.append(True)
                        raise KeyboardInterrupt
                if location == "sleep":
                    media.on_sleep = lambda _seconds: interrupt_once()
                else:
                    workers = media.workers
                    def interrupted_inventory():
                        if media.launches:
                            interrupt_once()
                        return workers()
                    media.workers = interrupted_inventory
                with media.installed(), self.assertRaisesRegex(pipeline.Paused, "coordinator interrupted"):
                    coordinator.command("textures", media.args, child_stop=media.external_stop)
                self.assertEqual(len(media.launches), 1)
                self.assertIn(("boundary-exit", 1, 2), media.events)
                self.assertFalse(Path(media.launches[0][-1]).exists())

    def test_private_marker_not_created_by_this_owner_is_never_removed(self):
        marker = pipeline.OwnedChildStop(self.source)
        marker.path.write_text(marker.content)
        marker.clear()
        self.assertTrue(marker.path.exists())
        with self.assertRaisesRegex(pipeline.Paused, "replaced; preserved"):
            marker.request()
        self.assertEqual(marker.path.read_text(), marker.content)

    def test_cooperative_child_requires_matching_explicit_stop_cli(self):
        coordinator = pipeline.Coordinator(self.source, self.backup)
        media = FakeMedia(coordinator)
        for args in (["fixture-python", "worker"], ["worker", "--stop-file", "some-other-stop"]):
            with media.installed(), self.assertRaisesRegex(ValueError, "cooperative child"):
                coordinator.command("textures", args, child_stop=media.external_stop)
        self.assertEqual(media.launches, [])


if __name__ == "__main__":
    unittest.main()
