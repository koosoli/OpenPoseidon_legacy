"""Synthetic wire, weight, envelope and fail-closed tests; no retail assets required."""

import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np

import inspect_stock_corpse_rig as rig


def array(fmt, values):
    return struct.pack("<I", len(values)) + b"".join(struct.pack("<" + fmt, *value) for value in values)


def wire_model(points, selections=(), resolution=0.0, roles=(-1,) * 12):
    """One complete small ODOL7 member, following the source wire layout."""
    empty = struct.pack("<I", 0)
    data = bytearray(b"ODOL" + struct.pack("<II", 7, 1))
    data += empty * 2 + array("3f", points) + array("3f", [(0, 1, 0)] * len(points))
    data += bytes(48) + empty * 6  # bounds/textures/edges/faces-offset/sections
    data += struct.pack("<I", len(selections))
    for name, indices, weights in selections:
        data += name.encode("ascii") + b"\0" + empty * 3 + b"\0" + empty
        data += array("H", [(index,) for index in indices])
        data += array("B", [(weight,) for weight in weights])
    data += empty * 2 + bytes(12) + empty  # properties/frames/end data/proxies
    data += struct.pack("<f", resolution) + bytes(144)
    data += struct.pack("<5Bb", 0, 0, 0, 0, 1, 0) + empty + bytes(16)
    data += struct.pack("<12b", *roles)
    return bytes(data)


def archive(path, members):
    header = b"".join(name.encode("ascii") + b"\0" + struct.pack("<5I", 0, 0, 0, 0, len(data))
                      for name, data in members)
    path.write_bytes(header + b"\0" + bytes(20) + b"".join(data for _, data in members))


