import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_hd  # noqa: E402


class FindGameTests(unittest.TestCase):
    def test_remembered_disc_image_then_game_folder(self):
        with tempfile.TemporaryDirectory() as d:
            native = Path(d) / "game"
            (native / "config").mkdir(parents=True)
            iso = Path(d) / "MC3.iso"
            iso.write_bytes(b"x")
            (native / "config" / "disc-path.txt").write_text(f'"{iso}"\n', encoding="utf-8")
            self.assertEqual(build_hd.find_game(native, None), (None, iso))
            (native / "assets").mkdir()
            (native / "assets" / "default.xbe").write_bytes(b"x")
            self.assertEqual(build_hd.find_game(native, None), (native / "assets", None))

    def test_explicit_disc_wins_and_nothing_found(self):
        with tempfile.TemporaryDirectory() as d:
            native = Path(d)
            self.assertEqual(build_hd.find_game(native, "D:/x.iso"), (None, Path("D:/x.iso")))
            self.assertEqual(build_hd.find_game(native, None), (None, None))


class DiscFileTests(unittest.TestCase):
    def test_copies_only_what_the_steps_read(self):
        wanted = ["assets_x.dat", "assets_x.dat_5", "a.xbck", "b.XBPF", "media.xpr", "mc3intro.bik"]
        skipped = ["default.xbe", "music.xwb", "sound.xsb", "font.ttf", "map.xbpmap"]
        for name in wanted:
            self.assertTrue(build_hd.NEEDED.match(name), name)
        for name in skipped:
            self.assertFalse(build_hd.NEEDED.match(name), name)


if __name__ == "__main__":
    unittest.main()
