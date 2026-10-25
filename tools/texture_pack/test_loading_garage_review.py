import sys
from pathlib import Path
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import loading_garage_review as review


class BoundaryStatisticsTests(unittest.TestCase):
    def test_flat_image_and_injected_grid_step_are_distinguished(self):
        image = np.zeros((192, 192, 3), np.uint8)
        flat = review.line_statistics(image, 1, 96)
        self.assertEqual(flat["mean_rgb_jump"], 0.)
        image[:, 96:] = 40
        step = review.line_statistics(image, 1, 96)
        self.assertEqual(step["mean_rgb_jump"], 40.)
        self.assertEqual(step["excess_over_nearby_p95"], 40.)
        horizontal = review.line_statistics(image.transpose(1, 0, 2), 0, 96)
        self.assertEqual(horizontal["mean_rgb_jump"], step["mean_rgb_jump"])

    def test_broad_gradient_has_no_unique_boundary_spike(self):
        image = np.repeat(np.arange(192, dtype=np.uint8)[None, :, None], 192, axis=0)
        image = np.repeat(image, 3, axis=2)
        result = review.line_statistics(image, 1, 96)
        self.assertEqual(result["ratio_to_nearby_median"], 1.)
        self.assertEqual(result["excess_over_nearby_p95"], 0.)
        with self.assertRaises(ValueError):
            review.line_statistics(image, 1, 0)


if __name__ == "__main__":
    unittest.main()