class StockCorpseRigTests(unittest.TestCase):
    def setUp(self):
        self.points = [(0, 0, -1), (0, .2, 0), (.2, 0, 0), (0, 0, 1)]

    def test_complete_wire_parse_coverage_and_absent_weight_fallback(self):
        data = wire_model(self.points, [("hlava", [0, 1], []), ("krk", [1, 2], [1, 255])],
                          roles=(0, 0, 0, 0) + (-1,) * 8)
        model = rig.inspect_model(data)
        coverage = model["lods"][0]["coverage"]
        self.assertEqual(model["specialLodIndices"]["fireGeometry"], 0)
        self.assertEqual(coverage["anatomicalVertices"], 3)
        self.assertEqual(coverage["unweightedVertices"], 1)
        self.assertEqual(coverage["sourceInfluenceHistogram"], {0: 1, 1: 2, 2: 1})
        self.assertFalse(model["runtimeRigAdmitted"])
        self.assertEqual(model["fallback"], "authored death animation")

    def test_declared_weights_must_not_silently_be_replaced_by_full_membership(self):
        self.assertEqual(rig.selection_weights(dict(indices=[1], weights=[]), 2), {1: 255})
        self.assertEqual(rig.selection_weights(dict(indices=[1], weights=[0]), 2), {1: 0})
        for selection in [dict(indices=[0, 1], weights=[255]), dict(indices=[1, 1], weights=[]),
                          dict(indices=[2], weights=[]), dict(indices=[-1], weights=[])]:
            with self.assertRaises(rig.Refused):
                rig.selection_weights(selection, 2)

    def test_weighted_capsule_encloses_every_positive_point_even_tiny_outliers(self):
        points = np.array(self.points + [(0, 2, 0), (99, 99, 99)])
        envelope = rig.fit_capsule(points, [255, 255, 255, 255, 1, 0])
        self.assertTrue(envelope["finiteEnvelope"])
        self.assertEqual(envelope["pointCount"], 5)
        start, end = np.array(envelope["start"]), np.array(envelope["end"])
        delta = end - start
        for point in points[:5]:
            projection = np.clip((point - start) @ delta / (delta @ delta), 0, 1)
            self.assertLessEqual(np.linalg.norm(point - (start + projection * delta)),
                                 envelope["radius"] + 1e-9)
        self.assertFalse(envelope["jointFrameAdmitted"])

    def test_rotated_translated_envelope_is_not_a_world_axis_box(self):
        a = .7
        rotation = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
        points = np.array([(0, 0, -2), (.1, 0, -2), (0, .1, 2), (0, 0, 2)])
        rotation = rotation @ np.array([[0, 0, 1], [0, 1, 0], [-1, 0, 0]])
        original = rig.fit_capsule(points, [1] * 4)
        rotated = rig.fit_capsule(points @ rotation.T + [5, -3, 9], [1] * 4)
        self.assertAlmostEqual(original["radius"], rotated["radius"])
        self.assertAlmostEqual(original["segmentLength"], rotated["segmentLength"])

    def test_degenerate_empty_and_nonfinite_fits_refuse(self):
        for points, weights in [([(0, 0, 0)] * 3, [1] * 3), ([(0, 0, i) for i in range(3)], [1] * 3),
                                (self.points, [0] * 4), (self.points[:2], [1] * 2)]:
            self.assertFalse(rig.fit_capsule(points, weights)["finiteEnvelope"])
        for points, weights in [([(0, 0, float("nan"))] * 3, [1] * 3),
                                (self.points, [1, -1, 1, 1]), (self.points, [1, 1])]:
            with self.assertRaises(rig.Refused):
                rig.fit_capsule(points, weights)

    def test_proxy_construction_triangle_does_not_inflate_physical_envelope(self):
        points = self.points + [(0, 100, 0), (0, 99, 0), (0, 100, 1)]
        data = wire_model(points, [("hrudnik", list(range(7)), []),
                                  ("proxy:flag.01", [4, 5, 6], [])])
        lod = rig.parse_odol7(data)["lods"][0]
        envelope = rig.envelope_for(lod, ("hrudnik",))
        self.assertEqual(envelope["excludedProxyVertices"], 3)
        self.assertEqual(envelope["pointCount"], 4)
        self.assertLess(envelope["radius"], 1)
        self.assertEqual(rig.lod_coverage(lod)["anatomicalVertices"], 7)

    def test_overlap_is_only_anchor_evidence_and_missing_overlap_stays_unresolved(self):
        lod = rig.parse_odol7(wire_model(self.points, [("bricho", [0, 1], []),
                                                       ("zebra", [1, 2], [])]))["lods"][0]
        candidates = rig.joint_candidates(lod)
        self.assertEqual(candidates[0]["sharedPositiveVertices"], 1)
        np.testing.assert_allclose(candidates[0]["overlapCentroid"], self.points[1], atol=1e-8)
        self.assertEqual(candidates[1]["sharedPositiveVertices"], 0)
        self.assertTrue(all(not candidate["jointFrameAdmitted"] for candidate in candidates))

    def test_source_weights_report_top_four_and_quantization_hazards(self):
        names = rig.STOCK_BONES[:5]
        data = wire_model(self.points, [(name, [0, 1], [255, 1]) for name in names])
        coverage = rig.lod_coverage(rig.parse_odol7(data)["lods"][0])
        self.assertEqual(coverage["overFourSourceInfluences"], 2)
        self.assertEqual(coverage["onlyQuantizedZeroInfluences"], 1)

    def test_corrupt_or_unsupported_wire_is_refused_before_partial_admission(self):
        valid = wire_model(self.points)
        candidates = [valid[:-1], valid + b"extra", b"ODOL" + struct.pack("<II", 49, 1),
                      wire_model(self.points, resolution=float("inf")),
                      wire_model(self.points, roles=(5,) + (-1,) * 11),
                      wire_model([(float("nan"), 0, 0)])]
        for data in candidates:
            with self.subTest(size=len(data)), self.assertRaises(rig.Refused):
                rig.inspect_model(data)

    def test_distance_backreferences_and_checksum_are_verified(self):
        decoded = b"abcabcabc"
        data = b"\x07abc\x03\x03" + struct.pack("<I", sum(decoded))
        reader = rig.Reader(data)
        self.assertEqual(reader.decompress(len(decoded)), decoded)
        reader.finish()
        with self.assertRaises(rig.Refused):
            rig.Reader(data[:-1] + b"\x01").decompress(len(decoded))
        with self.assertRaises(rig.Refused):
            rig.Reader(b"").decompress(rig.MAX_MEMBER + 1)

    def test_archive_scope_and_failed_model_keep_authored_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.pbo"
            valid = wire_model(self.points, [("hlava", [0, 1, 2, 3], [])])
            archive(path, [("mc good.p3d", valid), ("mc bad.p3d", b"MLOD"), ("house.p3d", valid)])
            report = rig.census(path)
            self.assertEqual(report["requestedCandidates"], 2)
            self.assertEqual(report["parsedCandidates"], 1)
            self.assertEqual(report["runtimeRigsAdmitted"], 0)
            self.assertTrue(all(model["fallback"] == "authored death animation" for model in report["models"]))
            missing = rig.census(path, ["missing.p3d"])
            self.assertFalse(missing["models"][0]["parsed"])
            # Parsed arrays/handles never survive a run: a second census reflects
            # current bytes, rather than reusing stale geometry from a cache.
            archive(path, [("mc good.p3d", b"ODOL")])
            repeated = rig.census(path)
            self.assertEqual(repeated["parsedCandidates"], 0)
            self.assertFalse(repeated["models"][0]["runtimeRigAdmitted"])
            json.dumps(repeated, allow_nan=False)

    def test_archive_duplicate_and_invalid_range_refuse(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.pbo"
            archive(path, [("mc a.p3d", b"ODOL"), ("MC A.P3D", b"ODOL")])
            with self.assertRaises(rig.Refused):
                rig.archive_index(path)
            archive(path, [("mc a.p3d", b"ODOL")])
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaises(rig.Refused):
                rig.archive_index(path)


if __name__ == "__main__":
    unittest.main()
