import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import loading_art_audit as audit
import loading_art_worker as worker
import upscale


class LoadingPixelsTests(unittest.TestCase):
    def test_loading_bundle_inventory_includes_all_categories_only(self):
        for name in ("loadscreen_garage_s.xbck", "loading_network_t01.xbck", "LoadMovie01.xbck",
                     "loadscreen_career_WorldChamp_1_part_7.xbck"):
            self.assertTrue(audit.loading_bundle(name))
        for name in ("garage.xbck", "bg_main.xbck", "loading_network_s.txt", "loads.xbck"):
            self.assertFalse(audit.loading_bundle(name))

    def test_independent_dxt5_known_color_and_alpha(self):
        block = bytes((255, 0))+bytes(6)+struct.pack("<HHI", 0xF800, 0x07E0, 0)
        image = audit.independent_dxt(block, 4, 4, 0x0F)
        self.assertEqual(image.shape, (4, 4, 4))
        self.assertTrue(np.all(image == (255, 0, 0, 255)))
        with self.assertRaises(ValueError):
            audit.independent_dxt(block[:-1], 4, 4, 0x0F)
        self.assertIsNone(audit.independent_dxt(b"", 4, 4, 0x0B))

    def test_report_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/"report.json"
            audit.write_new(path, {"original": True})
            before = path.read_bytes()
            with self.assertRaises(FileExistsError):
                audit.write_new(path, {"changed": True})
            self.assertEqual(path.read_bytes(), before)


