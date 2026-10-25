import json
from contextlib import nullcontext
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
import loading_garage_join as join
import upscale
import xbtex


def flash_fixture():
    """Synthetic Flash pointers exercise the non-adjacent thirteen-style Tokyo layout."""
    data = bytearray(4096)
    base = 0x82BF5980
    struct.pack_into("<I", data, 0, base)
    pointer = lambda value: base+value-0x80
    textures = []
    for header, name in ((0x100, 1), (0x180, 2)):
        bitmap = header-0x20
        struct.pack_into("<HH2f", data, bitmap+0xC, 256, 256, 1/256, 1/256)
        textures.append(types.SimpleNamespace(fmt=15, width=256, height=256, header=header, name=name))
    fills, commands, shape = 0x600, 0x570, 0x540
    for index in range(13):
        struct.pack_into("<II", data, fills+index*40, 0x6B617440, 0xFFFFFFFF)
    for index, texture, tx, scale in ((4, textures[0], -.2684, .004371), (12, textures[1], -1.23556, .004346)):
        struct.pack_into("<6fI", data, fills+index*40+8, scale, 0, 0, .003716, tx, -.009104, pointer(texture.header-0x20))
    struct.pack_into("<4I", data, shape+8, pointer(fills), pointer(fills+13*40), pointer(commands), 13)
    cursor = commands
    for index, bounds in ((5, (1246, 70, 5786, 5410)), (13, (5704, 70, 10270, 5410))):
        x0, y0, x1, y1 = bounds
        struct.pack_into("<4H8h", data, cursor, 4, index, 2, 4, x0, y0, x1, y0, x1, y1, x0, y1)
        struct.pack_into("<2H", data, cursor+24, 5, 0)
        cursor += 28
    return data, textures, shape


class FlashLayoutTests(unittest.TestCase):
    def test_thirteen_fill_styles_nonadjacent_photo_bindings_and_overlapping_quads(self):
        data, textures, _shape = flash_fixture()
        layout = join.panel_layout(data, textures)
        self.assertEqual(layout["fill_count"], 13)
        self.assertEqual((layout["left"]["fill_index"], layout["right"]["fill_index"]), (5, 13))
        self.assertEqual(layout["geometry_overlap_twips"], 82)
        self.assertFalse(layout["uv_linear_parts_identical"])
        self.assertEqual(layout["left"]["texture_hash"], "0000000000000001.png")

    def test_corrupt_array_end_or_bitmap_dimensions_fail_closed(self):
        data, textures, shape = flash_fixture()
        struct.pack_into("<I", data, shape+12, 0)
        with self.assertRaises(ValueError):
            join.panel_layout(data, textures)
        data, textures, _shape = flash_fixture()
        struct.pack_into("<H", data, textures[0].header-0x20+0xC, 1024)
        with self.assertRaises(ValueError):
            join.panel_layout(data, textures)

    def test_actual_original_garage_assets_preserve_layout_differences(self):
        assets = join.ROOT/"templates/game/assets/flash"
        if not (assets/"loadscreen_garage_t.xbck").exists():
            self.skipTest("local original assets are not available")
        expected = {"a": ("2C31319DDD40661C.png", "BA7DAD34CC50D094.png", 18),
                    "d": ("11602EACA97BFCEF.png", "70A5C638246CF68C.png", 9),
                    "s": ("4D145117D9DD0F63.png", "A21582D8BE650E3F.png", 0),
                    "t": ("14E9720CCE89AA66.png", "711D98603F8060F0.png", 82)}
        for city, (left, right, overlap) in expected.items():
            with self.subTest(city=city):
                path = assets/f"loadscreen_garage_{city}.xbck"
                before = path.read_bytes()
                textures = [t for t in xbtex.xbck_textures(before) if t.fmt == 15 and (t.width, t.height) == (256, 256)]
                layout = join.panel_layout(before, textures)
                self.assertEqual((layout["left"]["texture_hash"], layout["right"]["texture_hash"]), (left, right))
                self.assertEqual(layout["geometry_overlap_twips"], overlap)
                self.assertEqual(layout["fill_count"], 13 if city == "t" else 2)
                self.assertEqual(path.read_bytes(), before)


