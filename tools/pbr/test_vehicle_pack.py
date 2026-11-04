"""Synthetic CPU-only tests; never touch native textures, settings or live jobs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest

SPEC = importlib.util.spec_from_file_location("vehicle_pack", Path(__file__).with_name("vehicle_pack.py"))
V = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = V
SPEC.loader.exec_module(V)

TEX, VS, PS = "0123456789ABCDEF", "1111111111111111", "2222222222222222"
SHA = "a" * 64


def draw(n=0, stage=1, tci=0, flags="", bridge=2, texture=TEX, vs=VS, ps=PS, atest=0):
    reflection_stage = 2 if stage == 0 else 0
    return (f"{n:04d} draw rt 01 ds 02 prim 6 count 30 vs B01057 ps 00 blend 0 0 0 zen 1 zwrite 1 atest {atest} 207 0"
            f" | t{reflection_stage} 81000000 fmt F 4x4 target addr 11 filt 221 cop 4 hash 9999999999999999 tci 0"
            f" | t{stage} 82000000 fmt F 4x4 {flags} addr 11 filt 221 cop 4 hash {texture} tci {tci}\n"
            f"     keys vs 3333333333333333 ps 4444444444444444\n"
            f"     vehicle-keys vs {vs} ps {ps} bridge {bridge}\n")


class VehicleProposal(unittest.TestCase):
    def setUp(self):
        from PIL import Image
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.maps, self.cap = self.root / "maps", self.root / "capture"
        self.maps.mkdir()
        self.cap.mkdir()
        (self.cap / "frame.txt").write_text(draw(), encoding="utf-8")
        self.records = {}
        for role, mode, color in (("normal", "RGB", (128, 128, 255)), ("ao", "L", 192),
                                  ("roughness", "L", 96), ("metallic", "L", 32)):
            path = self.maps / f"selected_{role}.png"
            Image.new(mode, (4, 4), color).save(path)
            self.records[role] = {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        self.inventory = self.root / "inputs.json"
        self.gate = self.root / "gate.json"
        self.gate_data = {"schema": "mc3-offline-pbr-gate-v1", "offline_pbr_adoption_allowed": True,
                          "strict_movies": {"completed": 106, "total": 106, "failed": 0, "final_audit_sha256": SHA},
                          "full_project_backup": {"all_files_verified": True, "manifest_sha256": SHA,
                                                  "verification_sha256": SHA}}
        self.write_inventory()
        self.gate.write_text(json.dumps(self.gate_data), encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def write_inventory(self):
        self.inventory.write_text(json.dumps({"schema": "mc3-vehicle-map-inputs-v1", "textures": {TEX: self.records}}),
                                  encoding="utf-8")

    def args(self, output="proposal"):
        return dict(captures=[self.cap], textures=[TEX], maps_root=self.maps, maps_manifest=self.inventory,
                    maps_sha256=V.digest(self.inventory.read_bytes()), output=self.root / output,
                    review_sha256=SHA, gate_receipt=self.gate, gate_sha256=V.digest(self.gate.read_bytes()),
                    normal_convention="opengl")

    def test_exact_capture_identity_stage_and_bridge_mask(self):
        text = draw() + draw(1) + draw(2, stage=0, bridge=1)
        found = V.capture_candidates(text.encode(), {TEX})
        self.assertEqual(found, {V.Assignment(TEX, VS, PS, 1): 2, V.Assignment(TEX, VS, PS, 0): 1})
        # A later non-draw event cannot inherit the previous texture.
        self.assertEqual(V.capture_candidates((draw() + "0001 clear rt 0\n"
            f"     vehicle-keys vs {VS} ps {PS} bridge 2\n").encode(), {TEX}),
            {V.Assignment(TEX, VS, PS, 1): 1})

    def test_unknown_bridge_target_wrong_uv_and_wildcards_rejected(self):
        for text in (draw(bridge=0), draw(bridge=4), draw(stage=1, bridge=1), draw(tci=1),
                     draw(flags="target"), draw(flags="unbound"), draw(flags="rect"), draw(vs="F" * 16),
                     draw(ps="0" * 16), draw().replace("vehicle-keys", "keys"), draw().replace("atest 0 207 0", "")):
            with self.subTest(text=text):
                self.assertFalse(V.capture_candidates(text.encode(), {TEX}))

    def test_proposal_matches_loader_schema_pins_maps_and_preserves_inputs(self):
        from PIL import Image
        before = {p: V.digest(p.read_bytes()) for p in self.root.rglob("*") if p.is_file()}
        result = V.build(**self.args())
        out = self.root / "proposal"
        manifest = (out / "materials-v1.toml").read_bytes()
        parsed = tomllib.loads(manifest.decode())
        row = parsed["materials"][0]
        self.assertEqual(parsed["schema"], 1)
        self.assertEqual((row["vertex_shader"], row["pixel_shader"], row["texture"]), (VS, PS, TEX))
        self.assertEqual((row["stage"], row["uv_set"], row["surface"], row["albedo"]), (1, 0, "vehicle", "game"))
        self.assertNotIn("height", row)
        self.assertNotIn("emission", row)
        self.assertFalse(row["cutout"])
        for role in ("normal", "orm"):
            self.assertEqual(V.digest((out / row[role]["file"]).read_bytes()), row[role]["sha256"])
        with Image.open(out / row["orm"]["file"]) as orm:
            self.assertEqual(orm.getpixel((0, 0)), (192, 96, 32, 255))
        self.assertEqual(result["manifest_sha256"], V.digest(manifest))
        self.assertEqual(result["status"], "proposal")
        self.assertIs(result["installed"], False)
        self.assertIs(result["native_rendering_accepted"], False)
        self.assertEqual(result["height_mode"], "none")
        self.assertEqual({p: V.digest(p.read_bytes()) for p in before}, before)

    def test_gate_failures_never_create_output(self):
        cases = (("offline_pbr_adoption_allowed", False), ("strict_movies", {"completed": 105, "total": 106,
                  "failed": 1, "final_audit_sha256": SHA}), ("full_project_backup", {"all_files_verified": False,
                  "manifest_sha256": SHA, "verification_sha256": SHA}))
        for key, value in cases:
            good = self.gate_data[key]
            self.gate_data[key] = value
            self.gate.write_text(json.dumps(self.gate_data), encoding="utf-8")
            with self.subTest(key=key), self.assertRaises(V.ProposalError):
                V.build(**self.args())
            self.assertFalse((self.root / "proposal").exists())
            self.gate_data[key] = good

    def test_tampered_receipts_or_maps_rejected_before_output(self):
        for field in ("gate_sha256", "maps_sha256"):
            args = self.args()
            args[field] = "b" * 64
            with self.subTest(field=field), self.assertRaises(V.ProposalError):
                V.build(**args)
            self.assertFalse(args["output"].exists())
        (self.maps / self.records["normal"]["file"]).write_bytes(b"partial generated data")
        with self.assertRaisesRegex(V.ProposalError, "map SHA-256 mismatch"):
            V.build(**self.args())
        self.assertFalse((self.root / "proposal").exists())

    def test_missing_and_mismatched_maps_are_not_resampled_or_silently_adopted(self):
        from PIL import Image
        del self.records["metallic"]
        self.write_inventory()
        V.build(**self.args("without-metal"))
        with Image.open(self.root / "without-metal" / f"{TEX}_orm.png") as image:
            self.assertEqual(image.getpixel((0, 0))[2], 0)
        path = self.maps / self.records["ao"]["file"]
        Image.new("L", (2, 2), 255).save(path)
        self.records["ao"]["sha256"] = V.digest(path.read_bytes())
        self.write_inventory()
        with self.assertRaisesRegex(V.ProposalError, "dimensions differ"):
            V.build(**self.args())
        self.assertFalse((self.root / "proposal").exists())

    def test_output_is_exclusive_and_path_escape_rejected(self):
        out = self.root / "proposal"
        out.mkdir()
        marker = out / "foreign.txt"
        marker.write_text("preserve me")
        with self.assertRaisesRegex(V.ProposalError, "exclusive"):
            V.build(**self.args())
        self.assertEqual(marker.read_text(), "preserve me")
        self.records["normal"]["file"] = "../outside.png"
        self.write_inventory()
        with self.assertRaisesRegex(V.ProposalError, "inside maps root"):
            V.build(**self.args("escape"))
        self.assertFalse((self.root / "escape").exists())

    def test_all_selected_textures_need_evidence(self):
        args = self.args()
        args["textures"].append("3333333333333333")
        with self.assertRaisesRegex(V.ProposalError, "every selected texture"):
            V.build(**args)
        self.assertFalse(args["output"].exists())

    def test_alpha_test_enable_remains_part_of_the_exact_material_identity(self):
        found = V.capture_candidates((draw() + draw(1, atest=1)).encode(), {TEX})
        self.assertEqual(found, {V.Assignment(TEX, VS, PS, 1): 1, V.Assignment(TEX, VS, PS, 1, 0, True): 1})
        (self.cap / "frame.txt").write_text(draw(atest=1), encoding="utf-8")
        V.build(**self.args())
        row = tomllib.loads((self.root / "proposal" / "materials-v1.toml").read_text())["materials"][0]
        self.assertIs(row["cutout"], True)

    def test_budget_is_checked_before_output_and_duplicate_stages_are_ambiguous(self):
        original = V.MAX_DECODED_BYTES
        V.MAX_DECODED_BYTES = 8
        try:
            with self.assertRaisesRegex(V.ProposalError, "decoded map/mip budget"):
                V.build(**self.args())
            self.assertFalse((self.root / "proposal").exists())
        finally:
            V.MAX_DECODED_BYTES = original
        duplicate = draw().replace("\n", f" | t1 83000000 fmt F 4x4 addr 11 filt 221 cop 4 hash {TEX} tci 0\n", 1)
        self.assertFalse(V.capture_candidates(duplicate.encode(), {TEX}))


if __name__ == "__main__":
    unittest.main()
