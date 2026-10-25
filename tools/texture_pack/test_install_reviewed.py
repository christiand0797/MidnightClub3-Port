import json
import os
from types import SimpleNamespace
from pathlib import Path
import unittest
from unittest import mock

from PIL import Image

import install_reviewed as planner
import reviewed_fixes as fixes
import test_reviewed_fixes as fixture_module


class InstallationWorkersTests(unittest.TestCase):
    def test_installed_games_are_selected_without_blocking_steam_or_unrelated_python(self):
        rows = [
            {"ProcessId": os.getpid()+100, "Name": "BlackOps3.exe", "CommandLine": None},
            {"ProcessId": os.getpid()+101, "Name": "mc3.exe", "CommandLine": None},
            {"ProcessId": os.getpid()+102, "Name": "steam.exe", "CommandLine": None},
            {"ProcessId": os.getpid()+103, "Name": "python.exe", "CommandLine": "unrelated.py"},
            {"ProcessId": os.getpid(), "Name": "python.exe", "CommandLine": "install_reviewed.py"},
        ]
        with mock.patch.object(planner.os, "name", "nt"), mock.patch.object(
                planner.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(rows))) as run:
            self.assertEqual(planner.installation_workers(), rows[:2])
        self.assertIn("'BlackOps3.exe'", run.call_args.args[0][-1])


class InstallPlanTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixture_module.ReviewedFixTests("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root, self.textures = self.fixture.root, self.fixture.textures
        self.stage = self.root/self.fixture.stage
        # The shared audit fixture starts with one installed test PNG/ledger. Planning starts from the real
        # first-adoption situation: original outputs and no current correction ledger, entirely in temp files.
        original = self.textures/self.fixture.entry["base_archive"].removeprefix("textures/")
        (self.textures/"replace"/self.fixture.name).write_bytes(original.read_bytes())
        (self.textures/"reviewed_fixes.json").unlink()
        self.names = [self.fixture.plan["jobs"]["s"]["panels"][side]["name"] for side in ("left", "right")]
        self.proposal = self.root/".local/proposal-one"
        for module, filename in ((planner, "install_reviewed.py"), (fixes, "reviewed_fixes.py"),
                                 (fixes.cpu_x4plus, "cpu_x4plus.py")):
            path = self.root/"tools/texture_pack"/filename
            path.write_bytes(Path(module.__file__).read_bytes())
            patch = mock.patch.object(module, "__file__", str(path))
            patch.start(); self.addCleanup(patch.stop)
        for city in "adst":
            folder = self.stage/"joined-source"; folder.mkdir(exist_ok=True)
            Image.new("RGB", (506, 256), (20, 30, 40)).save(folder/f"garage_{city}.png")
        review = self.stage/"boundary-review/s"; review.mkdir(parents=True)
        Image.new("RGB", (64, 96), (30, 80, 90)).save(review/"source_active_candidate.png")
        self.native_path = self.root/".local/native-review.json"
        for name, color in (("native-before.png", (20, 30, 40)), ("native-after.png", (30, 40, 50))):
            Image.new("RGB", (64, 64), color).save(self.root/".local"/name)
        self.native = {"format": "mc3-native-texture-review-v1", "decision": "accepted", "reviewer": "fixture parent",
            "before_capture": ".local/native-before.png", "after_capture": ".local/native-after.png", "entries": {}}
        for name in self.names:
            panel = next(p for p in self.fixture.plan["jobs"]["s"]["panels"].values() if p["name"] == name)
            self.native["entries"][name] = {"source_sha256": panel["source_sha256"],
                "before_sha256": panel["base_replacement_sha256"],
                "after_sha256": fixes.upscale.file_digest(self.stage/"neural-candidates"/name)}
        self.native_path.write_text(json.dumps(self.native))
        self.before = {name: (self.textures/"replace"/name).read_bytes() for name in self.names}
        self.pass_bytes = (self.textures/"upscale_passes.json").read_bytes()

    def build(self, accepted=False, workers=None):
        return planner.build_plan(self.proposal, self.stage, "08"*20, native_proof=self.native_path if accepted else None,
            root=self.root, textures=self.textures, model=self.fixture.model, pass_id=self.fixture.pass_id,
            workers=[] if workers is None else workers)

    def check(self):
        return planner.validate_plan(self.proposal/"install-plan.json", root=self.root, workers=[])

    def unchanged(self):
        self.assertFalse((self.textures/"reviewed_fixes.json").exists())
        self.assertEqual((self.textures/"upscale_passes.json").read_bytes(), self.pass_bytes)
        for name in self.names:
            self.assertEqual((self.textures/"replace"/name).read_bytes(), self.before[name])

    def test_pending_native_evidence_stages_complete_two_panel_chain_without_installing(self):
        plan, readiness = self.build()
        self.assertTrue(readiness["candidate_chain_complete"])
        self.assertFalse(readiness["native_review_accepted"])
        self.assertFalse(readiness["install_ready"])
        self.assertFalse(readiness["installed"])
        self.assertEqual({item["name"] for item in plan["operations"]}, set(self.names))
        self.assertEqual(plan["operation"], "dry-run-only")
        self.unchanged()

    def test_accepted_native_projection_verifies_existing_auditor_with_independent_archives(self):
        buddy = self.root/"qa-readonly-hardlink.png"
        os.link(self.textures/"replace"/self.names[0], buddy)
        plan, ready = self.build(accepted=True)
        self.assertTrue(ready["native_review_accepted"])
        self.assertTrue(ready["install_ready"])
        for operation in plan["operations"]:
            archived = self.root/operation["archive_source"]
            self.assertEqual(archived.stat().st_nlink, 1)
            self.assertFalse(os.path.samefile(archived, self.root/operation["target"]))
            self.assertFalse((self.root/operation["archive_target"]).exists())
        self.assertEqual(buddy.read_bytes(), self.before[self.names[0]])
        self.unchanged()

    def test_live_coordinator_or_game_blocks_readiness_without_stopping_it(self):
        workers = [{"ProcessId": 123, "Name": "python.exe", "CommandLine": "overnight_pipeline.py --run"}]
        _plan, ready = self.build(accepted=True, workers=workers)
        self.assertFalse(ready["install_ready"])
        self.assertEqual(ready["blocking_workers"], workers)
        self.unchanged()

    def test_new_active_bytes_invalidate_plan_and_are_preserved(self):
        self.build(accepted=True)
        path = self.textures/"replace"/self.names[0]
        path.write_bytes(b"external user replacement")
        with self.assertRaisesRegex(ValueError, "active PNG changed"):
            self.check()
        self.assertEqual(path.read_bytes(), b"external user replacement")
        self.assertEqual((self.textures/"replace"/self.names[1]).read_bytes(), self.before[self.names[1]])

    def test_changed_native_capture_or_candidate_dependency_fails_closed(self):
        self.build(accepted=True)
        for path in (self.root/".local/native-after.png", self.stage/"neural-candidates"/self.names[0]):
            before = path.read_bytes()
            path.write_bytes(before+b"changed")
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.check()
            path.write_bytes(before)
        self.unchanged()

    def test_future_archive_is_never_overwritten_and_new_canonical_ledger_invalidates_plan(self):
        plan, _ready = self.build(accepted=True)
        target = self.root/plan["operations"][0]["archive_target"]
        target.parent.mkdir(parents=True)
        target.write_bytes(b"unrelated archive")
        with self.assertRaisesRegex(ValueError, "archive path already exists"):
            self.check()
        self.assertEqual(target.read_bytes(), b"unrelated archive")
        target.unlink()
        (self.textures/"reviewed_fixes.json").write_text("unrelated ledger")
        with self.assertRaisesRegex(ValueError, "canonical reviewed ledger appeared"):
            self.check()
        self.assertEqual((self.textures/"reviewed_fixes.json").read_text(), "unrelated ledger")

    def test_archive_hardlink_substitution_is_rejected(self):
        plan, _ready = self.build()
        operation = plan["operations"][0]
        path = self.root/operation["archive_source"]
        path.unlink()
        os.link(self.root/operation["target"], path)
        with self.assertRaisesRegex(ValueError, "independent copy"):
            self.check()
        self.unchanged()

    def test_existing_or_escaping_stage_and_unexpected_projection_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "escapes"):
            planner.build_plan(self.root.parent/"outside-proposal", self.stage, "08"*20,
                root=self.root, textures=self.textures, model=self.fixture.model, pass_id=self.fixture.pass_id, workers=[])
        self.build()
        with self.assertRaisesRegex(ValueError, "NEW isolated"):
            self.build()
        path = self.proposal/"install-plan.json"
        plan = json.loads(path.read_text())
        plan["future_bindings"]["unrelated.png"] = ".local/native-before.png"
        path.write_text(json.dumps(plan))
        with self.assertRaises(ValueError):
            self.check()
        self.unchanged()

    def test_accepted_wrong_before_after_native_evidence_is_not_adoption_ready(self):
        self.native["entries"][self.names[0]]["after_sha256"] = "0"*64
        self.native_path.write_text(json.dumps(self.native))
        with self.assertRaisesRegex(ValueError, "exact source/before/after"):
            self.build(accepted=True)
        self.unchanged()

    def test_old_anchor_must_match_historical_named_pass(self):
        path = self.textures/"replace"/self.names[0]
        path.write_bytes(b"unexpected old active bytes")
        with self.assertRaisesRegex(ValueError, "original named-pass anchor"):
            self.build()
        self.assertEqual(path.read_bytes(), b"unexpected old active bytes")

    def test_an_unrelated_original_pack_failure_prevents_readiness(self):
        self.build(accepted=True)
        unrelated = self.textures/"replace"/"0000000000000002.png"
        unrelated.unlink()
        with self.assertRaisesRegex(ValueError, "complete original pack"):
            self.check()
        self.assertFalse(unrelated.exists())
        self.unchanged()


if __name__ == "__main__":
    unittest.main()
