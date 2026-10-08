import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("earth_ground_roads", Path(__file__).parents[1]/"scripts/Classify-EarthGroundRoads.py")
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)

BBOX = (0, 0, .01, .01, 1000)


def way(way_id, x=500, tags=None, geometry=None):
    return {"type": "way", "id": way_id, "tags": {"highway": "residential", **(tags or {})},
            "geometry": geometry or [{"lat": .004, "lon": x/100000}, {"lat": .006, "lon": x/100000}]}


def matrix(x=500, z=500):
    return [1, 0, 0, 0, 1, 0, 0, 0, 1, x, 100, z]


class ClassificationTests(unittest.TestCase):
    def classify(self, elements, m=None):
        roads = mod.Roads({"elements": elements}, BBOX)
        return mod.classify_record("o\\road\\sil25.p3d", m or matrix(), roads, 1000)

    def test_unique_ordinary_road(self):
        match, reason = self.classify([way(10)])
        self.assertEqual(reason, "accepted")
        self.assertEqual(match["osmWay"], 10)
        self.assertLess(match["maximumAssociationDistanceMetres"], 1e-8)
        self.assertTrue(mod.safe_tags({"bridge": "no", "tunnel": "false", "layer": "0"}))

    def test_elevated_and_uncertain_tags_preserved(self):
        for tags in ({"bridge": "yes"}, {"tunnel": "building_passage"}, {"covered": "yes"},
                     {"layer": "1"}, {"layer": "-1"}, {"layer": "unknown"}, {"level": "1"},
                     {"indoor": "yes"}, {"location": "elevated"}, {"bridge:structure": "beam"}):
            with self.subTest(tags=tags):
                match, reason = self.classify([way(10, tags=tags)])
                self.assertIsNone(match)
                self.assertEqual(reason, "bridge_tunnel_layer_or_elevated_tags")

    def test_multiple_ways_overlap_is_not_nearest_winner(self):
        for second in (way(11, x=503), way(11, tags={"bridge": "yes"})):
            match, reason = self.classify([way(10), second])
            self.assertIsNone(match)
            self.assertEqual(reason, "ambiguous_multiple_ways")

    def test_same_way_hairpin_ambiguity(self):
        points = [{"lat": .004, "lon": .005}, {"lat": .006, "lon": .005},
                  {"lat": .007, "lon": .007}, {"lat": .006, "lon": .00503},
                  {"lat": .004, "lon": .00503}]
        match, reason = self.classify([way(10, geometry=points)])
        self.assertIsNone(match)
        self.assertEqual(reason, "ambiguous_same_way_passages")

    def test_exact_five_metre_limit_not_accepted(self):
        self.assertIsNone(self.classify([way(10, x=505)])[0])
        self.assertIsNotNone(self.classify([way(10, x=504.9)])[0])

    def test_direction_bounds_pitch_and_model(self):
        horizontal = [{"lat": .005, "lon": .004}, {"lat": .005, "lon": .006}]
        self.assertEqual(self.classify([way(10, geometry=horizontal)])[1], "direction_mismatch")
        self.assertEqual(self.classify([way(10)], matrix(x=1))[1], "footprint_outside_source_bounds")
        pitched = matrix();pitched[7] = .1
        self.assertEqual(self.classify([way(10)], pitched)[1], "unsupported_model_or_transform")
        self.assertIsNone(mod.road_length("o/hous/house.p3d", matrix()))

    def test_fresh_sidecar_apply_backup_and_status(self):
        with tempfile.TemporaryDirectory(prefix="earth-road-classification-") as folder:
            root = Path(folder);(root/"source").mkdir()
            original = root/"manifest.json"
            document = {"height_scale": .05, "world": {"name": "fixture", "cells": 16, "cell_size": 16,
                        "bbox": [0, 0, .01, .01]}, "final_validation": {"status": "failed", "deployment_allowed": True}}
            original.write_bytes(mod.encoded(document))
            (root/"osm-source.json").write_bytes(mod.encoded({"elements": [way(10)]}))
            header = struct.pack("<4sii", b"4WVR", 16, 16)
            m = matrix(x=128, z=128)
            # This WRP uses a 256m extent: same .005 longitude source centre.
            record = struct.pack("<12fI76s", *m, 20, b"o\\road\\sil25.p3d")
            wrp = root/"source/fixture.wrp"
            wrp.write_bytes(header+b"\0"*(16*16*4+512*32)+record+b"\0"*128)
            before, wrp_before = original.read_bytes(), wrp.read_bytes()
            with mock.patch.object(sys, "argv", ["classify", "--manifest", str(original)]), contextlib.redirect_stdout(io.StringIO()):
                mod.main()
            sidecar = root/"manifest-earth-ground-roads.json"
            annotated = mod.read_json(sidecar)
            self.assertEqual(annotated["earth_ground_road_ids"], [20])
            self.assertEqual(annotated["final_validation"], document["final_validation"])
            self.assertEqual(original.read_bytes(), before)
            with mock.patch.object(sys, "argv", ["classify", "--manifest", str(original), "--apply"]), contextlib.redirect_stdout(io.StringIO()):
                mod.main()
            self.assertEqual(mod.read_json(original)["earth_ground_road_ids"], [20])
            backups = list(root.glob("manifest.before-ground-roads-*.json"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_bytes(), before)
            self.assertEqual(wrp.read_bytes(), wrp_before)

    def test_input_budget_and_missing_geometry(self):
        with self.assertRaises(ValueError):
            mod.Roads({"elements": [way(10), way(10)]}, BBOX)
        roads = mod.Roads({"elements": [{"type": "way", "id": 1, "tags": {"highway": "residential"}, "nodes": [1, 2]}]}, BBOX)
        self.assertEqual(roads.missing_geometry, 1)
        with self.assertRaises(ValueError):
            mod.bounds({"world": {"bbox": [0, 179, 1, -179], "cells": 16, "cell_size": 16}})


if __name__ == "__main__":
    unittest.main()
