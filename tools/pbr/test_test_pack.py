"""Checks tools/pbr/test_pack.py on a synthetic capture and texture tree (needs Pillow + numpy, e.g. py -3.12).

    py -3.12 -I -m unittest discover -s tools/pbr -p test_test_pack.py
"""
import hashlib
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_pack as T  # noqa: E402

HEAD = "draw rt 03D4C000 ds 03BFA000 prim 6 count 83 vs B0105721 ps 00000000 blend {b} 0 0 zen 1 203 zwrite {z} " \
       "zbias 0 0.00 cull 1 cw F c0 0.000 0.000 0.000 0.000 op 0 sten 1 207 FF FF"
STAGE = " | t{i} 8172E500 fmt C 4x2 addr 11 filt 332 cop {cop} hash {h} tci {tci}"
FIXED, PROGRAM = "AAAAAAAAAAAAAAA1", "BBBBBBBBBBBBBBB2"
PS = "CCCCCCCCCCCCCCC3"
OPAQUE, VARYING, MISSING = "00000000000000A1", "00000000000000A2", "00000000000000A3"


def draw(n, vs=FIXED, blend=0, zwrite=1, stages=((4, OPAQUE, 0),)):
    line = f"{n:04d} " + HEAD.format(b=blend, z=zwrite)
    line += "".join(STAGE.format(i=i, cop=c, h=h, tci=t) for i, (c, h, t) in enumerate(stages))
    return [line, "     vb0 stride 20 v0 C1A00021", f"     keys vs {vs} ps {PS}"]


class TestPack(unittest.TestCase):
    def setUp(self):
        from PIL import Image
        import numpy as np
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.native, self.cap = root / "native", root / "native" / "captures" / "f1"
        self.cap.mkdir(parents=True)
        (self.cap / f"vs_{FIXED}.hlsl").write_text("float4 cp = mul(ip, wvp);")
        (self.cap / f"vs_{PROGRAM}.hlsl").write_text("float4 cp = r0;")
        lines = draw(1) + draw(2) + draw(3, stages=((4, OPAQUE, 1),))          # two assignments
        lines += draw(4, vs=PROGRAM) + draw(5, blend=1) + draw(6, zwrite=0)    # program, blended, no z write
        lines += draw(7, stages=((2, OPAQUE, 0),))                             # select, not modulate
        lines += draw(8, stages=((4, OPAQUE, 0), (4, OPAQUE, 0)))              # two stages
        lines += draw(9, stages=((4, "0000000000000000", 0),))                 # not packable
        lines += draw(10, stages=((4, VARYING, 0),)) + draw(11, stages=((4, MISSING, 0),))
        (self.cap / "frame.txt").write_text("\n".join(lines) + "\n")
        tex = self.native / "textures"
        (tex / "source").mkdir(parents=True)
        (tex / "replace" / "sub").mkdir(parents=True)
        px = np.zeros((2, 4, 4), np.uint8)
        px[..., 0], px[..., 3] = 200, 255
        Image.fromarray(px, "RGBA").save(tex / "source" / f"{OPAQUE}.png")
        hd = np.zeros((4, 8, 4), np.uint8)
        hd[..., 1], hd[..., 3] = 100, 77
        Image.fromarray(hd, "RGBA").save(tex / "replace" / "sub" / f"{OPAQUE.lower()}_hd.png")
        px[0, 0, 3] = 0
        Image.fromarray(px, "RGBA").save(tex / "source" / f"{VARYING}.png")
        (self.native / "settings.toml").write_text(
            "[graphics]\ntexture_pack = false\n\n[pbr]\napproved_manifest_sha256 = ''\nenabled = false\n"
            "materials = []\npack_path = ''\nquality = 'ultra'\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_candidates_follow_the_capture_gates(self):
        found = T.candidates(self.cap)
        self.assertEqual(found, {(FIXED, PS, OPAQUE, 0): 2, (FIXED, PS, OPAQUE, 1): 1,
                                 (FIXED, PS, VARYING, 0): 1, (FIXED, PS, MISSING, 0): 1})

    def test_pack_matches_the_loader_contract(self):
        self.assertEqual(T.main([str(self.cap), "--native", str(self.native), "--style", "neutral", "--install"]), 0)
        pack = self.native / "pbr-test-neutral"
        manifest = (pack / "materials-v1.toml").read_bytes()
        digest = hashlib.sha256(manifest).hexdigest()
        text = manifest.decode()
        self.assertEqual(text.count("[[materials]]"), 3)  # the missing source is skipped; varying alpha is kept
        self.assertIn(f'texture = "{VARYING}"', text)
        self.assertIn(f'texture = "{OPAQUE}"', text)
        self.assertIn("uv_set = 1", text)
        for name, sha in re.findall(r'file = "([^"]+)", sha256 = "([0-9a-f]{64})"', text):
            self.assertEqual(hashlib.sha256((pack / name).read_bytes()).hexdigest(), sha)
        # Texture pack off: the console source is the accepted colour, its alpha digest unchanged.
        alpha = hashlib.sha256(struct.pack("<II", 4, 2) + bytes([255]) * 8).hexdigest()
        self.assertIn(f'baseline_alpha_sha256 = "{alpha}"', text)
        settings = (self.native / "settings.toml").read_text()
        self.assertIn(f"approved_manifest_sha256 = '{digest}'", settings)
        self.assertIn("pack_path = 'pbr-test-neutral'", settings)
        self.assertIn("enabled = true", settings.split("[pbr]")[1])
        self.assertIn("texture_pack = false", settings)
        self.assertTrue((self.native / "settings.toml.bak").exists())

    def test_hd_replacement_is_the_accepted_colour_and_marked_keeps_alpha(self):
        from PIL import Image
        import numpy as np
        s = self.native / "settings.toml"
        s.write_text(s.read_text().replace("texture_pack = false", "texture_pack = true"))
        self.assertEqual(T.main([str(self.cap), "--native", str(self.native)]), 0)
        albedo = np.asarray(Image.open(self.native / "pbr-test-marked" / f"{OPAQUE}_albedo.png"))
        self.assertEqual(albedo.shape, (4, 8, 4))
        self.assertTrue((albedo[..., 3] == 77).all())
        self.assertEqual(tuple(albedo[0, 0, :3]), (102, 60, 102))  # 0.6*(0,100,0) + 0.4*magenta

    def test_constant_alpha_only_and_merged_captures(self):
        other = self.native / "captures" / "f2"
        other.mkdir()
        (other / f"vs_{FIXED}.hlsl").write_text("float4 cp = mul(ip, wvp);")
        (other / "frame.txt").write_text("\n".join(draw(1) + draw(2, stages=((4, OPAQUE, 2),))) + "\n")
        out = self.native / "only"
        self.assertEqual(T.main([str(self.cap), str(other), "--native", str(self.native), "--out", str(out),
                                 "--constant-alpha-only"]), 0)
        text = (out / "materials-v1.toml").read_text()
        self.assertNotIn(VARYING, text)
        self.assertIn("uv_set = 2", text)                    # from the second capture
        self.assertIn("# 3 captured draw(s)", text)          # uv 0 counted across both captures

    def test_old_capture_says_why(self):
        (self.cap / "frame.txt").write_text(f"0001 {HEAD.format(b=0, z=1)} | t0 8172E500 fmt C 4x2 addr 11 filt 332 cop 4\n")
        with self.assertRaises(SystemExit) as e:
            T.main([str(self.cap), "--native", str(self.native)])
        self.assertIn("predates", str(e.exception))


if __name__ == "__main__":
    unittest.main()
