"""Exercise draft ownership and upload collision checks without contacting GitHub."""
import argparse
import importlib.util
import json
import os
import stat
import tempfile
import unittest
from types import SimpleNamespace
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("publish_portable", Path(__file__).with_name("publish_portable.py"))
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class DraftSafetyTests(unittest.TestCase):
    def uploader(self, control):
        args = argparse.Namespace(control=control, repo="christiand0797/MidnightClub3-Port", tag="vfixture",
                                  replace_owned_checksums=False)
        result = publisher.DraftUploader(args)
        result.state["release_id"] = 10
        result.account = "christiand0797"
        return result

    def setUp(self):
        root = Path(__file__).resolve().parents[2] / "out" / "publish-unit-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temp.cleanup)
        self.upload = self.uploader(Path(self.temp.name))

    def test_published_or_unowned_release_is_rejected(self):
        valid = {"tag_name": "vfixture", "draft": True, "prerelease": True, "body": self.upload.marker}
        with patch.object(publisher, "gh_json", return_value=valid):
            self.assertEqual(self.upload.owned_draft(), valid)
        for change in [{"draft": False}, {"body": "someone else's release"}, {"tag_name": "different"}]:
            with self.subTest(change=change), patch.object(publisher, "gh_json", return_value=valid | change):
                with self.assertRaises(RuntimeError):
                    self.upload.owned_draft()

    def test_server_digest_and_size_must_both_match(self):
        local = {"size": 123, "sha256": "a" * 64}
        remote = {"state": "uploaded", "size": 123, "digest": "sha256:" + "a" * 64}
        self.assertTrue(self.upload.remote_matches(remote, local))
        self.assertFalse(self.upload.remote_matches(remote | {"size": 124}, local))
        self.assertFalse(self.upload.remote_matches(remote | {"digest": "sha256:" + "b" * 64}, local))

    def test_target_update_keeps_the_fixed_draft_tag(self):
        notes = Path(self.temp.name) / "notes.md"
        notes.write_text("Verified final build", encoding="utf-8")
        self.upload.args.notes = notes
        self.upload.args.target = "b" * 40
        self.upload.args.update_owned_target = True
        current = {"id": 10, "tag_name": "vfixture", "draft": True, "prerelease": True,
                   "body": self.upload.marker, "target_commitish": "a" * 40,
                   "html_url": "https://example.test/draft"}

        def api(endpoint, *, method="GET", body=None):
            if method == "PATCH":
                self.assertEqual(body["tag_name"], "vfixture")
                current.update(body)
            return dict(current)

        with patch.object(publisher, "gh_json", side_effect=api):
            self.upload.ensure_draft()
        self.assertEqual(current["tag_name"], "vfixture")
        self.assertEqual(current["target_commitish"], "b" * 40)

    def test_control_directory_rejects_a_second_uploader(self):
        with publisher.ControlLock(Path(self.temp.name)):
            with self.assertRaises(RuntimeError):
                publisher.ControlLock(Path(self.temp.name))

    def test_state_publish_retries_transient_windows_sharing_failure(self):
        original = Path.replace
        calls = []

        def replace(path, destination):
            calls.append(path)
            if len(calls) < 3:
                raise PermissionError("file is being scanned")
            return original(path, destination)

        self.upload.state["retry_proof"] = True
        with patch.object(Path, "replace", replace), patch.object(publisher.time, "sleep") as sleep:
            self.upload.save()
        self.assertEqual(len(calls), 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertIn('"retry_proof": true', self.upload.state_path.read_text(encoding="utf-8"))

    def test_old_empty_asset_is_not_adopted_as_this_attempt(self):
        local = {"name": "part.mc3c", "size": 123, "sha256": "a" * 64}
        remote = {"id": 20, "name": local["name"], "state": "starter", "size": 0,
                  "created_at": "2026-01-01T00:00:00Z", "uploader": {"login": self.upload.account}}
        self.upload.state["assets"][local["name"]] = {"attempt_started_utc": "2026-01-02T00:00:00+00:00"}
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", return_value=[remote]), patch.object(publisher, "gh_json") as api:
            with self.assertRaisesRegex(RuntimeError, "refusing to clobber"):
                self.upload.upload_one(local)
            api.assert_not_called()

    def test_owned_empty_failed_upload_can_restart_at_server_second_precision(self):
        path = Path(self.temp.name) / "part.mc3c"
        path.write_bytes(b"a" * 123)
        local = {"name": path.name, "path": str(path), "size": 123, "sha256": publisher.sha256(path)}
        starter = {"id": 20, "name": path.name, "state": "starter", "size": 0,
                   "created_at": "2026-01-02T00:00:00Z", "uploader": {"login": self.upload.account}}
        complete = {"id": 21, "name": path.name, "state": "uploaded", "size": 123,
                    "digest": "sha256:" + local["sha256"], "browser_download_url": "https://example.test/part.mc3c"}
        self.upload.state["assets"][path.name] = {"attempt_started_utc": "2026-01-02T00:00:00.123456+00:00"}
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", side_effect=[[starter], [complete]]), patch.object(publisher, "gh_json") as api, patch.object(publisher.subprocess, "run", return_value=SimpleNamespace(returncode=0, stderr="")):
            self.upload.upload_one(local)
            api.assert_called_once_with(f"{self.upload.prefix}/releases/assets/20", method="DELETE")

    def test_mismatched_uploaded_asset_is_never_clobbered(self):
        local = {"name": "content-manifest.tsv", "size": 123, "sha256": "a" * 64}
        remote = {"id": 20, "name": local["name"], "state": "uploaded", "size": 123, "digest": "sha256:" + "b" * 64}
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", return_value=[remote]), patch.object(publisher, "gh_json") as api:
            with self.assertRaisesRegex(RuntimeError, "refusing to clobber"):
                self.upload.upload_one(local)
            api.assert_not_called()

    def test_matching_remote_asset_is_skipped_without_local_read_or_upload(self):
        local = {"name": "part.mc3c", "size": 123, "sha256": "a" * 64}
        remote = {"id": 20, "name": local["name"], "state": "uploaded", "size": 123,
                  "digest": "sha256:" + "a" * 64, "browser_download_url": "https://example.test/part.mc3c"}
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", return_value=[remote]), patch.object(publisher.subprocess, "run") as run:
            self.upload.upload_one(local)
            run.assert_not_called()
        self.assertEqual(self.upload.state["assets"][local["name"]]["status"], "verified-existing")


class AuxiliaryAssetTests(unittest.TestCase):
    def setUp(self):
        self.workspace = Path(__file__).resolve().parents[2] / "out" / "publish-unit-tests"
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=self.workspace)
        self.root = Path(self.temp.name).resolve()
        self.addCleanup(self.cleanup)
        self.content, self.input = self.root / "content", self.root / "proof-input"
        self.content.mkdir(); self.input.mkdir()
        chunk = self.content / "mc3-content-fixture.001.mc3c"
        chunk.write_bytes(b"synthetic tiny content")
        digest = publisher.sha256(chunk)
        (self.content / "content-manifest.tsv").write_text(
            f"MC3-CONTENT\t1\narchive\t{chunk.stat().st_size}\t{digest}\nchunk\t{chunk.name}\t{chunk.stat().st_size}\t{digest}\n")
        (self.content / "content-files.tsv").write_text("MC3-CONTENT-FILES\t1\n")
        self.proof = self.input / "reviewed-proof.zip"
        self.proof.write_bytes(b"pinned proof archive fixture")
        self.request = self.input / "request.json"
        self.row = {"name": self.proof.name, "path": self.proof.name, "size": self.proof.stat().st_size,
                    "sha256": publisher.sha256(self.proof)}
        self.write_request([self.row])
        self.args = argparse.Namespace(control=self.root / "control", repo="christiand0797/MidnightClub3-Port",
            tag="vfixture", content=self.content, runtime_zip=None, runtime_sha256=None,
            auxiliary_plan=self.request, replace_owned_checksums=False)
        self.upload = publisher.DraftUploader(self.args)
        self.upload.account = "fixture-account"

    def cleanup(self):
        # TemporaryDirectory recursively removes only this resolved, uniquely owned test workspace child.
        if self.root.parent != self.workspace.resolve() or self.root == self.workspace.resolve():
            raise RuntimeError("Refusing cleanup outside publisher fixture workspace")
        self.temp.cleanup()

    def write_request(self, rows):
        self.request.write_text(json.dumps({"format": publisher.AUXILIARY_FORMAT, "assets": rows}))

    def prepare(self):
        assets = self.upload.prepare_assets()
        return next(asset for asset in assets if asset["name"] == self.proof.name)

    def remote(self, local, **changes):
        row = {"id": 30, "name": local["name"], "state": "uploaded", "size": local["size"],
               "digest": "sha256:" + local["sha256"], "browser_download_url": "https://example.test/proof.zip"}
        return row | changes

    def test_default_plan_has_no_auxiliary_receipt_or_assets(self):
        self.args.auxiliary_plan = None
        assets = self.upload.prepare_assets()
        self.assertEqual(len(assets), 4)
        self.assertNotIn("auxiliary_plan", self.upload.state)
        self.assertFalse((self.args.control / publisher.AUXILIARY_RECEIPT).exists())

    def test_opt_in_checksum_and_immutable_control_pin_survive_resume(self):
        local = self.prepare()
        self.assertEqual(local, dict(self.row, path=str(self.proof.resolve())))
        receipt = self.args.control / publisher.AUXILIARY_RECEIPT
        raw, before = receipt.read_bytes(), receipt.stat().st_mtime_ns
        saved = json.loads(raw)
        self.assertEqual(saved["input_sha256"], publisher.sha256(self.request))
        self.assertEqual(saved["assets"], [local])
        self.assertIn(f"{local['sha256']}  {local['name']}\n", (self.args.control / "SHA256SUMS.txt").read_text())
        resumed = publisher.DraftUploader(self.args)
        self.assertEqual(resumed.prepare_assets(), self.upload.state["plan"])
        self.assertEqual(receipt.read_bytes(), raw)
        self.assertEqual(receipt.stat().st_mtime_ns, before)

    def test_unsafe_paths_basenames_types_and_hashes_fail_before_pin(self):
        invalid = [{"path": value} for value in ("../reviewed-proof.zip", "/reviewed-proof.zip",
                   "C:/reviewed-proof.zip", "dir\\reviewed-proof.zip", "dir//reviewed-proof.zip", "./reviewed-proof.zip")]
        invalid += [{"name": value} for value in ("../proof.zip", "SHA256SUMS.txt", "release-state.json", "proof.txt", "CON.zip")]
        invalid += [{"path": value} for value in ("folder./reviewed-proof.zip", "AUX/reviewed-proof.zip", "bad\x1f/reviewed-proof.zip")]
        invalid += [{"size": value} for value in (0, -1, True, "28", (1900 << 20) + 1)]
        invalid += [{"sha256": "a" * 63}, {"sha256": "A" * 64}, {"extra": "unknown"}]
        for change in invalid:
            with self.subTest(change=change):
                self.write_request([self.row | change])
                with self.assertRaises(RuntimeError): self.upload.prepare_assets()
                self.assertFalse((self.args.control / publisher.AUXILIARY_RECEIPT).exists())

    def test_invalid_structured_plan_is_rejected_before_control_registration(self):
        invalid = [None, [], {}, {"format": "wrong", "assets": [self.row]},
                   {"format": publisher.AUXILIARY_FORMAT, "assets": []},
                   {"format": publisher.AUXILIARY_FORMAT, "assets": [self.row], "unknown": 1}]
        for document in invalid:
            with self.subTest(document=document):
                self.request.write_text(json.dumps(document))
                with self.assertRaises(RuntimeError): self.upload.prepare_assets()
                self.assertFalse((self.args.control / publisher.AUXILIARY_RECEIPT).exists())

    def test_duplicate_case_and_runtime_asset_collisions_fail(self):
        self.write_request([self.row, self.row | {"name": "REVIEWED-PROOF.zip", "path": "REVIEWED-PROOF.zip"}])
        with self.assertRaisesRegex(RuntimeError, "Duplicate auxiliary"): self.upload.prepare_assets()
        runtime = self.root / "MidnightClub3-Native-fixture-win64.zip"
        runtime.write_bytes(b"runtime fixture")
        self.args.runtime_zip, self.args.runtime_sha256 = runtime, publisher.sha256(runtime)
        Path(str(runtime) + ".sha256").write_text(f"{self.args.runtime_sha256}  {runtime.name}\n")
        copied = self.input / runtime.name
        copied.write_bytes(runtime.read_bytes())
        self.write_request([{"name": runtime.name, "path": runtime.name, "size": copied.stat().st_size,
                             "sha256": publisher.sha256(copied)}])
        with self.assertRaisesRegex(RuntimeError, "Duplicate release"): self.upload.prepare_assets()

    def test_control_source_link_and_foreign_receipt_collisions_fail(self):
        source = self.args.control / "control-proof.zip"
        source.write_bytes(b"control bytes")
        # The request root contains control, but artifacts may not alias its mutable outputs.
        self.request = self.root / "request.json"; self.args.auxiliary_plan = self.request
        self.write_request([{"name": source.name, "path": "control/" + source.name,
                             "size": source.stat().st_size, "sha256": publisher.sha256(source)}])
        with self.assertRaisesRegex(RuntimeError, "control files"): self.upload.prepare_assets()
        self.request = self.input / "request.json"; self.args.auxiliary_plan = self.request; self.write_request([self.row])
        original = Path.lstat
        def linked(path, *args, **kwargs):
            if path == self.proof:
                return SimpleNamespace(st_mode=stat.S_IFREG, st_file_attributes=0x400)
            return original(path, *args, **kwargs)
        with patch.object(Path, "lstat", linked), self.assertRaisesRegex(RuntimeError, "link or junction"):
            self.upload.prepare_assets()
        receipt = self.args.control / publisher.AUXILIARY_RECEIPT
        receipt.write_bytes(b"foreign receipt")
        with self.assertRaisesRegex(RuntimeError, "Foreign/unregistered"): self.upload.prepare_assets()
        self.assertEqual(receipt.read_bytes(), b"foreign receipt")

    def test_missing_or_changed_bytes_never_become_a_pin(self):
        original = self.proof.read_bytes()
        self.proof.unlink()
        with self.assertRaises(FileNotFoundError): self.upload.prepare_assets()
        self.proof.write_bytes(b"x" * len(original))
        with self.assertRaisesRegex(RuntimeError, "bytes changed"): self.upload.prepare_assets()
        self.assertNotIn("auxiliary_plan", self.upload.state)

    def test_pinned_request_receipt_and_omitted_flag_are_immutable(self):
        self.prepare()
        request = self.request.read_bytes()
        self.request.write_bytes(request + b" ")
        with self.assertRaisesRegex(RuntimeError, "Immutable auxiliary"): self.upload.prepare_assets()
        self.request.write_bytes(request)
        receipt = self.args.control / publisher.AUXILIARY_RECEIPT
        receipt.write_bytes(receipt.read_bytes() + b" ")
        with self.assertRaisesRegex(RuntimeError, "Immutable auxiliary"): self.upload.prepare_assets()
        self.args.auxiliary_plan = None
        with self.assertRaisesRegex(RuntimeError, "cannot be omitted"): self.upload.prepare_assets()

    def test_matching_remote_auxiliary_still_checks_local_bytes_before_skip(self):
        local = self.prepare()
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", return_value=[self.remote(local)]), patch.object(publisher.subprocess, "run") as run:
            self.upload.upload_one(local)
            run.assert_not_called()
        self.proof.write_bytes(b"x" * local["size"])
        with patch.object(self.upload, "owned_draft") as owned, patch.object(publisher.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "bytes changed"): self.upload.upload_one(local)
            owned.assert_not_called(); run.assert_not_called()

    def test_case_collision_foreign_and_old_empty_assets_are_not_deleted(self):
        local = self.prepare()
        self.upload.state["assets"][local["name"]] = {"attempt_started_utc": "2026-01-02T00:00:00+00:00",
            "size": local["size"], "sha256": local["sha256"]}
        remotes = [self.remote(local, name=local["name"].upper()), self.remote(local, digest="sha256:" + "a" * 64),
            self.remote(local, state="starter", size=0, created_at="2026-01-01T00:00:00Z", uploader={"login": self.upload.account}),
            self.remote(local, state="starter", size=0, created_at="2026-01-03T00:00:00Z", uploader={"login": "foreign"})]
        for remote in remotes:
            with self.subTest(remote=remote), patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", return_value=[remote]), patch.object(publisher, "gh_json") as api, patch.object(publisher.subprocess, "run") as run:
                with self.assertRaises(RuntimeError): self.upload.upload_one(local)
                api.assert_not_called(); run.assert_not_called()

    def test_owned_matching_empty_attempt_can_resume_and_authenticate(self):
        local = self.prepare()
        self.upload.state["assets"][local["name"]] = {"attempt_started_utc": "2026-01-02T00:00:00.123+00:00",
            "size": local["size"], "sha256": local["sha256"]}
        starter = self.remote(local, state="starter", size=0, created_at="2026-01-02T00:00:00Z", uploader={"login": self.upload.account})
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", side_effect=[[starter], [self.remote(local)]]), patch.object(publisher, "gh_json") as api, patch.object(publisher.subprocess, "run", return_value=SimpleNamespace(returncode=0, stderr="")) as run:
            self.upload.upload_one(local)
            api.assert_called_once_with(f"{self.upload.prefix}/releases/assets/30", method="DELETE")
            self.assertNotIn("--clobber", run.call_args.args[0])

    def test_mutation_during_remote_lookup_cannot_trigger_delete(self):
        local = self.prepare()
        self.upload.state["assets"][local["name"]] = {"attempt_started_utc": "2026-01-02T00:00:00+00:00",
            "size": local["size"], "sha256": local["sha256"]}
        def remote():
            self.proof.write_bytes(b"x" * local["size"])
            return [self.remote(local, state="starter", size=0, created_at="2026-01-03T00:00:00Z", uploader={"login": self.upload.account})]
        with patch.object(self.upload, "owned_draft"), patch.object(self.upload, "assets", side_effect=remote), patch.object(publisher, "gh_json") as api:
            with self.assertRaisesRegex(RuntimeError, "bytes changed"): self.upload.upload_one(local)
            api.assert_not_called()


if __name__ == "__main__":
    unittest.main()
