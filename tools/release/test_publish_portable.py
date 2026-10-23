"""Exercise draft ownership and upload collision checks without contacting GitHub."""
import argparse
import importlib.util
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


if __name__ == "__main__":
    unittest.main()
