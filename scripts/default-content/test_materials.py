import json
from pathlib import Path
import unittest

import numpy as np

from generate_materials import encode_nohq, generate


class MaterialTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2]
        self.specs = json.loads((root / "content/default-packs/visual/materials.json").read_text())["materials"]

    def test_deterministic_and_bounded(self):
        for spec in self.specs.values():
            first, second = generate(spec, 64), generate(spec, 64)
            for kind in first:
                np.testing.assert_array_equal(first[kind], second[kind])
            self.assertTrue(np.isfinite(first["height"]).all())
            self.assertEqual(first["nohq"][:, :, 0].max(), 0)
            self.assertGreater(first["roughness"].min(), 200)

    def test_flat_nohq_uses_alpha_green_not_red(self):
        result = encode_nohq(np.zeros((32, 32)), [4, 4])
        np.testing.assert_array_equal(result[0, 0], [0, 128, 255, 128])

    def test_height_and_normal_ignore_road_colour_wear(self):
        spec = self.specs["road_asphalt"]
        road = generate(spec, 64)
        plain = generate(dict(spec, road=False), 64)
        np.testing.assert_array_equal(road["height"], plain["height"])
        np.testing.assert_array_equal(road["nohq"], plain["nohq"])
        self.assertFalse(np.array_equal(road["co"], plain["co"]))

    def test_periodic_normal_and_sign(self):
        u = np.arange(64) / 64
        height = np.broadcast_to(0.01 * np.sin(2 * np.pi * u), (64, 64))
        result = encode_nohq(height, [1, 1])
        self.assertLess(result[0, 0, 3], 128)
        np.testing.assert_array_equal(encode_nohq(np.roll(height, 7, 1), [1, 1]), np.roll(result, 7, 1))

    def test_sand_ripples_are_relief_not_baked_light(self):
        spec = self.specs["sand"]
        rippled = generate(spec, 128)
        flat = generate(dict(spec, rippleMetres=0.0), 128)
        np.testing.assert_array_equal(rippled["co"], flat["co"])
        np.testing.assert_array_equal(rippled["roughness"], flat["roughness"])
        self.assertFalse(np.array_equal(rippled["height"], flat["height"]))
        self.assertFalse(np.array_equal(rippled["nohq"], flat["nohq"]))

    def test_field_track_retains_open_centre_and_shoulders(self):
        maps = generate(self.specs["gravel"], 128)
        alpha = maps["co"][:, :, 3]
        self.assertLess(alpha[:, 64].max(), 2)
        self.assertLess(alpha[:, 0].max(), 2)
        self.assertLess(alpha[:, -1].max(), 2)
        self.assertGreater(alpha[:, 34].min(), 240)
        self.assertGreater(alpha[:, 93].min(), 240)

    def test_reject_invalid_dimensions(self):
        with self.assertRaises(ValueError):
            generate(self.specs["soil"], 100)
        with self.assertRaises(ValueError):
            generate(dict(self.specs["soil"], tileMetres=[0, 4]), 64)


if __name__ == "__main__":
    unittest.main()
