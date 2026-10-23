"""Small publication/resume checks; no model or GPU subprocess is executed."""
import tempfile
import json
import io
import subprocess
from contextlib import redirect_stdout, redirect_stderr
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
from PIL import Image
from PIL.PngImagePlugin import PngImageFile
import upscale
import audit


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.source = self.folder / "source.png"
        self.destination = self.folder / "replacement.png"
        Image.new("RGBA", (16, 16), (1, 2, 3, 255)).save(self.source)

    def test_resume_requires_a_complete_png_of_expected_dimensions(self):
        self.assertFalse(upscale.replacement_valid(self.destination, self.source))
        self.destination.write_bytes(b"\x89PNG\r\n\x1a\ninterrupted")
        self.assertFalse(upscale.replacement_valid(self.destination, self.source))
        Image.new("RGBA", (32, 32)).save(self.destination)
        self.assertFalse(upscale.replacement_valid(self.destination, self.source))
        upscale.publish_png(Image.new("RGBA", (64, 64)), self.destination)
        self.assertTrue(upscale.replacement_valid(self.destination, self.source))

    def test_encoding_failure_preserves_previous_image_and_cleans_temporary(self):
        upscale.publish_png(Image.new("RGBA", (64, 64), "red"), self.destination)
        before = self.destination.read_bytes()
        with patch.object(Image.Image, "save", side_effect=OSError("simulated interruption")):
            with self.assertRaises(OSError):
                upscale.publish_png(Image.new("RGBA", (64, 64), "blue"), self.destination)
        self.assertEqual(self.destination.read_bytes(), before)
        self.assertEqual(list(self.folder.glob(".*.tmp")), [])

    def test_validation_and_replacement_failures_do_not_publish(self):
        upscale.publish_png(Image.new("RGBA", (64, 64), "red"), self.destination)
        before = self.destination.read_bytes()
        with patch.object(PngImageFile, "verify", side_effect=SyntaxError("invalid PNG")):
            with self.assertRaises(SyntaxError):
                upscale.publish_png(Image.new("RGBA", (64, 64)), self.destination)
        with patch.object(upscale.os, "replace", side_effect=PermissionError("file in use")):
            with self.assertRaises(PermissionError):
                upscale.publish_png(Image.new("RGBA", (64, 64)), self.destination)
        self.assertEqual(self.destination.read_bytes(), before)
        self.assertEqual(list(self.folder.glob(".*.tmp")), [])

    def test_finish_crops_padding_and_preserves_alpha(self):
        network = self.folder / "network.png"
        Image.new("RGB", (128, 128), (50, 100, 150)).save(network)
        job = {"size": (16, 16), "pad": 8, "scale": 4, "alpha": np.full((16, 16), 128, np.uint8)}
        upscale.finish(job, network, self.destination)
        with Image.open(self.destination) as image:
            self.assertEqual(image.size, (64, 64))
            self.assertEqual(image.getpixel((0, 0)), (50, 100, 150, 128))
        self.assertTrue(upscale.replacement_valid(self.destination, self.source))

    def test_bad_network_dimensions_leave_existing_image_intact(self):
        network = self.folder / "network.png"
        Image.new("RGB", (16, 16)).save(network)
        upscale.publish_png(Image.new("RGBA", (64, 64), "red"), self.destination)
        before = self.destination.read_bytes()
        job = {"size": (16, 16), "pad": 8, "scale": 4, "alpha": np.full((16, 16), 255, np.uint8)}
        with self.assertRaises(ValueError):
            upscale.finish(job, network, self.destination)
        self.assertEqual(self.destination.read_bytes(), before)

    def test_interrupted_state_publication_preserves_last_valid_model_records(self):
        destination = self.folder / "upscale_state.json"
        upscale.publish_json({"abc.png": "old-model"}, destination)
        before = destination.read_bytes()
        with patch.object(upscale.os, "replace", side_effect=PermissionError("file in use")):
            with self.assertRaises(PermissionError):
                upscale.publish_json({"abc.png": "new-model"}, destination)
        self.assertEqual(destination.read_bytes(), before)
        self.assertEqual(list(self.folder.glob(".*.tmp")), [])
        upscale.publish_json({"abc.png": "new-model"}, destination)
        self.assertEqual(json.loads(destination.read_text()), {"abc.png": "new-model"})

    def test_masks_stay_excluded_with_archive_paths_and_saved_hash_fallback(self):
        index = {"1111111111111111": r"resources\vehicle\decal.xbpf:vinyl@0x20 P8 64x64 L1",
                 "2222222222222222": "assets_x.dat:resources/vehicle/decal2.xbpf:vinyl P8 64x64 L1",
                 "3333333333333333": "resources/vehicle/decal2.xbpf:art DXT5 64x64 L1"}
        (self.folder / "source_index.json").write_text(json.dumps(index))
        (self.folder / "vinyl_hashes.txt").write_text("4444444444444444\ninvalid text\n")
        self.assertEqual(upscale.excluded_masks(self.folder),
                         {name + ".png" for name in ("1111111111111111", "2222222222222222", "4444444444444444")})
        self.assertIn(upscale.texture_name("1111111111111111.png"), upscale.excluded_masks(self.folder))
        self.assertIn(upscale.texture_name("1111111111111111.PNG"), upscale.excluded_masks(self.folder))
        (self.folder / "source_index.json").unlink()
        self.assertEqual(upscale.excluded_masks(self.folder), {"4444444444444444.png"})

    def test_original_source_wins_over_dump_and_replacement_is_never_input(self):
        for directory in ("source", "dump", "replace"):
            (self.folder / directory).mkdir()
            (self.folder / directory / "same.png").write_bytes(b"unused fixture")
        self.assertEqual(upscale.source_paths(self.folder), {"same.png": self.folder / "source" / "same.png"})

    def test_named_pass_rejects_changed_source_or_output_and_missing_provenance(self):
        upscale.publish_png(Image.new("RGBA", (64, 64), "red"), self.destination)
        record = {"source_sha256": upscale.file_digest(self.source),
                  "output_sha256": upscale.file_digest(self.destination)}
        self.assertFalse(upscale.pass_complete(None, self.source, self.destination))
        self.assertTrue(upscale.pass_complete(record, self.source, self.destination))
        original = self.source.read_bytes()
        self.source.write_bytes(original + b"changed")
        self.assertFalse(upscale.pass_complete(record, self.source, self.destination))
        self.source.write_bytes(original)
        upscale.publish_png(Image.new("RGBA", (64, 64), "blue"), self.destination)
        self.assertFalse(upscale.pass_complete(record, self.source, self.destination))
        self.destination.unlink()
        self.assertFalse(upscale.pass_complete(record, self.source, self.destination))

    def test_named_pass_resumes_originals_and_never_sends_masks_to_the_model(self):
        textures = self.folder / "textures"
        originals = textures / "source"
        originals.mkdir(parents=True)
        name = "ABCDEF0123456789.png"
        mask = "1111111111111111.png"
        art = np.arange(16 * 16 * 3, dtype=np.uint8).reshape(16, 16, 3)
        Image.fromarray(art).save(originals / name)
        Image.fromarray(art).save(originals / mask)
        (textures / "vinyl_hashes.txt").write_text(Path(mask).stem)
        runner = self.folder / "runner.exe"
        runner.touch()
        model = "ultrasharp-4x"
        for suffix in (".param", ".bin"):
            (self.folder / (model + suffix)).write_bytes(b"model fixture")

        def fake_network(command, **_kwargs):
            input_dir = Path(command[command.index("-i") + 1])
            output_dir = Path(command[command.index("-o") + 1])
            self.assertEqual([path.name for path in input_dir.glob("*.png")], [name])
            with Image.open(input_dir / name) as padded:
                padded.resize((padded.width * 4, padded.height * 4)).save(output_dir / name)
            return subprocess.CompletedProcess(command, 0, "", "")

        args = ["upscale.py", "--textures", str(textures), "--pass-id", "fresh-pass"]
        with (patch.object(upscale, "ESRGAN", runner), patch.dict(upscale.MODELS, {model: self.folder}),
              patch.object(upscale.sys, "argv", args),
              patch.object(upscale.subprocess, "run", side_effect=fake_network) as network,
              redirect_stdout(io.StringIO())):
            self.assertEqual(upscale.main(), 0)
            self.assertEqual(network.call_count, 1)
            self.assertFalse((textures / "replace" / mask).exists())
            checked = audit.audit(textures, model, "fresh-pass")
            self.assertTrue(checked["complete"])
            self.assertEqual(checked["counts"]["complete_named_pass"], 1)
            network.reset_mock()
            self.assertEqual(upscale.main(), 0)
            network.assert_not_called()
            Image.fromarray(255 - art).save(originals / name)
            self.assertEqual(upscale.main(), 0)
            self.assertEqual(network.call_count, 1)
            (self.folder / (model + ".bin")).write_bytes(b"changed model")
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                upscale.main()
            self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
