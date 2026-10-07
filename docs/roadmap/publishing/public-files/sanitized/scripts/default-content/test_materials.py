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

    def test_detail_normal_has_physical_scale_and_matching_catalog(self):
        root = Path(__file__).resolve().parents[2]
        catalog = json.loads((root / "content/default-packs/visual/material-overrides.json").read_text())
        entry = next(m for m in catalog["materials"] if m["source"] == "eden\\ps.paa")
        spec = self.specs["sand_detail"]
        self.assertEqual(spec["tileMetres"], [entry["detailNormalMetres"]] * 2)
        self.assertEqual(entry["detailNormal"], "op_ground_materials\\sand_detail_nohq.paa")
        self.assertLess(spec["heightMetres"], 0.001)
        maps = generate(spec, 128)
        self.assertGreater(maps["nohq"][:, :, 3].std(), 1.0)
        self.assertEqual(maps["nohq"][:, :, 0].max(), 0)

    def test_original_sand_additions_retain_authored_albedo_and_sources(self):
        root = Path(__file__).resolve().parents[2]
        catalog = json.loads((root / "content/default-packs/visual/material-overrides.json").read_text())
        entries = {entry["source"].lower(): entry for entry in catalog["materials"]}
        self.assertEqual(len(entries), len(catalog["materials"]))
        census = json.loads((root / "tests/fixtures/original-surfaces.json").read_text())
        for source, archive, world, surface_class in (
            ("abel\\pi.paa", "Dta\\Abel.pbo", "abel", "SandAbel"),
            ("o\\pt.paa", "AddOns\\O.pbo", "noe", "SandDark"),
            ("o\\ps.paa", "AddOns\\O.pbo", "noe", "Sand"),
        ):
            entry = entries[source]
            self.assertEqual(entry["albedo"], source)
            self.assertEqual(entry["sourceArchive"], archive)
            palette = next(p for p in census["maps"][world]["qualifyingPalette"] if p["texture"] == source)
            self.assertTrue(palette["pureSand"])
            self.assertFalse(palette["pureMud"])
            self.assertEqual(palette["classes"], [surface_class] * 4)
        # Default/unknown, rock and mixed tiles do not acquire a whole-tile override.
        self.assertFalse(any(source.startswith("cain\\") for source in entries))
        self.assertNotIn("abel\\pb.paa", entries)
        self.assertNotIn("o\\psptpspt.pac", entries)

    def test_sand_additions_share_normal_scale_without_new_generated_assets(self):
        root = Path(__file__).resolve().parents[2]
        catalog = json.loads((root / "content/default-packs/visual/material-overrides.json").read_text())
        entries = {entry["source"]: entry for entry in catalog["materials"]}
        original = entries["eden\\ps.paa"]
        self.assertEqual(self.specs["sand"]["tileMetres"], [50.0, 50.0])
        for source in ("abel\\pi.paa", "o\\pt.paa", "o\\ps.paa"):
            for field in ("normal", "detailNormal", "detailNormalMetres"):
                self.assertEqual(entries[source][field], original[field])
            metres = entries[source]["detailNormalMetres"]
            self.assertEqual(self.specs["sand_detail"]["tileMetres"], [metres, metres])
            self.assertEqual(50.0 / metres, 100.0)
        package = (root / "scripts/Build-DefaultContent.ps1").read_text()
        self.assertIn("@('road_asphalt','sand','gravel')", package)
        self.assertIn("$expected += 'sand_detail_nohq.paa'", package)

    def test_original_albedo_detail_uses_existing_guarded_loader_contract(self):
        # Static source contract, not a simulated loader or installed render proof.
        root = Path(__file__).resolve().parents[2]
        bank = (root / "engine/WgpuRenderer/TextureBankWgpu.cpp").read_text()
        resolve = bank.split("TextureBankWgpu::ResolveLegacyMaterial", 1)[1].split("TextureBankWgpu::LegacyNormalPath", 1)[0]
        self.assertIn("_legacyMaterials.find(MaterialPathKey(name))", resolve)
        self.assertIn("QIFStreamB::AutoBank(name)", resolve)
        self.assertIn("original->GetOpenName()", resolve)
        self.assertIn("MaterialPathKey(actual.generic_string().c_str()) != MaterialPathKey(expected.generic_string().c_str())", resolve)
        load = bank.split("Ref<Texture> TextureBankWgpu::Load(RStringB name)", 1)[1]
        self.assertIn("texture->SetName(name)", load)
        self.assertIn("texture->_sourceName = material->albedo.c_str()", load)
        self.assertIn("_textureByName.emplace(texture->GetName(), iFree)", load)
        self.assertIn("if (index >= 0)", load)
        header = (root / "engine/WgpuRenderer/TextureWgpu.hpp").read_text()
        self.assertIn("_sourceName.GetLength() ? (const char*)_sourceName : Name()", header)
        source = (root / "engine/WgpuRenderer/TextureWgpu.cpp").read_text()
        self.assertIn("factory->Create(SourceName(), _mipmaps, MAX_MIPMAPS)", source)
        # Missing original albedo cannot leave a normal-only half material bound.
        self.assertIn("if (!_src && _sourceName.GetLength())", source)
        self.assertIn("if (loaded >= 0 && !_texture[loaded]->_sourceName.GetLength())", bank)
        self.assertIn("repeatsPerMetre = 1.0f / material->detailNormalMetres", bank)


if __name__ == "__main__":
    unittest.main()
