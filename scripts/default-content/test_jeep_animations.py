import json
from pathlib import Path
import struct
import unittest
import re

import numpy as np

from generate_jeep_animations import clip, elbow, encode, frame, point, pose, rest_point, rest_proxy, rotation


class JeepAnimations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.spec = json.loads((root / 'content/default-packs/vehicle-actions/jeep-pose.json').read_text())
        cls.source = root / 'content/default-packs/vehicle-actions'

    def test_default_package_references_only_generated_clips(self):
        config = (self.source / 'config.cpp').read_text()
        files = re.findall(r'file\s*=\s*"([^";]+)"', config)
        kinds = ('idle', 'raise', 'aim', 'recoil', 'reload', 'lower')
        self.assertEqual(set(files), {f'\\op_jeep_actions\\{prefix}_{kind}.rtm'
                                      for prefix in ('jeep', 'uh60') for kind in kinds})
        self.assertEqual(len(files), 2 * len(kinds))
        manifest = json.loads((self.source / 'mod.json').read_text())
        self.assertEqual(manifest['defaultContentApi'], '1')
        self.assertTrue(manifest['version'])
        self.assertNotIn('op_jeep_animation_preview', config)

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
            gun = matrices['zbran'] @ rest_proxy(self.spec) @ frame(rotation([0, 1, 0], -90))
            for side in ('p', 'l'):
                palm = point(matrices[side+'ruka'], rest_point(self.spec, self.spec['arms'][side]['palm']))
                np.testing.assert_allclose(palm, point(gun, self.spec['grips'][side]), atol=1e-9)

    def test_wrists_join_forearms_without_gaps(self):
        for kind in ('idle', 'raise', 'reload', 'lower'):
            for _, matrices in clip(self.spec, kind):
                for side in ('p', 'l'):
                    wrist = rest_point(self.spec, self.spec['arms'][side]['wrist'])
                    np.testing.assert_allclose(point(matrices[side+'ruka'], wrist),
                                               point(matrices[side+'loket'], wrist), atol=1e-9)

    def test_rtm_pivots_are_not_centered_mesh_coordinates(self):
        hip = rest_point(self.spec, self.spec['legs']['p']['hip'])
        self.assertLess(np.linalg.norm(hip), .2)
        self.assertGreater(np.linalg.norm(self.spec['legs']['p']['hip']), .6)

    def test_actual_m16_barrel_points_out_the_right_side(self):
        rifle = pose(self.spec, 1)['zbran'] @ rest_proxy(self.spec)
        direction = rifle[:3, :3] @ [-1, 0, 0]
        # AnimationRT applies the OFP X/Z reversal during loading.
        np.testing.assert_allclose(direction * [-1, 1, -1], [1, 0, 0], atol=1e-9)

    def test_relaxed_muzzle_points_down_not_into_roof(self):
        rifle = pose(self.spec, 0)['zbran'] @ rest_proxy(self.spec)
        direction = (rifle[:3, :3] @ [-1, 0, 0]) * [-1, 1, -1]
        self.assertLess(direction[1], -.4)
        self.assertGreater(direction[0], .9)

    def test_raised_head_faces_the_rifle_without_moving_neck_axis(self):
        matrices = pose(self.spec, 1)
        rifle = matrices['zbran'] @ rest_proxy(self.spec)
        # Head's -X is already in engine space: RTM load conjugates its matrix
        # by the X/Z reversal. The stored proxy/rifle bind frame is source-space.
        flip = np.diag([-1, 1, -1])
        np.testing.assert_allclose(flip @ matrices['hlava'][:3, :3] @ flip @ [-1, 0, 0],
                                   flip @ rifle[:3, :3] @ [-1, 0, 0], atol=1e-9)
        neck = rest_point(self.spec, self.spec['waist']) + [0, .55, 0]
        for _, matrices in clip(self.spec, 'raise'):
            np.testing.assert_allclose(point(matrices['hlava'], neck),
                                       point(matrices['hrudnik'], neck), atol=1e-9)

    def test_reflections_are_not_valid_bone_rotations(self):
        matrices = pose(self.spec, 1)
        matrices['hlava'][:3, 0] *= -1
        with self.assertRaises(ValueError):
            encode(self.spec, [(0, matrices)])


class UH60Animations(JeepAnimations):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.spec.update(json.loads((cls.source / 'uh60-pose.json').read_text()))


if __name__ == '__main__':
    unittest.main()
