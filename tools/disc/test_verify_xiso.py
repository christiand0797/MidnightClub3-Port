"""Synthetic XDVDFS fixtures; no copyrighted assets or source-image writes."""

import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import verify_xiso as verifier


def fixture(path, base=0):
    xbe = bytearray(512)
    xbe[:4] = b"XBEH"
    struct.pack_into("<I", xbe, 0x104, 0x10000)
    struct.pack_into("<I", xbe, 0x118, 0x10180)
    struct.pack_into("<I", xbe, 0x188, 0x54540079)
    title = "Test disc".encode("utf-16-le")
    xbe[0x18C:0x18C + len(title)] = title
    image = bytearray(37 * verifier.SECTOR)
    volume = 32 * verifier.SECTOR
    image[volume:volume + 20] = verifier.MAGIC
    image[volume + 0x7EC:volume + 0x800] = verifier.MAGIC
    struct.pack_into("<II", image, volume + 20, 33, 52)
    # The root's second node begins at 28 and contains a directory.
    struct.pack_into("<HHIIBB", image, 33 * verifier.SECTOR, 0, 7, 34, len(xbe), 0, 11)
    image[33 * verifier.SECTOR + 14:33 * verifier.SECTOR + 25] = b"default.xbe"
    struct.pack_into("<HHIIBB", image, 33 * verifier.SECTOR + 28, 0, 0, 35, 25, 0x10, 4)
    image[33 * verifier.SECTOR + 42:33 * verifier.SECTOR + 46] = b"cars"
    image[34 * verifier.SECTOR:34 * verifier.SECTOR + len(xbe)] = xbe
    struct.pack_into("<HHIIBB", image, 35 * verifier.SECTOR, 0, 0, 36, 7, 0, 11)
    image[35 * verifier.SECTOR + 14:35 * verifier.SECTOR + 25] = b"vehicle.dat"
    image[36 * verifier.SECTOR:36 * verifier.SECTOR + 7] = b"carDATA"
    path.write_bytes(b"\0" * base + image)
    return bytes(xbe)


class DiscVerificationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.iso = self.root / "test.iso"
        self.xbe = fixture(self.iso)
        self.assets = self.root / "assets"
        (self.assets / "CARS").mkdir(parents=True)
        (self.assets / "DEFAULT.XBE").write_bytes(self.xbe)
        (self.assets / "CARS" / "vehicle.dat").write_bytes(b"carDATA")

    def mutate(self, offset, data):
        with self.iso.open("r+b") as image:
            image.seek(offset)
            image.write(data)

    def test_inventory_title_and_complete_hash_verification(self):
        before = hashlib.sha256(self.iso.read_bytes()).hexdigest()
        with verifier.Xiso(self.iso) as disc:
            info = disc.xbe_info()
            self.assertEqual(info["title_id"], "0x54540079")
            self.assertEqual(info["title"], "Test disc")
            self.assertEqual(info["sha1"], hashlib.sha1(self.xbe).hexdigest())
            result = verifier.verify_assets(disc, self.assets, hashes=True)
            self.assertTrue(result["ok"])
            self.assertEqual(result["hashed_files"], 2)
        self.assertEqual(before, hashlib.sha256(self.iso.read_bytes()).hexdigest())

    def test_equal_size_corruption_requires_hashes(self):
        (self.assets / "CARS" / "vehicle.dat").write_bytes(b"BADdata")
        with verifier.Xiso(self.iso) as disc:
            self.assertTrue(verifier.verify_assets(disc, self.assets)["ok"])
            result = verifier.verify_assets(disc, self.assets, hashes=True)
        self.assertEqual(result["problems"][0]["problem"], "SHA-256 mismatch")

    def test_missing_extra_and_wrong_types_are_reported(self):
        (self.assets / "CARS" / "vehicle.dat").unlink()
        (self.assets / "DEFAULT.XBE").unlink()
        (self.assets / "DEFAULT.XBE").mkdir()
        (self.assets / "extra.txt").write_text("extra")
        with verifier.Xiso(self.iso) as disc:
            result = verifier.verify_assets(disc, self.assets)
        self.assertEqual({p["problem"] for p in result["problems"]}, {"missing", "extra", "wrong type"})

    def test_bad_signature_extent_and_cycle_are_rejected(self):
        self.mutate(32 * verifier.SECTOR, b"invalid!")
        with self.assertRaises(verifier.DiscError):
            with verifier.Xiso(self.iso):
                pass
        fixture(self.iso)
        self.mutate(33 * verifier.SECTOR + 8, struct.pack("<I", 0xFFFFFFFF))
        with self.assertRaisesRegex(verifier.DiscError, "beyond"):
            with verifier.Xiso(self.iso):
                pass
        fixture(self.iso)
        self.mutate(33 * verifier.SECTOR + 28 + 2, struct.pack("<H", 7))
        with self.assertRaisesRegex(verifier.DiscError, "repeated"):
            with verifier.Xiso(self.iso):
                pass

    def test_unsafe_path_and_directory_alias_are_rejected(self):
        self.mutate(33 * verifier.SECTOR + 14, b"../evil.xbe")
        with self.assertRaisesRegex(verifier.DiscError, "Unsafe"):
            with verifier.Xiso(self.iso):
                pass
        fixture(self.iso)
        self.mutate(33 * verifier.SECTOR + 28 + 4, struct.pack("<II", 33, 52))
        with self.assertRaisesRegex(verifier.DiscError, "aliased"):
            with verifier.Xiso(self.iso):
                pass

    def test_extraction_is_complete_idempotent_and_never_overwrites(self):
        destination = self.root / "default.xbe"
        with verifier.Xiso(self.iso) as disc:
            self.assertEqual(verifier.extract_xbe(disc, destination), "extracted")
            self.assertEqual(destination.read_bytes(), self.xbe)
            before = destination.stat().st_mtime_ns
            self.assertEqual(verifier.extract_xbe(disc, destination), "already matches")
            self.assertEqual(before, destination.stat().st_mtime_ns)
            destination.write_bytes(b"preserve this")
            with self.assertRaisesRegex(verifier.DiscError, "Refusing to overwrite"):
                verifier.extract_xbe(disc, destination)
        self.assertEqual(destination.read_bytes(), b"preserve this")
        self.assertFalse(list(self.root.glob(".default.xbe-*")))

    def test_extraction_race_does_not_replace_destination(self):
        destination = self.root / "default.xbe"
        original_link = verifier.os.link

        def concurrent_destination(source, target):
            Path(target).write_bytes(b"another writer")
            original_link(source, target)

        with verifier.Xiso(self.iso) as disc, patch.object(verifier.os, "link", concurrent_destination):
            with self.assertRaises(FileExistsError):
                verifier.extract_xbe(disc, destination)
        self.assertEqual(destination.read_bytes(), b"another writer")
        self.assertFalse(list(self.root.glob(".default.xbe-*")))

    def test_nonzero_partition_offset(self):
        fixture(self.iso, base=4096)
        with patch.object(verifier, "PARTITIONS", (0, 4096)), verifier.Xiso(self.iso) as disc:
            self.assertEqual(disc.base, 4096)
            self.assertEqual(disc.xbe_info()["sha1"], hashlib.sha1(self.xbe).hexdigest())


if __name__ == "__main__":
    unittest.main()
