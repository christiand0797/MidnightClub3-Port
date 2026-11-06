"""Synthetic CPU-only font publication; no installed textures, model or GPU access."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from PIL import Image
import crisp_fonts as fonts


class FontPublication(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mc3-font-publication-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source, self.output = self.root / "source.png", self.root / "output.png"
        Image.new("RGBA", (16, 16), "white").save(self.source)
        self.image = Image.new("RGBA", (128, 128), "white")

    def publish(self):
        fonts.publish(self.image, self.output, fonts.file_digest(self.source))

    def test_validity_binds_original_pipeline_dimensions_and_intact_png(self):
        self.publish()
        self.assertTrue(fonts.replacement_valid(self.output, self.source))
        Image.new("RGBA", (16, 16), "red").save(self.source)
        self.assertFalse(fonts.replacement_valid(self.output, self.source))
        self.publish()
        real_digest = fonts.file_digest
        with patch.object(fonts, "file_digest", side_effect=lambda p: "0" * 64 if str(p) == fonts.__file__ else real_digest(p)):
            self.assertFalse(fonts.replacement_valid(self.output, self.source))
        self.output.write_bytes(self.output.read_bytes()[:100])
        self.assertFalse(fonts.replacement_valid(self.output, self.source))

    def test_foreign_fixed_temporary_and_hardlinked_previous_output_survive(self):
        foreign = self.output.with_suffix(".crisp.tmp")
        foreign.write_bytes(b"foreign in-flight work")
        Image.new("RGBA", (64, 64), "red").save(self.output)
        archive = self.root / "archive.png"
        archive.hardlink_to(self.output)
        previous = archive.read_bytes()
        self.publish()
        self.assertEqual(archive.read_bytes(), previous)
        self.assertEqual(foreign.read_bytes(), b"foreign in-flight work")
        self.assertTrue(fonts.replacement_valid(self.output, self.source))
        self.assertEqual(list(self.root.glob(".*.crisp.tmp")), [])

    def test_failed_encoding_and_replace_preserve_previous_output(self):
        self.publish()
        previous = self.output.read_bytes()
        for operation in (patch.object(Image.Image, "save", side_effect=OSError("encode failed")),
                          patch.object(fonts.os, "replace", side_effect=PermissionError("busy"))):
            with operation, self.assertRaises(OSError):
                self.publish()
            self.assertEqual(self.output.read_bytes(), previous)
            self.assertEqual(list(self.root.glob(".*.crisp.tmp")), [])

    def test_oversized_source_is_left_unchanged(self):
        Image.new("RGBA", (2049, 1), "white").save(self.source)
        self.assertIsNone(fonts.rebuild(self.source, self.root / "missing.png"))


if __name__ == "__main__":
    unittest.main()
