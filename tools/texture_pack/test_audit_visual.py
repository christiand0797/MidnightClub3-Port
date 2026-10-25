import hashlib
from pathlib import Path
import tempfile
import unittest

import numpy as np
from PIL import Image

import audit_visual


class VisualAuditTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        for folder in ("source", "dump", "replace"):
            (self.root / folder).mkdir()

    def tearDown(self):
        self.directory.cleanup()

    def fixture(self):
        source = Image.new("RGBA", (16, 16), (90, 120, 150, 255))
        source.putalpha(Image.fromarray(np.tile(np.arange(16, dtype=np.uint8) * 17, (16, 1))))
        source.save(self.root / "source/ABC.png")
        source.resize((64, 64), Image.Resampling.LANCZOS).save(self.root / "replace/ABC.png")

    def test_exact_alpha_and_inspection_is_read_only(self):
        self.fixture()
        paths = [self.root / "source/ABC.png", self.root / "replace/ABC.png"]
        before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths]
        result = audit_visual.inspect_pair(self.root, "ABC.png", "flash\\loading_a.xbck")
        self.assertNotIn("error", result)
        self.assertEqual(result["alpha_changed_pixels"], 0)
        self.assertEqual(result["source_size"], [16, 16])
        self.assertEqual(result["output_size"], [64, 64])
        self.assertEqual(before, [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths])

    def test_reports_one_alpha_pixel_mutation(self):
        self.fixture()
        path = self.root / "replace/ABC.png"
        with Image.open(path) as image:
            output = image.convert("RGBA")
        output.putpixel((32, 32), (90, 120, 150, 0))
        output.save(path)
        result = audit_visual.inspect_pair(self.root, "ABC.png", "")
        self.assertEqual(result["alpha_changed_pixels"], 1)
        self.assertGreater(result["alpha_max_error"], 0)

    def test_damaged_output_is_an_error_not_a_clean_metric(self):
        self.fixture()
        (self.root / "replace/ABC.png").write_bytes(b"not an image")
        self.assertIn("error", audit_visual.inspect_pair(self.root, "ABC.png", ""))

    def test_windows_provenance_categories(self):
        self.assertEqual(audit_visual.category("resources\\vehicle\\rider.xbpf"), "02_people")
        self.assertEqual(audit_visual.category("flash\\garage.xbck"), "03_ui")
        self.assertEqual(audit_visual.category("resources\\vehicle\\rim.xbpf"), "04_vehicle")

    def test_original_skip_classification_preserves_excluded_palette_art(self):
        self.fixture()
        path = self.root / "source/ABC.png"
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        mask = audit_visual.inspect_original("ABC.png", path, "vinyl", {"ABC.png"})
        self.assertEqual(mask["skip_reason"], "categorical_vinyl_mask")
        self.assertEqual(mask["category"], "01_mask")
        self.assertEqual(before, hashlib.sha256(path.read_bytes()).hexdigest())
        Image.new("RGBA", (32, 32), (128, 128, 128, 255)).save(path)
        self.assertEqual(audit_visual.inspect_original("ABC.png", path, "", set())["skip_reason"], "flat")
        Image.new("RGBA", (4, 4), (128, 128, 128, 255)).save(path)
        self.assertEqual(audit_visual.inspect_original("ABC.png", path, "", set())["skip_reason"], "tiny")


if __name__ == "__main__":
    unittest.main()
