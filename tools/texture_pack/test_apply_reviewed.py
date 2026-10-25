import json
import os
from pathlib import Path
import sys
import unittest
from unittest import mock
import uuid

import apply_reviewed as transaction
import test_install_reviewed as plan_fixture


class SimulatedCrash(BaseException):
    """An interruption that deliberately bypasses ordinary exception rollback."""


@unittest.skipUnless(os.name == "nt", "Windows directory-entry publication/byte-lock semantics")
class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.fixture = plan_fixture.InstallPlanTests("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root, self.textures, self.names = self.fixture.root, self.fixture.textures, self.fixture.names
        patch = mock.patch.object(transaction.planner, "installation_workers", return_value=[])
        patch.start(); self.addCleanup(patch.stop)
        self.counter = 0

    def proposal(self, accepted=True):
        self.counter += 1
        self.fixture.proposal = self.root/".local"/f"proposal-{self.counter}"
        plan, ready = self.fixture.build(accepted=accepted)
        self.assertEqual(ready["native_review_accepted"], accepted)
        return self.fixture.proposal/"install-plan.json", plan

    def apply(self, path, fault=None):
        directory = self.root/".local"/("transaction-"+uuid.uuid4().hex)
        return transaction.apply(path, directory, root=self.root, fault=fault), directory

    def journal_for(self):
        paths = list((self.root/".local").glob("transaction-*/journal.json"))
        return max(paths, key=lambda path: path.stat().st_mtime_ns)

    def assert_old(self):
        self.fixture.unchanged()

    def marker(self):
        return self.root/transaction.ACTIVE_MARKER

    def test_success_audits_exact_pair_and_rollback_keeps_qa_hardlink_bytes(self):
        buddy = self.root/"qa-original.png"
        os.link(self.textures/"replace"/self.names[0], buddy)
        old = buddy.read_bytes()
        path, plan = self.proposal()
        result, directory = self.apply(path)
        self.assertEqual(result["status"], "committed")
        self.assertEqual(result["eligible"], 8)
        self.assertEqual(buddy.read_bytes(), old)
        self.assertNotEqual((self.textures/"replace"/self.names[0]).read_bytes(), old)
        for operation in plan["operations"]:
            self.assertEqual((self.root/operation["archive_target"]).stat().st_nlink, 1)
            self.assertEqual(transaction.upscale.file_digest(self.root/operation["target"]), operation["expected_after_sha256"])
        report = json.loads((directory/"post-apply-audit.json").read_text())
        self.assertTrue(report["complete"])
        self.assertEqual(report["counts"]["reviewed_fixes"], 2)
        recovered = transaction.recover(directory/"journal.json", root=self.root)
        self.assertFalse(recovered["conflicts"])
        self.assertEqual(set(recovered["restored"]), set(self.names))
        self.assertEqual(buddy.read_bytes(), old)
        self.assert_old()
        for operation in plan["operations"]:
            self.assertTrue((self.root/operation["archive_target"]).exists())
        self.assertTrue(list(directory.glob("retired-ledger-*.json")))
        self.assertFalse(self.marker().exists())
        self.assertEqual(transaction.recover(directory/"journal.json", root=self.root), recovered)

    def test_ordinary_failures_before_and_after_each_png_swap_roll_back(self):
        for phase in ("before", "after"):
            for name in self.names:
                with self.subTest(phase=phase, name=name):
                    path, _plan = self.proposal()
                    def fail(point, _journal):
                        if point == phase+"-png-swap:"+name:
                            raise RuntimeError("forced ordinary failure")
                    with self.assertRaises(transaction.TransactionFailure):
                        self.apply(path, fail)
                    journal = json.loads(self.journal_for().read_text())
                    self.assertEqual(journal["status"], "rolled-back")
                    self.assert_old()

    def test_failure_after_ledger_publication_retires_only_own_exact_ledger(self):
        path, _plan = self.proposal()
        def fail(point, _journal):
            if point == "after-ledger-publish":
                raise RuntimeError("forced ledger-boundary failure")
        with self.assertRaises(transaction.TransactionFailure):
            self.apply(path, fail)
        journal_path = self.journal_for()
        journal = json.loads(journal_path.read_text())
        self.assertEqual(journal["status"], "rolled-back")
        retired = list(journal_path.parent.glob("retired-ledger-*.json"))
        self.assertEqual(len(retired), 1)
        self.assertEqual(transaction.upscale.file_digest(retired[0]), journal["intent"]["ledger_sha256"])
        self.assert_old()

    def test_crash_after_swap_with_intent_but_no_done_recovers_from_actual_hash(self):
        path, _plan = self.proposal()
        name = self.names[0]
        def crash(point, _journal):
            if point == "after-png-swap:"+name:
                raise SimulatedCrash()
        with self.assertRaises(SimulatedCrash):
            self.apply(path, crash)
        journal_path = self.journal_for()
        journal = json.loads(journal_path.read_text())
        events = [event["phase"] for event in journal["events"] if event["action"] == "png-swap" and event["name"] == name]
        self.assertEqual(events, ["intent"])
        self.assertNotEqual((self.textures/"replace"/name).read_bytes(), self.fixture.before[name])
        result = transaction.recover(journal_path, root=self.root)
        self.assertFalse(result["conflicts"])
        self.assert_old()

    def test_crash_before_swap_and_after_ledger_publish_are_idempotently_recoverable(self):
        for point in ("before-png-swap:"+self.names[0], "after-ledger-publish"):
            with self.subTest(point=point):
                path, _plan = self.proposal()
                def crash(actual, _journal):
                    if actual == point:
                        raise SimulatedCrash()
                with self.assertRaises(SimulatedCrash):
                    self.apply(path, crash)
                transaction.recover(self.journal_for(), root=self.root)
                self.assert_old()

    def test_crash_during_recovery_with_restore_intent_is_resumable(self):
        path, _plan = self.proposal()
        _result, directory = self.apply(path)
        name = self.names[-1]  # Recovery walks the pair in reverse.
        def crash(point, _journal):
            if point == "after-png-restore:"+name:
                raise SimulatedCrash()
        with self.assertRaises(SimulatedCrash):
            transaction.recover(directory/"journal.json", root=self.root, fault=crash)
        result = transaction.recover(directory/"journal.json", root=self.root)
        self.assertFalse(result["conflicts"])
        self.assert_old()

    def test_unrelated_external_png_change_is_preserved_and_reported(self):
        path, _plan = self.proposal()
        name = self.names[0]
        def change(point, _journal):
            if point == "after-png-swap:"+name:
                (self.textures/"replace"/name).write_bytes(b"external user texture")
                raise RuntimeError("external edit during transaction")
        with self.assertRaises(transaction.TransactionFailure):
            self.apply(path, change)
        journal = json.loads(self.journal_for().read_text())
        self.assertEqual(journal["status"], "recovery-conflicts")
        self.assertTrue(self.marker().exists())
        self.assertEqual((self.textures/"replace"/name).read_bytes(), b"external user texture")
        self.assertEqual((self.textures/"replace"/self.names[1]).read_bytes(), self.fixture.before[self.names[1]])
        self.assertFalse((self.textures/"reviewed_fixes.json").exists())

    def test_unrelated_external_ledger_is_preserved_while_pngs_roll_back(self):
        path, _plan = self.proposal()
        def change(point, _journal):
            if point == "after-ledger-publish":
                (self.textures/"reviewed_fixes.json").write_bytes(b"external ledger")
                raise RuntimeError("external ledger edit")
        with self.assertRaises(transaction.TransactionFailure):
            self.apply(path, change)
        journal = json.loads(self.journal_for().read_text())
        self.assertEqual(journal["status"], "recovery-conflicts")
        self.assertTrue(self.marker().exists())
        self.assertEqual((self.textures/"reviewed_fixes.json").read_bytes(), b"external ledger")
        for name in self.names:
            self.assertEqual((self.textures/"replace"/name).read_bytes(), self.fixture.before[name])

    def test_bad_full_post_apply_audit_triggers_conditional_rollback(self):
        path, _plan = self.proposal()
        with mock.patch.object(transaction.fixes, "audit", return_value={"complete": False}):
            with self.assertRaises(transaction.TransactionFailure):
                self.apply(path)
        self.assert_old()

    def test_pending_proof_and_live_workers_refuse_before_transaction_creation(self):
        path, _plan = self.proposal(accepted=False)
        with self.assertRaisesRegex(ValueError, "accepted native proof"):
            self.apply(path)
        self.assertFalse(list((self.root/".local").glob("transaction-*/journal.json")))
        path, _plan = self.proposal()
        for name in ("mc3.exe", "FortniteClient-Win64-Shipping.exe", "BlackOps3.exe"):
            with self.subTest(game=name), mock.patch.object(
                    transaction.planner, "installation_workers", return_value=[{"Name": name, "ProcessId": 77}]):
                with self.assertRaisesRegex(RuntimeError, "workers must close"):
                    self.apply(path)
        self.assertFalse(list((self.root/".local").glob("transaction-*/journal.json")))
        self.assert_old()

    def test_changed_proof_or_existing_archive_is_refused_without_overwrite(self):
        path, plan = self.proposal()
        candidate = self.root/plan["operations"][0]["candidate_source"]
        original = candidate.read_bytes()
        candidate.write_bytes(original+b"external proof change")
        with self.assertRaises(ValueError):
            self.apply(path)
        self.assertFalse(list((self.root/".local").glob("transaction-*/journal.json")))
        candidate.write_bytes(original)
        archive = self.root/plan["operations"][0]["archive_target"]
        archive.parent.mkdir(parents=True)
        archive.write_bytes(b"existing unrelated archive")
        with self.assertRaisesRegex(ValueError, "archive path already exists"):
            self.apply(path)
        self.assertEqual(archive.read_bytes(), b"existing unrelated archive")
        self.assert_old()

    def test_new_game_at_mutation_boundary_defers_rollback_until_closed(self):
        path, _plan = self.proposal()
        running = []
        def start_game(point, _journal):
            if point == "after-png-swap:"+self.names[0]:
                running.append({"Name": "mc3.exe", "ProcessId": 77})
        with mock.patch.object(transaction.planner, "installation_workers", side_effect=lambda: running):
            with self.assertRaisesRegex(transaction.TransactionFailure, "recovery remains pending"):
                self.apply(path, start_game)
        journal_path = self.journal_for()
        self.assertEqual(json.loads(journal_path.read_text())["status"], "recovery-pending")
        transaction.recover(journal_path, root=self.root)
        self.assert_old()

    def test_coordinator_or_install_byte_lock_excludes_transaction(self):
        path, _plan = self.proposal()
        sys.path.insert(0, str(transaction.ROOT/"tools/maintenance"))
        import overnight_pipeline
        for relative in (transaction.COORDINATOR_LOCK, transaction.INSTALL_LOCK):
            with self.subTest(lock=relative), overnight_pipeline.exclusive_lock(self.root/relative):
                with self.assertRaises(RuntimeError):
                    self.apply(path)
        self.assert_old()

    def test_immutable_intent_tampering_refuses_recovery_without_touching_targets(self):
        path, _plan = self.proposal()
        def crash(point, _journal):
            if point == "after-png-swap:"+self.names[0]:
                raise SimulatedCrash()
        with self.assertRaises(SimulatedCrash):
            self.apply(path, crash)
        journal_path = self.journal_for()
        journal = json.loads(journal_path.read_text())
        current = (self.textures/"replace"/self.names[0]).read_bytes()
        journal["intent"]["plan"]["operations"][0]["expected_before_sha256"] = "0"*64
        journal_path.write_text(json.dumps(journal))
        with self.assertRaisesRegex(ValueError, "immutable intent"):
            transaction.recover(journal_path, root=self.root)
        self.assertEqual((self.textures/"replace"/self.names[0]).read_bytes(), current)

    def test_prepared_temporary_tampering_never_publishes_foreign_bytes(self):
        for kind, point in (("archive", "before-archive-publish:"+self.names[0]),
                            ("png", "before-png-swap:"+self.names[0]), ("ledger", "before-ledger-publish")):
            with self.subTest(kind=kind):
                path, _plan = self.proposal()
                def corrupt(actual, journal):
                    if actual == point:
                        temporary = self.root/journal.value["events"][-1]["temporary"]
                        temporary.write_bytes(b"foreign prepared temporary")
                with self.assertRaises(transaction.TransactionFailure):
                    self.apply(path, corrupt)
                journal = json.loads(self.journal_for().read_text())
                self.assertEqual(journal["status"], "rolled-back")
                self.assertFalse(journal["recovery"]["conflicts"])
                self.assertFalse(self.marker().exists())
                self.assert_old()

    def test_prepared_temporary_hardlink_substitution_is_refused(self):
        path, plan = self.proposal()
        def link(point, journal):
            if point == "before-png-swap:"+self.names[0]:
                temporary = self.root/journal.value["events"][-1]["temporary"]
                temporary.unlink()
                os.link(self.root/plan["operations"][0]["candidate_source"], temporary)
        with self.assertRaises(transaction.TransactionFailure):
            self.apply(path, link)
        self.assert_old()

    def test_bad_rollback_temporary_is_preserved_without_overwriting_current_png(self):
        path, plan = self.proposal()
        _result, directory = self.apply(path)
        name = self.names[-1]
        def corrupt(point, journal):
            if point == "before-png-restore:"+name:
                (self.root/journal.value["events"][-1]["temporary"]).write_bytes(b"bad rollback bytes")
        with self.assertRaisesRegex(ValueError, "temporary bytes/link"):
            transaction.recover(directory/"journal.json", root=self.root, fault=corrupt)
        operation = next(op for op in plan["operations"] if op["name"] == name)
        self.assertEqual(transaction.upscale.file_digest(self.root/operation["target"]), operation["expected_after_sha256"])
        self.assertTrue(self.marker().exists())  # Explicit rollback re-registers its crash gate before mutation.
        transaction.recover(directory/"journal.json", root=self.root)
        self.assert_old()

    def test_marker_registration_crash_before_or_after_atomic_create_is_recoverable(self):
        for point in ("before-marker-create", "after-marker-create"):
            with self.subTest(point=point):
                path, plan = self.proposal()
                def crash(actual, _journal):
                    if actual == point:
                        raise SimulatedCrash()
                with self.assertRaises(SimulatedCrash):
                    self.apply(path, crash)
                journal = json.loads(self.journal_for().read_text())
                if point == "after-marker-create":
                    self.assertEqual(transaction.upscale.file_digest(self.marker()), journal["active_marker_sha256"])
                    marker = json.loads(self.marker().read_text())
                    self.assertEqual(marker["intent_sha256"], journal["intent_sha256"])
                    self.assertEqual(marker["nonce"], journal["intent"]["marker_nonce"])
                else:
                    self.assertFalse(self.marker().exists())
                for operation in plan["operations"]:
                    self.assertFalse((self.root/operation["archive_target"]).exists())
                transaction.recover(self.journal_for(), root=self.root)
                self.assertFalse(self.marker().exists())
                self.assert_old()

    def test_committed_or_rolled_back_status_crash_still_resolves_own_marker(self):
        for point in ("after-commit-status", "after-rollback-status", "after-marker-retire"):
            with self.subTest(point=point):
                path, _plan = self.proposal()
                def crash(actual, _journal):
                    if actual == point:
                        raise SimulatedCrash()
                if point == "after-rollback-status":
                    # Reach rollback via a real interrupted forward transaction, then interrupt terminal cleanup.
                    def interrupted(actual, _journal):
                        if actual == "after-png-swap:"+self.names[0]: raise SimulatedCrash()
                    with self.assertRaises(SimulatedCrash): self.apply(path, interrupted)
                    with self.assertRaises(SimulatedCrash):
                        transaction.recover(self.journal_for(), root=self.root, fault=crash)
                else:
                    with self.assertRaises(SimulatedCrash): self.apply(path, crash)
                transaction.recover(self.journal_for(), root=self.root)
                self.assertFalse(self.marker().exists())
                self.assert_old()

    def test_foreign_marker_blocks_new_apply_and_recovery_without_mutation(self):
        path, _plan = self.proposal()
        self.marker().parent.mkdir(parents=True, exist_ok=True)
        self.marker().write_bytes(b"foreign active owner")
        with self.assertRaisesRegex(RuntimeError, "existing active"):
            self.apply(path)
        self.assertEqual(self.marker().read_bytes(), b"foreign active owner")
        self.marker().unlink()  # Test-only removal of the fixture's own foreign-marker simulation.
        def crash(point, _journal):
            if point == "after-png-swap:"+self.names[0]: raise SimulatedCrash()
        with self.assertRaises(SimulatedCrash): self.apply(path, crash)
        current = (self.textures/"replace"/self.names[0]).read_bytes()
        self.marker().write_bytes(b"different external active owner")
        with self.assertRaisesRegex(RuntimeError, "foreign/changed"):
            transaction.recover(self.journal_for(), root=self.root)
        self.assertEqual(self.marker().read_bytes(), b"different external active owner")
        self.assertEqual((self.textures/"replace"/self.names[0]).read_bytes(), current)

    def test_foreign_marker_race_during_create_is_not_overwritten(self):
        path, plan = self.proposal()
        def foreign(point, _journal):
            if point == "before-marker-create":
                self.marker().write_bytes(b"external marker won atomic create")
        with self.assertRaisesRegex(transaction.TransactionFailure, "recovery remains pending"):
            self.apply(path, foreign)
        self.assertEqual(self.marker().read_bytes(), b"external marker won atomic create")
        for operation in plan["operations"]:
            self.assertFalse((self.root/operation["archive_target"]).exists())
        self.assert_old()

    def test_foreign_marker_at_commit_cleanup_is_preserved_and_pack_not_rolled_back(self):
        path, plan = self.proposal()
        def foreign(point, _journal):
            if point == "before-marker-retire":
                self.marker().write_bytes(b"external marker during cleanup")
        with self.assertRaisesRegex(transaction.TransactionFailure, "recovery remains pending"):
            self.apply(path, foreign)
        self.assertEqual(self.marker().read_bytes(), b"external marker during cleanup")
        for operation in plan["operations"]:
            self.assertEqual(transaction.upscale.file_digest(self.root/operation["target"]), operation["expected_after_sha256"])
        self.assertTrue((self.textures/"reviewed_fixes.json").exists())


if __name__ == "__main__":
    unittest.main()
