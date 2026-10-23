import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("snapshot_publish", Path(__file__).with_name("snapshot_publish.py"))
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class ArchiveCoverageTests(unittest.TestCase):
    # Captures the actual 7-Zip 26.04 SLT shapes that broke the full project: RDA root, HDA .git.
    SLT = """Path = checkpoint
Size = 0
Packed Size = 0
Attributes = RDA
CRC =

Path = checkpoint\\.git
Size = 0
Attributes = HDA

Path = checkpoint\\empty hidden system
Size = 0
Attributes = HSDA

Path = checkpoint\\.git\\config
Size = 6
Attributes = A
CRC = 12345678

Path = checkpoint\\empty file
Size = 0
Attributes = RHA
CRC = 00000000

Path = checkpoint\\D in filename.txt
Size = 3
Attributes = A

"""
    MANIFEST = {
        "files": [{"path": ".git/config", "size": 6}, {"path": "empty file", "size": 0},
                  {"path": "D in filename.txt", "size": 3}],
        "directories": [".git", "empty hidden system"],
    }

    def verify(self, rows=None, manifest=None):
        with patch.object(publisher, "archive_listing", return_value=rows or publisher.parse_archive_listing(self.SLT)):
            return publisher.verify_archive_listing(Path("checkpoint.7z.001"), Path("checkpoint"), manifest or self.MANIFEST)

    def test_hidden_readonly_system_directories_keep_exact_coverage(self):
        rows = publisher.parse_archive_listing(self.SLT.replace("\n", "\r\n"))
        self.assertEqual(rows[0]["Attributes"], "RDA")
        self.assertEqual(rows[0]["CRC"], "")
        self.assertEqual(self.verify(rows), {"file_count": 3, "directory_count": 3, "total_bytes": 9})

    def test_directory_flag_is_not_position_dependent(self):
        for attributes in ("D", "DA", "RDA", "HDA", "HSDA", "ARHD", "D_ drwxr-xr-x"):
            with self.subTest(attributes=attributes):
                self.assertTrue(publisher.archive_directory({"Attributes": attributes}))
        self.assertTrue(publisher.archive_directory({"Folder": "+"}))
        for attributes in ("", "A", "RHA", "A_ -rw-r--r--"):
            with self.subTest(attributes=attributes):
                self.assertFalse(publisher.archive_directory({"Attributes": attributes, "Size": "0", "Path": "D"}))

    def test_missing_hidden_directory_is_still_an_error(self):
        rows = [row for row in publisher.parse_archive_listing(self.SLT) if row["Path"] != "checkpoint\\.git"]
        with self.assertRaisesRegex(ValueError, "directory coverage"):
            self.verify(rows)

    def test_missing_file_is_still_an_error(self):
        rows = [row for row in publisher.parse_archive_listing(self.SLT) if row["Path"] != "checkpoint\\.git\\config"]
        with self.assertRaisesRegex(ValueError, "missing=.*config"):
            self.verify(rows)

    def test_extra_file_is_not_hidden_by_its_attributes(self):
        rows = publisher.parse_archive_listing(self.SLT)
        rows.append({"Path": "checkpoint/unexpected", "Attributes": "HRA", "Size": "0"})
        with self.assertRaisesRegex(ValueError, "extra=.*unexpected"):
            self.verify(rows)

    def test_wrong_size_is_reported(self):
        rows = publisher.parse_archive_listing(self.SLT)
        rows[-1]["Size"] = "8"
        with self.assertRaisesRegex(ValueError, "wrong_size=.*filename"):
            self.verify(rows)

    def test_duplicate_entries_are_rejected(self):
        for index in (0, 3):
            rows = publisher.parse_archive_listing(self.SLT)
            rows.append(dict(rows[index]))
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "duplicate archive path"):
                self.verify(rows)


