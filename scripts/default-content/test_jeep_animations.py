import json
from pathlib import Path
import struct
import unittest

import numpy as np

from generate_jeep_animations import clip, elbow, encode, point, pose


class JeepAnimations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.spec = json.loads((root / 'content/default-packs/vehicle-actions/jeep-pose.json').read_text())

    def test_all_clips_are_rigid_and_zero_root_motion(self):
        for kind in ('idle', 'raise', 'aim', 'recoil', 'reload', 'lower'):
            data = encode(self.spec, clip(self.spec, kind))
            self.assertEqual(data[:8], b'RTM_0101')
            self.assertEqual(struct.unpack_from('<3f2i', data, 8), (0, 0, 0, 31, 25))
            self.assertEqual(len(data), 28 + 25*32 + 31*(4 + 25*80))

    def test_lower_body_never_drifts(self):
        baseline = pose(self.spec, 0)
        legs = [n for n in self.spec['bones'] if any(s in n for s in ('chodidlo', 'prsty', 'holen', 'stehno', 'zadek'))]
        for kind in ('raise', 'recoil', 'reload', 'lower'):
            for _, matrices in clip(self.spec, kind):
                for bone in legs:
                    np.testing.assert_array_equal(matrices[bone], baseline[bone])

    def test_raise_lower_and_reload_endpoints_match(self):
        low, high = pose(self.spec, 0), pose(self.spec, 1)
        for kind, start, end in [('raise', low, high), ('lower', high, low), ('reload', high, high), ('recoil', high, high)]:
            phases = list(clip(self.spec, kind))
            for name in self.spec['bones']:
                np.testing.assert_allclose(phases[0][1][name], start[name], atol=1e-12)
                np.testing.assert_allclose(phases[-1][1][name], end[name], atol=1e-12)

    def test_elbow_preserves_segment_lengths_and_rejects_stretch(self):
        joint = elbow([0, 0, 0], [.4, 0, 0], .3, .3, [0, -1, 0])
        self.assertAlmostEqual(np.linalg.norm(joint), .3)
        self.assertAlmostEqual(np.linalg.norm(joint-[.4, 0, 0]), .3)
        with self.assertRaises(ValueError):
            elbow([0, 0, 0], [1, 0, 0], .3, .3, [0, -1, 0])

    def test_hands_remain_on_authored_grips(self):
        for _, matrices in clip(self.spec, 'raise'):
            gun = matrices['zbran'] @ np.array(self.spec['weapon_proxy'])
            for side in ('p', 'l'):
                wrist = point(matrices[side+'ruka'], self.spec['arms'][side]['wrist'])
                np.testing.assert_allclose(wrist, point(gun, self.spec['grips'][side]), atol=1e-9)


if __name__ == '__main__':
    unittest.main()
