import unittest

import numpy as np

import inspect_stock_corpse_parts as parts
import inspect_stock_corpse_rig as rig


class AuthoredParts(unittest.TestCase):
    def lod(self):
        return dict(points=np.array([[0., 0., 0.], [1., 2., 3.], [2., 0., 1.]]),
                    selections={"component01": {0: 255, 1: 255},
                                "hlava": {0: 255, 1: 255}})

    def test_full_weight_single_bone_is_evidence_only(self):
        result = parts.components(self.lod())[0]
        self.assertEqual(result["exclusiveFullWeightBone"], "hlava")
        self.assertEqual(result["boundsMax"], [1., 2., 3.])

    def test_overlap_and_partial_membership_are_not_rigid_ownership(self):
        lod = self.lod()
        lod["selections"]["krk"] = {1: 1}
        self.assertIsNone(parts.components(lod)[0]["exclusiveFullWeightBone"])
        del lod["selections"]["krk"]
        lod["selections"]["hlava"][1] = 254
        self.assertIsNone(parts.components(lod)[0]["exclusiveFullWeightBone"])
        del lod["selections"]["hlava"][1]
        self.assertIsNone(parts.components(lod)[0]["exclusiveFullWeightBone"])

    def test_empty_and_nonfinite_refuse(self):
        lod = self.lod()
        lod["selections"]["component01"] = {0: 0}
        with self.assertRaises(rig.Refused):
            parts.components(lod)
        lod = self.lod()
        lod["points"][1, 0] = float("nan")
        with self.assertRaises(rig.Refused):
            parts.components(lod)


if __name__ == "__main__":
    unittest.main()
