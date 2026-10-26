"""Small CPU-only proof-package fixtures; no game, media, GPU or network work."""
import hashlib
import contextlib
import io
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
import warnings
from unittest.mock import patch
import zipfile


spec = importlib.util.spec_from_file_location("package_reviewed_proof", Path(__file__).with_name("package_reviewed_proof.py"))
proof = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proof)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mc3-proof-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "project"
        self.root.mkdir()
        self.ledger = "textures/reviewed.json"
        self.refs = {}

    def put(self, name, data=b"fixture"):
        if isinstance(data, dict):
            data = (json.dumps(data, sort_keys=True) + "\n").encode()
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return {"path": name, "sha256": sha(data), "bytes": len(data)}

    def document(self, name, data):
        return self.put(name, data)

    def ledger_with(self, references):
        value = {"format": "mc3-reviewed-texture-fixes-v1", "dependencies":
                 {x["path"]: x["sha256"] for x in references}, "entries": {}}
        return self.document(self.ledger, value)["sha256"]

    def basic_graph(self):
        image = self.put("qa/capture/present.bmp", b"tiny raw BMP fixture\x00")
        meta = self.document("qa/capture/photo.json", {"format": "mc3-photo-capture-v1", "width": 2})
        context = self.document("qa/context.json", {"format": "mc3-native-gated-run-v1", "token": "0" * 32,
                                                   "release": "qa/gate.release"})
        released = self.document("qa/released.json", {"context_sha256": context["sha256"]})
        log = self.put("qa/closed.log", b"pre-init released then image captured")
        release = {"path": "qa/gate.release", "sha256": "a" * 64}
        run = self.document("proof/run.json", {
            "format": "mc3-native-photo-r11-root-run-proof-v1",
            "runs": [{"captures": [{"image": image, "metadata": meta}], "context": context,
                      "released_receipt": released, "archived_closed_log": log, "release_token": release}],
            "settings_after": {"path": "qa/settings.toml", "sha256": "b" * 64},
            "private_save_sha256": {"hdd6": {"E/save.dat": "c" * 64}},
            "isolated_exe": {"path": "qa/mc3.exe", "sha256": "d" * 64},
        })
        model = self.put("models/model.bin", b"model weights")
        module = self.put("tools/model.py", b"# model implementation")
        input_image = self.put("stage/input.png", b"input pixels")
        inference = self.document("stage/inference.json", {"format": "mc3-garage-photo-inference-v1",
            "engine": {"model_path": model["path"], "model_sha256": model["sha256"],
                       "module_path": module["path"], "module_sha256": module["sha256"]},
            "records": {"image": {"source": input_image["path"], "source_sha256": input_image["sha256"]}}})
        historical = self.put("textures/current.png", b"current candidate")
        handshake = self.document("proof/handshake.json", {
            "format": "mc3-native-photo-r11-handshake-independent-review-v1",
            "runs": [{"captures": [{"image": image, "metadata": meta}], "context": context,
                      "released_receipt": released, "archived_closed_log": log, "release_file": release}],
            "unchanged_pins": {"S_files": [{"path": historical["path"], "sha256": "0" * 64}],
                               "private_settings": {"path": "qa/settings.toml", "sha256": "1" * 64}},
            "media_resume": {"path": "live/state.json", "sha256": "2" * 64},
        })
        build = self.document("build/manifest.json", {"compiler": "C:/external/compiler.exe",
            "objects": [{"path": "missing.obj", "sha256": "3" * 64}], "command": ["--file", "unrelated"]})
        matched = self.document("proof/matched.json", {"format": "mc3-native-photo-matched-local-review-v1",
            "matched_pair": {"base": {"image": image, "metadata": meta}},
            "provenance": {"root_run_proof": run, "source_build": build}})
        native = self.document("proof/native.json", {"format": "mc3-native-texture-review-v1",
            "decision": "accepted", "evidence_sha256": {x["path"]: x["sha256"] for x in [matched, handshake]}})
        self.refs = {"image": image, "meta": meta, "context": context, "released": released,
                     "log": log, "run": run, "handshake": handshake, "native": native,
                     "model": model, "module": module, "input": input_image, "build": build,
                     "historical": historical}
        return self.ledger_with([native, inference, historical])

    def package(self, expected, name="output", **options):
        return proof.package(self.root, self.ledger, expected, self.base / name, **options)

    def test_transitive_graph_zip_manifest_determinism_and_observation_exclusions(self):
        expected = self.basic_graph()
        unrelated = self.put("unrelated/private.data", b"never package")
        first = self.package(expected, "first")
        second = self.package(expected, "second")
        self.assertEqual(first, second)
        self.assertEqual((self.base / "first" / proof.ZIP_NAME).read_bytes(),
                         (self.base / "second" / proof.ZIP_NAME).read_bytes())
        names = {f["path"] for f in first["files"]}
        for value in self.refs.values():
            self.assertIn(value["path"], names)
        self.assertNotIn(unrelated["path"], names)
        self.assertNotIn("qa/settings.toml", names)
        self.assertNotIn("qa/mc3.exe", names)
        self.assertNotIn("qa/gate.release", names)
        self.assertTrue(first["metadata_only_observations"])
        self.assertIn("build/manifest.json", {x["document"] for x in first["opaque_receipts_not_expanded"]})
        with zipfile.ZipFile(self.base / "first" / proof.ZIP_NAME) as archive:
            self.assertEqual(archive.namelist(), sorted(names))
            for item in first["files"]:
                self.assertEqual(sha(archive.read(item["path"])), item["sha256"])
            inventory_bytes = archive.read(proof.INVENTORY_NAME)
            self.assertEqual(sha(inventory_bytes), first["inventory"]["sha256"])
            inventory = json.loads(inventory_bytes)
            self.assertEqual(inventory["metadata_only_observations"], first["metadata_only_observations"])
            self.assertNotIn("archive", inventory)
            self.assertNotIn(proof.INVENTORY_NAME, [x["path"] for x in inventory["files"]])
        self.assertEqual(sha((self.base / "first" / proof.ZIP_NAME).read_bytes()), first["archive"]["sha256"])

    def test_missing_and_changed_dependencies_refuse_without_output(self):
        for mode in ("missing", "changed"):
            with self.subTest(mode=mode):
                expected = self.basic_graph()
                p = self.root / self.refs["image"]["path"]
                if mode == "missing":
                    p.unlink()
                else:
                    p.write_bytes(b"changed pixels")
                with self.assertRaises(proof.ProofError):
                    self.package(expected)
                self.assertFalse((self.base / "output").exists())

    def test_required_hash_conflict_is_not_repaired_using_history(self):
        current = self.put("art/panel.png", b"new")
        child = self.document("proof/native.json", {"format": "mc3-native-texture-review-v1",
            "decision": "accepted", "evidence_sha256": {current["path"]: "0" * 64}})
        expected = self.ledger_with([current, child])
        with self.assertRaisesRegex(proof.ProofError, "Conflicting required hashes"):
            self.package(expected)

    def test_safe_paths_and_case_collisions(self):
        item = self.put("art/panel.png")
        for bad in ("../escape", "art/../panel.png", "C:/elsewhere/asset", "/elsewhere/asset",
                    "art/CON.png", "art/trailing. ", "art/has:stream", "art//empty", "art/e\u0301.png"):
            with self.subTest(path=bad):
                expected = self.ledger_with([item, {"path": bad, "sha256": item["sha256"]}])
                with self.assertRaises(proof.ProofError):
                    self.package(expected)
        expected = self.ledger_with([item, {"path": "ART/panel.png", "sha256": item["sha256"]}])
        with self.assertRaisesRegex(proof.ProofError, "Case-colliding"):
            self.package(expected)

    def test_absolute_in_project_and_backslash_references_are_normalized(self):
        item = self.put("art/panel.png")
        child = self.document("proof/native.json", {"format": "mc3-native-texture-review-v1", "decision": "accepted",
            "evidence_sha256": {str(self.root / item["path"]): item["sha256"],
                                "art\\panel.png": item["sha256"]}})
        result = self.package(self.ledger_with([child]))
        self.assertIn("art/panel.png", [x["path"] for x in result["files"]])

    def test_linked_file_hardlink_ancestor_and_output_parent_rejected(self):
        item = self.put("art/panel.png")
        expected = self.ledger_with([item])
        hard = self.root / "hardlink"
        os.link(self.root / item["path"], hard)
        with self.assertRaisesRegex(proof.ProofError, "Linked"):
            self.package(expected)
        hard.unlink()
        real = self.root / "art/panel.png"
        real.rename(real.with_name("original"))
        try:
            os.symlink(real.with_name("original"), real)
        except OSError as exc:
            self.skipTest(f"Host does not support fixture symlinks: {exc}")
        with self.assertRaisesRegex(proof.ProofError, "Linked"):
            self.package(expected)
        real.unlink()
        real.with_name("original").rename(real)
        linked_dir = self.base / "linked-project"
        os.symlink(self.root, linked_dir, target_is_directory=True)
        with self.assertRaisesRegex(proof.ProofError, "ancestor"):
            proof.package(linked_dir, self.ledger, expected, self.base / "output")
        linked_out = self.base / "linked-output"
        os.symlink(self.base, linked_out, target_is_directory=True)
        with self.assertRaisesRegex(proof.ProofError, "ancestor"):
            proof.package(self.root, self.ledger, expected, linked_out / "new-output")

    def test_private_required_dependencies_are_rejected_not_silently_omitted(self):
        for name in ("qa/hdd6/E/save.dat", "qa/settings.toml", ".env", "secrets/key", "qa/gate.release", "qa/mc3.exe"):
            with self.subTest(name=name):
                item = self.put(name, b"must not copy")
                with self.assertRaisesRegex(proof.ProofError, "Private dependency"):
                    self.package(self.ledger_with([item]))

    def test_unknown_mc3_format_duplicate_json_keys_and_bad_hash_refused(self):
        item = self.document("proof/future.json", {"format": "mc3-future-v2", "path": "anything"})
        with self.assertRaisesRegex(proof.ProofError, "Unsupported proof format"):
            self.package(self.ledger_with([item]))
        malformed = self.put("proof/bad.json", b'{"format":"data", "x":1,"x":2}')
        with self.assertRaisesRegex(proof.ProofError, "Duplicate JSON key"):
            self.package(self.ledger_with([malformed]))
        with self.assertRaisesRegex(proof.ProofError, "Invalid SHA"):
            self.package(self.ledger_with([{"path": "missing", "sha256": "A" * 64}]))

    def test_file_and_byte_limits(self):
        expected = self.basic_graph()
        with self.assertRaisesRegex(proof.ProofError, "ceiling"):
            self.package(expected, max_files=2)
        with self.assertRaisesRegex(proof.ProofError, "ceiling"):
            self.package(expected, max_bytes=10)
        self.assertFalse((self.base / "output").exists())

    def test_existing_output_never_overwritten(self):
        expected = self.basic_graph()
        output = self.base / "output"
        output.mkdir()
        sentinel = output / "sentinel"
        sentinel.write_bytes(b"preserve")
        with self.assertRaisesRegex(proof.ProofError, "fresh output"):
            self.package(expected)
        self.assertEqual(sentinel.read_bytes(), b"preserve")
        self.assertEqual(list(output.iterdir()), [sentinel])

    def test_mutation_after_graph_check_fails_without_complete_manifest(self):
        expected = self.basic_graph()
        original = proof._copy_verified
        changed = False

        def copy(source, destination, entry):
            nonlocal changed
            if not changed:
                source.write_bytes(b"changed after planning")
                changed = True
            return original(source, destination, entry)

        with patch.object(proof, "_copy_verified", side_effect=copy):
            with self.assertRaises(proof.ProofError):
                self.package(expected)
        self.assertFalse((self.base / "output" / proof.MANIFEST_NAME).exists())

    def test_archive_reread_rejects_changed_extra_and_duplicate_members(self):
        payload = b"correct"
        expected = {"item": {"bytes": len(payload), "sha256": sha(payload)}}
        for mode in ("wrong bytes", "extra", "duplicate"):
            file = self.base / (mode + ".zip")
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(file, "w") as archive:
                    archive.writestr("item", b"bad!!!!" if mode == "wrong bytes" else payload)
                    if mode in ("extra", "duplicate"):
                        archive.writestr("other" if mode == "extra" else "item", payload)
            with self.subTest(mode=mode), self.assertRaises(proof.ProofError):
                proof.verify_archive(file, expected)

    def test_ledger_archive_and_review_bindings_must_be_explicit(self):
        refs = [self.put("templates/game/out/native/textures/replace/A.png", b"after"),
                self.put("templates/game/out/native/textures/source/A.png", b"source"),
                self.put("archive/A.png", b"before"),
                self.document("review.json", {"format": "mc3-texture-review-v1", "decision": "accepted"})]
        entry = {"after_sha256": refs[0]["sha256"], "source_sha256": refs[1]["sha256"],
                 "before_sha256": refs[2]["sha256"], "base_archive": refs[2]["path"], "review_proof": refs[3]["path"]}
        doc = {"format": "mc3-reviewed-texture-fixes-v1", "entries": {"A.png": entry},
               "dependencies": {x["path"]: x["sha256"] for x in refs}}
        expected = self.document(self.ledger, doc)["sha256"]
        self.assertTrue(self.package(expected)["complete"])
        del doc["dependencies"]["archive/A.png"]
        expected = self.document(self.ledger, doc)["sha256"]
        with self.assertRaisesRegex(proof.ProofError, "binding"):
            self.package(expected, "other-output")

    def test_check_mode_writes_nothing_and_does_not_infer_arbitrary_paths(self):
        expected = self.basic_graph()
        before = sorted(str(x.relative_to(self.base)) for x in self.base.rglob("*"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(proof.main(["--project", str(self.root), "--ledger", self.ledger,
                                         "--ledger-sha256", expected, "--check"]), 0)
        self.assertEqual(before, sorted(str(x.relative_to(self.base)) for x in self.base.rglob("*")))

    def test_reserved_inventory_and_file_directory_collisions_refused(self):
        for name in (proof.INVENTORY_NAME, proof.INVENTORY_NAME.upper(), proof.INVENTORY_NAME + "/child"):
            with self.subTest(name=name):
                with self.assertRaisesRegex(proof.ProofError, "reserved inventory"):
                    self.package(self.ledger_with([{"path": name, "sha256": "0" * 64}]))
        tree = self.put("tree", b"file")
        expected = self.ledger_with([tree, {"path": "tree/child", "sha256": "0" * 64}])
        with self.assertRaisesRegex(proof.ProofError, "File/directory-colliding"):
            self.package(expected)


if __name__ == "__main__":
    unittest.main()