class JoinedPixelTests(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(714)
        original = rng.integers(0, 256, (16, 26, 4), np.uint8)
        original[..., 3] = 255
        self.left, self.right = original[:, :16].copy(), original[:, 10:].copy()

    def test_six_pixel_registration_and_unmodified_nonoverlapping_source_pixels(self):
        errors = join.overlap_errors(self.left, self.right)
        self.assertEqual(errors[0], {"overlap": 6, "mean_rgb_error": 0.0})
        joined = join.join_rgb(self.left, self.right, 6)
        self.assertEqual(joined.shape, (16, 26, 3))
        self.assertTrue(np.array_equal(joined[:, :10], self.left[:, :10, :3]))
        self.assertTrue(np.array_equal(joined[:, 16:], self.right[:, 6:, :3]))

    def test_shared_rgb_is_identical_after_exact_split_and_each_original_alpha_is_preserved(self):
        self.left[..., 3] = np.arange(16, dtype=np.uint8)[:, None]*17
        self.right[..., 3] = 255-self.left[..., 3]
        joined = Image.fromarray(join.join_rgb(self.left, self.right, 6)).resize((104, 64), Image.Resampling.LANCZOS)
        left_alpha, right_alpha = (Image.fromarray(image[..., 3]) for image in (self.left, self.right))
        left, right = join.split_panels(joined, left_alpha, right_alpha, 4, 6)
        a, b = np.asarray(left), np.asarray(right)
        self.assertTrue(np.array_equal(a[:, -24:, :3], b[:, :24, :3]))
        for output, alpha in ((a, left_alpha), (b, right_alpha)):
            self.assertTrue(np.array_equal(output[..., 3], np.asarray(alpha.resize((64, 64), Image.Resampling.LANCZOS))))
        with self.assertRaises(ValueError):
            join.split_panels(joined.crop((0, 0, 100, 64)), left_alpha, right_alpha, 4, 6)

    def test_unproven_network_outputs_are_explicitly_unauthenticated_and_changed_original_rejected(self):
        with tempfile.TemporaryDirectory(prefix="joined-unit-", dir=join.ROOT/".local") as folder:
            root = Path(folder)
            for directory in ("network-input", "network-output", "source"):
                (root/directory).mkdir()
            panels = {}
            for side, array, name in (("left", self.left, "0"*16+".png"), ("right", self.right, "1"*16+".png")):
                path = root/"source"/name
                Image.fromarray(array).save(path)
                panels[side] = {"name": name, "source": str(path), "source_sha256": upscale.file_digest(path)}
            source = Image.fromarray(join.join_rgb(self.left, self.right, 6))
            source.save(root/"network-input/garage_s.png")
            source.resize((104, 64), Image.Resampling.LANCZOS).save(root/"network-output/garage_s.png")
            job = {"panels": panels, "joined_size": [26, 16], "pad": 0,
                   "network_input_sha256": upscale.file_digest(root/"network-input/garage_s.png")}
            plan = {"format": "mc3-garage-photo-joined-plan-v1", "worker_sha256": upscale.file_digest(Path(join.__file__)),
                    "planned_ncnn_context": {"never_executed": True}, "jobs": {"s": job}}
            (root/"joined-plan.json").write_text(json.dumps(plan))
            self.assertEqual(len(join.split_network(root)), 2)
            records = json.loads((root/"neural-candidate-records.json").read_text())
            self.assertFalse(records["inference_provenance_validated"])
            self.assertIsNone(records["actual_inference"])
            (root/"neural-candidates").rename(root/"preserved-diagnostic-candidates")
            (root/"neural-candidate-records.json").rename(root/"preserved-diagnostic-records.json")
            import cpu_x4plus
            weights = root/"fake-unit-weights"
            weights.write_bytes(b"unit provenance fixture; no inference is claimed by this test")
            record = {"source_sha256": job["network_input_sha256"],
                      "output_sha256": upscale.file_digest(root/"network-output/garage_s.png"),
                      "model_path": str(weights), "model_sha256": upscale.file_digest(weights),
                      "module_path": cpu_x4plus.__file__, "module_sha256": upscale.file_digest(Path(cpu_x4plus.__file__)),
                      "device": "cpu", "intra_op_threads": 2, "inter_op_threads": 2,
                      "backend": "pytorch-cpu-x4plus-v1", "architecture": cpu_x4plus.ARCHITECTURE,
                      "weight_key_validation": {"strict": True, "tensor_keys": 702, "dtype": "float32", "device": "cpu"}}
            proof = {"format": "mc3-garage-photo-inference-v1", "phase": "complete",
                     "plan_sha256": upscale.file_digest(root/"joined-plan.json"), "records": {"garage_s.png": record}}
            proof_path = root/"inference-record.json"
            proof_path.write_text(json.dumps(proof))
            self.assertEqual(len(join.split_network(root, proof_path)), 2)
            verified = json.loads((root/"neural-candidate-records.json").read_text())
            self.assertTrue(verified["inference_provenance_validated"])
            self.assertNotIn("context", verified)  # A planned NCNN model must not authenticate CPU output.
            (root/"neural-candidates").rename(root/"preserved-verified-candidates")
            (root/"neural-candidate-records.json").rename(root/"preserved-verified-records.json")
            record["output_sha256"] = "0"*64
            proof_path.write_text(json.dumps(proof))
            with self.assertRaises(ValueError):
                join.split_network(root, proof_path)
            self.assertFalse((root/"neural-candidates").exists())
            (root/"source"/panels["left"]["name"]).write_bytes(b"changed")
            with self.assertRaises(ValueError):
                join.split_network(root)
            self.assertFalse((root/"neural-candidates").exists())

    def test_cpu_owned_stop_is_preserved_without_loading_model_or_starting_gpu(self):
        import cpu_x4plus
        with tempfile.TemporaryDirectory(prefix="joined-stop-unit-", dir=join.ROOT/".local") as folder:
            root = Path(folder)
            plan = {"format": "mc3-garage-photo-joined-plan-v1", "jobs": dict.fromkeys("adst"),
                    "worker_sha256": upscale.file_digest(Path(join.__file__))}
            (root/"joined-plan.json").write_text(json.dumps(plan))
            stop = root/"stop"
            stop.write_text("preserve explicit CPU cancel\n")
            with patch.object(cpu_x4plus, "CpuX4Plus") as engine, \
                    patch.object(join.candidates, "exclusive_candidate_lock", return_value=nullcontext()):
                self.assertEqual(join.run_cpu(root, stop), 2)
                engine.assert_not_called()
            self.assertEqual(stop.read_text(), "preserve explicit CPU cancel\n")
            self.assertFalse((root/"inference-record.json").exists())


if __name__ == "__main__":
    unittest.main()
