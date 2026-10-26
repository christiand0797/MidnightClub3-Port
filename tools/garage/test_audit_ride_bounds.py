"""Synthetic metadata fixtures, never retail payload bytes or a game/GPU launch."""
import math
import struct
import unittest

import audit_ride_bounds as audit


class BoundMetadataTests(unittest.TestCase):
    def fixture(self):
        data = bytearray(0x4200)
        base, description, bound, vertices, polygons = 0x82000000, 0x100, 0x4000, 0x4080, 0x40E0
        def word(at, value):
            struct.pack_into("<I", data, at, value)
        def ptr(at, value):
            word(at, base+value-0x80)
        word(0, base); word(0xC, len(data)-0x80)
        ptr(0x88, description); ptr(0x8C, bound)
        word(description+0x3808, 4)
        ptr(description+0x3818, 0x4120); ptr(description+0x381C, 0x4140)
        struct.pack_into("<f", data, 0x4134, .30); struct.pack_into("<f", data, 0x4154, .35)
        data[bound+4] = 3
        word(bound+0x4C, 8); word(bound+0x50, 1)
        ptr(bound+0x68, vertices); ptr(bound+0x6C, polygons)
        shape = ((-1, 0, -2), (1, 0, -2), (1, 0, 2), (-1, 0, 2),
                 (-1, .3, -2), (1, .3, -2), (1, .3, 2), (-1, .3, 2))
        for index, point in enumerate(shape):
            struct.pack_into("<3f", data, vertices+12*index, *point)
        struct.pack_into("<4H", data, polygons+0x10, 0, 1, 2, 3)
        return data

    def test_baked_pointer_mapping_layer_and_independent_radii(self):
        row = audit.parse_bound(self.fixture())
        self.assertEqual(row["bound_file_offset"], 0x4000)
        self.assertEqual(row["vertices_file_offset"], 0x4080)
        self.assertEqual(row["bottom_indices"], [0, 1, 2, 3])
        self.assertEqual(row["type"], 4)
        self.assertAlmostEqual(row["cap_y"], .25)
        self.assertEqual(row["stock_axle_radii"], [audit.f32(.30), audit.f32(.35)])

    def test_strict_threshold_excludes_equal_vertex_from_both_layers(self):
        epsilon = audit.f32(.05)
        row = audit.layer_summary(((0, 0, 0), (99, epsilon, 0), (2, audit.f32(.1), 0)))
        self.assertEqual(row["bottom_indices"], [0])
        self.assertEqual(row["cap_y"], audit.f32(audit.f32(.1)-epsilon))
        self.assertEqual(row["upper_half_x"], 2)  # Equal-threshold vertex must not widen upper envelope.

    def test_threshold_arithmetic_rounds_to_sse_float32(self):
        minimum = audit.f32(.327949)
        upper = audit.f32(minimum+audit.f32(.05))
        row = audit.layer_summary(((0, minimum, 0), (99, upper, 0), (2, audit.f32(.7), 0)))
        self.assertEqual(row["upper_threshold"], upper)
        self.assertEqual(row["bottom_indices"], [0])
        self.assertEqual(row["upper_half_x"], 2)

    def test_no_upper_layer_and_nonfinite_refused(self):
        for vertices in ((), ((0, 0, 0),), ((0, math.nan, 0),), ((math.inf, 0, 0), (0, .3, 0))):
            with self.subTest(vertices=vertices), self.assertRaises(ValueError):
                audit.layer_summary(vertices)

    def test_payload_extent_pointer_type_and_indices_refused(self):
        for offset, value in ((0xC, 0), (0x8C, 1), (0x404C, 0), (0x404C, 0x10001),
                              (0x4068, 0x82000000-4), (0x3918, 0), (0x4134, 0x7F800000)):
            data = self.fixture()
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                audit.parse_bound(data)
        data = self.fixture(); data[0x4004] = 2
        with self.assertRaisesRegex(ValueError, "bound type"):
            audit.parse_bound(data)
        data = self.fixture(); struct.pack_into("<H", data, 0x40F0, 8)
        with self.assertRaisesRegex(ValueError, "polygon index"):
            audit.parse_bound(data)

    def test_overlap_arrays_refused(self):
        data = self.fixture()
        struct.pack_into("<I", data, 0x406C, 0x82000000+0x4080-0x80)
        with self.assertRaisesRegex(ValueError, "overlapping"):
            audit.parse_bound(data)

    def test_skin_parser_retains_authored_clearance_or_constructor_default(self):
        self.assertEqual(audit.scalar("BoundClearance 0.08", "BoundClearance", .05), (audit.f32(.08), True))
        self.assertEqual(audit.scalar("BoundClearance 0.09", "BoundClearance", .05), (audit.f32(.09), True))
        self.assertEqual(audit.scalar("Mass 1400", "BoundClearance", .05), (audit.f32(.05), False))


if __name__ == "__main__":
    unittest.main()