class ArchiveResumeTests(unittest.TestCase):
    def test_completed_attempt_is_adopted_without_compression(self):
        state = {"control": "control", "snapshot": "checkpoint", "archive_attempts": ["control/archive-attempt-01"]}
        with patch.object(Path, "exists", return_value=True), patch.object(publisher, "adopt_archive") as adopt, \
                patch.object(publisher, "run_logged") as run, patch.object(publisher, "verified_archive_manifest") as verify:
            publisher.create_archive(state)
        adopt.assert_called_once_with(state, Path("control/archive-attempt-01/checkpoint.7z.001"))
        run.assert_not_called()
        verify.assert_not_called()

    def test_an_invalid_previous_attempt_does_not_silently_recompress(self):
        state = {"control": "control", "snapshot": "checkpoint", "archive_attempts": ["control/archive-attempt-01"]}
        with patch.object(Path, "exists", return_value=True), \
                patch.object(publisher, "adopt_archive", side_effect=ValueError("bad archive")), \
                patch.object(publisher, "run_logged") as run:
            with self.assertRaisesRegex(ValueError, "bad archive"):
                publisher.create_archive(state)
        run.assert_not_called()

    def test_adoption_rejects_an_unrecorded_attempt(self):
        state = {"control": "control", "snapshot": "checkpoint", "archive_attempts": ["control/archive-attempt-01"]}
        with patch.object(publisher, "verified_archive_manifest") as verify:
            with self.assertRaisesRegex(ValueError, "recorded archive attempt"):
                publisher.adopt_archive(state, Path("control/archive-attempt-02/checkpoint.7z.001"))
        verify.assert_not_called()

    def test_local_adoption_never_calls_github(self):
        state_file = Path("state.json").resolve()
        state = {"state_path": str(state_file), "source": "source", "backup_root": "backup", "snapshot": "checkpoint",
                 "copied": True, "verified": True, "phase": "archiving"}
        args = ["snapshot_publish.py", "--state", str(state_file), "--resume", "--execute", "--adopt-archive", "checkpoint.7z.001"]
        with patch("sys.argv", args), patch.object(Path, "is_file", return_value=True), \
                patch.object(Path, "read_text", return_value=json.dumps(state)), \
                patch.object(publisher, "validate_paths"), patch.object(publisher, "checked_tool"), \
                patch.object(publisher, "state_lock", return_value=contextlib.nullcontext()), \
                patch.object(publisher, "save_state"), patch.object(publisher, "adopt_archive") as adopt, \
                patch.object(publisher, "assert_private_repo") as cloud, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(publisher.main(), 2)
        adopt.assert_called_once()
        cloud.assert_not_called()


class DraftReleaseLookupTests(unittest.TestCase):
    STATE = {"repo": "owner/private-backups", "tag": "checkpoint-1"}

    def test_draft_without_a_created_git_tag_is_recovered(self):
        draft = {"id": 42, "tag_name": "checkpoint-1", "draft": True,
                 "html_url": "https://github.com/owner/private-backups/releases/tag/untagged-123"}
        other = {"id": 1, "tag_name": "another-checkpoint", "draft": False}
        with patch.object(publisher, "gh_json", side_effect=[None, [[other], [draft]]]) as api:
            self.assertEqual(publisher.release_record(self.STATE), draft)
        self.assertIn("--paginate", api.call_args_list[1].args[0])
        self.assertIn("--slurp", api.call_args_list[1].args[0])

    def test_saved_release_id_avoids_the_unpublished_tag_lookup(self):
        release = {"id": 42, "tag_name": "checkpoint-1", "draft": True}
        with patch.object(publisher, "gh_json", return_value=release) as api:
            self.assertEqual(publisher.release_record({**self.STATE, "release_id": 42}), release)
        api.assert_called_once_with(["api", "repos/owner/private-backups/releases/42"], missing_ok=True)

    def test_published_tag_needs_no_list_fallback(self):
        release = {"id": 42, "tag_name": "checkpoint-1", "draft": False}
        with patch.object(publisher, "gh_json", return_value=release) as api:
            self.assertEqual(publisher.release_record(self.STATE), release)
        self.assertEqual(api.call_count, 1)

    def test_absent_tag_and_absent_draft_return_none(self):
        with patch.object(publisher, "gh_json", side_effect=[None, [[]]]):
            self.assertIsNone(publisher.release_record(self.STATE))

    def test_ambiguous_drafts_are_preserved_and_rejected(self):
        drafts = [{"id": n, "tag_name": "checkpoint-1", "draft": True} for n in (1, 2)]
        with patch.object(publisher, "gh_json", side_effect=[None, [drafts]]):
            with self.assertRaisesRegex(RuntimeError, "multiple releases"):
                publisher.release_record(self.STATE)

    def test_publication_records_the_canonical_url_instead_of_the_draft_url(self):
        state = {**self.STATE, "control": "control", "state_path": "state.json", "snapshot_id": "checkpoint-root",
                 "snapshot_files": 1, "snapshot_bytes": 2, "source_git_head": "a" * 40}
        draft = {"id": 42, "tag_name": state["tag"], "draft": True, "body": "Snapshot ID: checkpoint-root",
                 "html_url": "https://github.com/owner/private-backups/releases/tag/untagged-123"}
        published = {**draft, "draft": False, "html_url": "https://github.com/owner/private-backups/releases/tag/checkpoint-1",
                     "published_at": "2026-10-23T00:00:00Z"}
        with patch.object(publisher, "assert_private_repo"), patch.object(publisher, "release_assets", return_value=[]), \
                patch.object(Path, "write_text"), patch.object(publisher, "release_record", side_effect=[draft, published]), \
                patch.object(publisher, "remote_assets", return_value={}), patch.object(publisher, "save_state") as save, \
                patch.object(publisher, "checked_tool", return_value="gh"), patch.object(publisher, "run_logged"):
            publisher.upload_and_publish(state)
        self.assertTrue(state["published"])
        self.assertEqual(state["release_url"], published["html_url"])
        self.assertEqual(save.call_args.args[2], "published")


if __name__ == "__main__":
    unittest.main()
