"""Synthetic files and isolated Python children only; no native game or upload."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "cloud_snapshot.py"
SPEC = importlib.util.spec_from_file_location("mc3_cloud_snapshot", MODULE_PATH)
snapshot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = snapshot
SPEC.loader.exec_module(snapshot)


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mc3-cloud-snapshot-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.install = self.root / "install"
        self.install.mkdir()
        self.name = "hdd/E/UDATA/54540079/Fixture/Player1.mc3Remix"
        self.selected = self.install / "hdd7/E/UDATA/54540079/Fixture/Player1.mc3Remix"
        self.selected.parent.mkdir(parents=True)
        self.binary = bytes(range(256)) * 23 + b"\x00\xff\r\n"
        self.selected.write_bytes(self.binary)
        (self.install / "mc3.exe").write_bytes(b"non-executable fixture identity only")
        self.observation = snapshot.ExitObservation(123, str(self.install / "mc3.exe"), 1234560, 7, 0)

    def collect(self, names=None, **options):
        return snapshot.collect_snapshot(self.install, 7, names or [self.name], 4, self.observation,
                                         closed_check=lambda _: None, stability_seconds=0, **options)

    def test_exact_binary_bytes_and_virtual_instance_mapping(self):
        # A different/default profile never substitutes for the explicitly selected hdd7.
        other = self.install / self.name
        other.parent.mkdir(parents=True)
        other.write_bytes(b"default profile")
        request, receipt = self.collect()
        self.assertEqual(request["base_revision"], 4)
        self.assertEqual(request["settings"], {})
        self.assertEqual(request["files"][0]["path"], self.name)
        self.assertEqual(base64.b64decode(request["files"][0]["data"]), self.binary)
        self.assertEqual(request["files"][0]["sha256"], hashlib.sha256(self.binary).hexdigest())
        self.assertEqual(receipt["physical_hdd_directory"], "hdd7")
        self.assertEqual(self.selected.read_bytes(), self.binary)
        self.assertEqual(other.read_bytes(), b"default profile")
        for field in ("uploaded", "restored", "physical_disk_durability_proved", "original_loader_accepted"):
            self.assertIs(receipt[field], False)

    def test_explicit_known_preferences_only(self):
        config = self.install / "config"
        config.mkdir()
        (config / "tuning_profiles.toml").write_bytes(b"[profiles]\nfixture='opaque'\n")
        (self.install / "settings.toml").write_bytes(b"[graphics]\nbrightness=0.125\n")
        request, _ = self.collect([self.name, "settings.toml", "config/tuning_profiles.toml"])
        self.assertEqual(len(request["files"]), 3)
        self.assertEqual(base64.b64decode(request["files"][2]["data"]), (config / "tuning_profiles.toml").read_bytes())
        for name in ("config/account.json", "config/session.toml", ".env", "relay/db.json", "hdd/launch.bin",
                     "hdd/E/UDATA/54540079/token.json", "config/passwords.ini"):
            with self.subTest(name=name), self.assertRaises(snapshot.SnapshotError):
                snapshot.virtual_path(name)

    def test_portable_path_and_title_scope(self):
        for name in ("../settings.toml", "hdd7/E/UDATA/54540079/Fixture/Player1.mc3Remix", "C:/settings.toml",
                     "hdd/E/UDATA/OTHER/Profile.mc3Remix", "hdd/E/UDATA/54540079/A/../Player1.mc3Remix",
                     "hdd/E/UDATA/54540079/CON.mc3Remix", "hdd/E/UDATA/54540079/COM¹.mc3Remix",
                     "hdd/E/UDATA/54540079/a?.mc3Remix", "hdd/E/UDATA/54540079/A /SaveMeta.xbx",
                     "hdd/E/UDATA/54540079/A\\Player1.mc3Remix", "hdd/E/UDATA/54540079/A:stream.mc3Remix",
                     "hdd/E/UDATA/54540079/e\u0301.mc3Remix", "hdd/E/UDATA/54540079/\ud800.mc3Remix"):
            with self.subTest(name=repr(name)), self.assertRaises(snapshot.SnapshotError):
                snapshot.virtual_path(name)

    def test_no_implicit_scan_or_missing_source_fallback(self):
        self.selected.unlink()
        with self.assertRaises(OSError):
            self.collect()
        self.assertFalse((self.root / "proposal").exists())

    def test_selected_identity_required(self):
        for observation in (None, snapshot.ExitObservation(123, str(self.install / "mc3.exe"), 1, 1, 0),
                            snapshot.ExitObservation(123, str(self.root / "foreign.exe"), 1, 7, 0),
                            snapshot.ExitObservation(123, str(self.install / "mc3.exe"), 1, 7, 5)):
            with self.subTest(observation=observation), self.assertRaises(snapshot.SnapshotError):
                snapshot.collect_snapshot(self.install, 7, [self.name], 0, observation, closed_check=lambda _: None)

    def test_revision_instances_and_duplicates(self):
        for value in (-1, True, 2**53, "1"):
            with self.subTest(revision=value), self.assertRaises(snapshot.SnapshotError):
                snapshot.collect_snapshot(self.install, 7, [self.name], value, self.observation)
        for instance in (-1, 9, True):
            with self.subTest(instance=instance), self.assertRaises(snapshot.SnapshotError):
                snapshot.instance_root(self.install, instance)
        self.assertEqual(snapshot.instance_root(self.install, 0).name, "hdd")
        self.assertEqual(snapshot.instance_root(self.install, 1).name, "hdd")
        with self.assertRaises(snapshot.SnapshotError):
            self.collect([self.name, self.name.lower()])
        with self.assertRaises(snapshot.SnapshotError):
            self.collect([self.name] * 257)

    def test_rejects_hard_link_alias(self):
        linked = self.root / "outside-original"
        os.link(self.selected, linked)
        with self.assertRaises(snapshot.SnapshotError):
            self.collect()
        self.assertEqual(linked.read_bytes(), self.binary)

    def test_rejects_symlink_alias(self):
        external = self.root / "outside-save"
        external.write_bytes(self.binary)
        self.selected.unlink()
        try:
            self.selected.symlink_to(external)
        except OSError:
            self.skipTest("Creating symlinks is unavailable on this host.")
        with self.assertRaises(snapshot.SnapshotError):
            self.collect()
        self.assertEqual(external.read_bytes(), self.binary)

    def test_source_change_between_passes_is_rejected(self):
        count = 0
        def check(_):
            nonlocal count
            count += 1
            if count == 2:
                self.selected.write_bytes(b"changed after first read")
        with self.assertRaises(snapshot.SnapshotError):
            snapshot.collect_snapshot(self.install, 7, [self.name], 0, self.observation,
                                      closed_check=check, stability_seconds=0)

    def test_source_identity_change_on_open_is_rejected(self):
        original = snapshot._read_file
        count = 0
        def read(path, remaining):
            nonlocal count
            count += 1
            if count == 2:
                path.unlink()
                path.write_bytes(self.binary)
            return original(path, remaining)
        with patch.object(snapshot, "_read_file", side_effect=read), self.assertRaises(snapshot.SnapshotError):
            self.collect()

    def test_running_game_blocks_collection(self):
        with patch.object(snapshot, "_cim_processes", return_value=[{"ExecutablePath": str(self.install / "mc3.exe")}]), self.assertRaises(snapshot.SnapshotError):
            snapshot.assert_native_closed(self.install)
        with patch.object(snapshot, "_cim_processes", return_value=[{"ExecutablePath": None}]), self.assertRaises(snapshot.SnapshotError):
            snapshot.assert_native_closed(self.install)
        with patch.object(snapshot, "_cim_processes", return_value=[{"ExecutablePath": str(self.root / "different/mc3.exe")} ]):
            snapshot.assert_native_closed(self.install)

    def test_shared_preferences_change_caught_by_final_closed_check(self):
        count = 0
        def check(_):
            nonlocal count
            count += 1
            if count == 3:
                raise snapshot.SnapshotError("A new MC3 instance started.")
        with self.assertRaises(snapshot.SnapshotError):
            snapshot.collect_snapshot(self.install, 7, [self.name], 0, self.observation,
                                      closed_check=check, stability_seconds=0)

    def test_total_byte_quota(self):
        second = self.selected.with_name("Player2.mc3Remix")
        self.selected.write_bytes(b"\0" * snapshot.MAX_BYTES)
        second.write_bytes(b"x")
        with self.assertRaises(snapshot.SnapshotError):
            self.collect([self.name, self.name.replace("Player1", "Player2")])

    def test_exclusive_output_and_request_digest(self):
        request, receipt = self.collect()
        output = self.root / "proposal"
        snapshot.write_proposal(output, self.install, request, receipt)
        data = (output / "cloud-request.json").read_bytes()
        actual = json.loads((output / "proposal.json").read_bytes())
        self.assertEqual(json.loads(data), request)
        self.assertEqual(actual["request_sha256"], hashlib.sha256(data).hexdigest())
        with self.assertRaises(OSError):
            snapshot.write_proposal(output, self.install, request, receipt)
        with self.assertRaises(snapshot.SnapshotError):
            snapshot.write_proposal(self.install / "proposal", self.install, request, receipt)
        self.assertEqual((output / "cloud-request.json").read_bytes(), data)

    def test_instance_parser_matches_actual_port(self):
        for command, expected in [('mc3.exe', 0), ('mc3.exe --instance 7', 7), ('mc3.exe --instance7', 7),
                                  ('mc3.exe --instance -1', 0), ('mc3.exe --instance 99', 8),
                                  ('mc3.exe --instance=7', 0), ('mc3.exe --instance 2 --instance 7', 2)]:
            with self.subTest(command=command):
                self.assertEqual(snapshot.original_instance(command), expected)

    @unittest.skipUnless(shutil.which("node"), "Node is unavailable for server contract check")
    def test_actual_server_accepts_opaque_snapshot_contract(self):
        request, _ = self.collect()
        module = MODULE_PATH.parent / "relay/account_service.js"
        code = ("const fs=require('node:fs');const assert=require('node:assert/strict');"
                "const m=require(process.argv[1]);const value=JSON.parse(fs.readFileSync(0,'utf8'));"
                "const result=m.validateSnapshot(value);assert.equal(result.files.length,1);"
                "assert.equal(result.bytes,5892);assert.equal(result.files[0].sha256,value.files[0].sha256);"
                "assert.equal(result.files[0].data,value.files[0].data);process.stdout.write('accepted');")
        result = subprocess.run([shutil.which("node"), "-e", code, str(module)],
                                input=json.dumps(request), text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "accepted")

    @unittest.skipUnless(os.name == "nt", "Windows process handle verification")
    def test_actual_wrong_instance_rejected(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(2)", "--instance", "2"])
        try:
            with self.assertRaises(snapshot.SnapshotError):
                snapshot._wait_windows_process(Path(sys.executable), child.pid, 5, 7)
        finally:
            child.wait(timeout=10)

    @unittest.skipUnless(os.name == "nt", "Windows process handle verification")
    def test_actual_checked_python_child_with_instance(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3)", "--instance", "7"])
        try:
            observation = snapshot._wait_windows_process(Path(sys.executable), child.pid, 15, 7)
            self.assertEqual(observation.process_id, child.pid)
            self.assertEqual(observation.exit_code, 0)
            self.assertEqual(observation.instance, 7)
            self.assertGreater(observation.creation_filetime, 0)
        finally:
            child.wait(timeout=10)

    @unittest.skipUnless(os.name == "nt", "Windows process handle verification")
    def test_actual_foreign_executable_rejected(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(1)"])
        try:
            with self.assertRaises(snapshot.SnapshotError):
                snapshot.wait_for_native_exit(self.install, 7, child.pid, 3)
        finally:
            child.wait(timeout=10)

    @unittest.skipUnless(os.name == "nt", "Windows process handle verification")
    def test_actual_nonzero_exit_rejected(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(1); raise SystemExit(5)"])
        try:
            with self.assertRaises(snapshot.SnapshotError):
                snapshot._wait_windows_process(Path(sys.executable), child.pid, 4, None)
        finally:
            child.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