class LoadingCandidateTests(unittest.TestCase):
    def setUp(self):
        (worker.ROOT/".local").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="loading-unit-", dir=worker.ROOT/".local")
        self.folder = Path(self.temporary.name)
        self.textures = self.folder/"textures"
        for name in ("source", "replace"):
            (self.textures/name).mkdir(parents=True)
        self.name = "0123456789ABCDEF.png"
        self.source = self.textures/"source"/self.name
        pixels = np.zeros((16, 16, 4), np.uint8)
        pixels[..., :3] = np.arange(16, dtype=np.uint8)[None, :, None]*15
        pixels[..., 3] = np.arange(16, dtype=np.uint8)[:, None]*17
        Image.fromarray(pixels).save(self.source)
        Image.new("RGBA", (64, 64), "green").save(self.textures/"replace"/self.name)
        self.source_bytes = self.source.read_bytes()
        self.active_bytes = (self.textures/"replace"/self.name).read_bytes()
        self.manifest = self.folder/"manifest.json"
        self.manifest.write_text(json.dumps({"format": "mc3-loading-art-candidate-manifest-v1",
            "recipe": "loading-reflect-both-v1", "jobs": {self.name: {"source": str(self.source),
                "source_sha256": upscale.file_digest(self.source), "size": [16, 16], "scale": 4,
                "base_replacement_sha256": upscale.file_digest(self.textures/"replace"/self.name)}}}))
        self.patch_textures = patch.object(upscale, "TEXTURES", self.textures)
        self.patch_textures.start()
        self.patch_context = patch.object(worker, "context", side_effect=lambda p, m:
            {"manifest_sha256": upscale.file_digest(p), "model": m, "recipe": "unit-reflect"})
        self.patch_context.start()

    def tearDown(self):
        self.patch_context.stop()
        self.patch_textures.stop()
        self.temporary.cleanup()

    def test_structural_alpha_mask_is_separate_from_color_art(self):
        image = np.full((16, 16, 4), 255, np.uint8)
        image[..., 3] = np.arange(16, dtype=np.uint8)[:, None]*17
        Image.fromarray(image).save(self.source)
        _job, reason = audit.classify(image, self.textures, self.name)
        self.assertEqual(reason, "structural_alpha_mask")

    def test_forced_reflection_uses_near_edge_not_opposite_image_edge(self):
        work = self.folder/"padded.png"
        with patch.object(upscale, "tiles", return_value=(True, True)):
            job = worker.prepare_source(self.source, upscale.file_digest(self.source), work)
        self.assertFalse(job["wrap_x"] or job["wrap_y"])
        original = np.asarray(Image.open(self.source).convert("RGBA"))[..., :3]
        expected = np.pad(original, ((8, 8), (8, 8), (0, 0)), mode="reflect")
        self.assertTrue(np.array_equal(np.asarray(Image.open(work)), expected))
        self.assertEqual(self.source.read_bytes(), self.source_bytes)

    def test_preparation_refuses_source_changed_since_audit(self):
        with self.assertRaises(ValueError):
            worker.prepare_source(self.source, "0"*64, self.folder/"bad.png")
        self.assertFalse((self.folder/"bad.png").exists())

    def test_stage_preserves_active_data_and_rejects_recipe_drift(self):
        stage = self.folder/"stage"
        _manifest, ledger, path = worker.stage(self.manifest, stage, "ultrasharp-4x")
        self.assertEqual(ledger["phase"], "prepared")
        self.assertEqual(ledger["completed"], {})
        self.assertEqual((self.textures/"replace"/self.name).read_bytes(), self.active_bytes)
        self.manifest.write_text(self.manifest.read_text()+" ")
        with self.assertRaises(ValueError):
            worker.stage(self.manifest, stage, "ultrasharp-4x")
        self.assertEqual(json.loads(path.read_text())["phase"], "prepared")

    def test_staging_cannot_overlap_active_pack_or_escape_project_local(self):
        for directory in (self.textures, self.textures/"replace", worker.ROOT/"templates/game/out/native/textures",
                          worker.ROOT/"templates/game/assets", worker.ROOT/".local", worker.ROOT.parent/"unrelated"):
            with self.assertRaises(ValueError):
                worker.safe_staging(directory)

    def test_existing_game_or_gpu_worker_prevents_inference(self):
        with patch.object(worker, "stopped", return_value=False), patch.object(worker, "active_workers", return_value=[{"Name": "mc3.exe"}]), \
                patch.object(worker.subprocess, "run") as network:
            with self.assertRaises(RuntimeError):
                worker.run(self.manifest, self.folder/"stage", "ultrasharp-4x", 8)
            network.assert_not_called()

    def test_owned_stop_is_preserved_and_no_inference_begins(self):
        marker = self.folder/"stop"
        marker.write_text("user cancel\n")
        with patch.object(worker, "stopped", side_effect=lambda _: marker.exists()), patch.object(worker.subprocess, "run") as network:
            self.assertEqual(worker.run(self.manifest, self.folder/"stage", "ultrasharp-4x", 8, marker), 2)
            network.assert_not_called()
        self.assertEqual(marker.read_text(), "user cancel\n")

    def test_simulated_network_stages_exact_alpha_and_never_publishes_active_pack(self):
        def simulate(command, **_kwargs):
            source = Path(command[command.index("-i")+1]); destination = Path(command[command.index("-o")+1])
            for path in source.glob("*.png"):
                with Image.open(path) as image:
                    image.resize((image.width*4, image.height*4), Image.Resampling.NEAREST).save(destination/path.name)
            return types.SimpleNamespace(returncode=0, stderr="")
        stage = self.folder/"stage"
        with patch.object(worker, "stopped", return_value=False), patch.object(worker, "active_workers", return_value=[]), \
                patch.object(worker.subprocess, "run", side_effect=simulate) as network:
            self.assertEqual(worker.run(self.manifest, stage, "ultrasharp-4x", 8), 0)
            self.assertEqual(worker.run(self.manifest, stage, "ultrasharp-4x", 8), 0)
            self.assertEqual(network.call_count, 1)
        candidate = np.asarray(Image.open(stage/"replace"/self.name).convert("RGBA"))
        original_alpha = Image.open(self.source).convert("RGBA").getchannel("A").resize((64, 64), Image.Resampling.LANCZOS)
        self.assertTrue(np.array_equal(candidate[..., 3], np.asarray(original_alpha)))
        self.assertEqual(self.source.read_bytes(), self.source_bytes)
        self.assertEqual((self.textures/"replace"/self.name).read_bytes(), self.active_bytes)
        ledger = json.loads((stage/"loading_candidate_pass.json").read_text())
        self.assertEqual(ledger["phase"], "complete")
        self.assertEqual(ledger["completed"][self.name]["padding"], "reflect-both")


if __name__ == "__main__":
    unittest.main()
