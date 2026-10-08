"""CPU tests: real OSM reference resolution, holes, crossing roads, cell limits."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "earth_extract", Path(__file__).resolve().parents[1] / "scripts/Import-EarthOsmExtract.py")
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)

BBOX = [49.60, 6.10, 49.62, 6.12]


def fixture(path):
    # No road vertex lies inside BBOX; the full segment crosses the region.
    nodes = [(1, 49.61, 6.095), (2, 49.61, 6.125),
             (3, 49.612, 6.112), (4, 49.613, 6.112), (5, 49.613, 6.113), (6, 49.612, 6.113),
             (7, 49.595, 6.095), (8, 49.625, 6.095), (9, 49.625, 6.125), (10, 49.595, 6.125),
             (11, 49.605, 6.105), (12, 49.606, 6.105), (13, 49.606, 6.106), (14, 49.605, 6.106)]
    data = ['<osm version="0.6">']
    data += [f'<node id="{i}" lat="{lat}" lon="{lon}"/>' for i, lat, lon in nodes]
    data += ['<node id="15" lat="49.61" lon="6.11"><tag k="natural" v="tree"/></node>']
    ways = [(20, [1, 2], '<tag k="highway" v="residential"/>'),
            (21, [3, 4, 5, 6, 3], '<tag k="building" v="house"/>'),
            (22, [7, 8, 9, 10, 7], ''), (23, [11, 12, 13, 14, 11], '')]
    for i, refs, tags in ways:
        data += [f'<way id="{i}">' + ''.join(f'<nd ref="{ref}"/>' for ref in refs) + tags + '</way>']
    data += ['<relation id="30"><member type="way" ref="22" role="outer"/>'
             '<member type="way" ref="23" role="inner"/><tag k="type" v="multipolygon"/>'
             '<tag k="landcover" v="trees"/></relation></osm>']
    path.write_text(''.join(data), encoding="utf-8")


class Limits(unittest.TestCase):
    def test_geographic_and_input_bounds(self):
        for bbox in ([0, 0, 1, 1], [49, 179, 49.01, -179], [float('nan'), 1, 2, 3]):
            with self.assertRaises(ValueError):
                extract.validate_bbox(bbox)

    def test_cells_refuse_overflow_without_truncation(self):
        polygon = [{'lat': 49.61, 'lon': 6.11}, {'lat': 49.611, 'lon': 6.11},
                   {'lat': 49.611, 'lon': 6.111}, {'lat': 49.61, 'lon': 6.11}]
        rows = [{'type': 'way', 'id': i, 'tags': {'building': 'yes'}, 'geometry': polygon}
                for i in range(2001)]
        with self.assertRaisesRegex(ValueError, 'no truncation'):
            extract.prepare_documents(rows, BBOX)

    def test_large_geometry_exclusion_is_explicit(self):
        row = {'type': 'way', 'id': 99, 'tags': {'natural': 'wood'},
               'geometry': [{'lat': 49.61, 'lon': 6.11}] * 2049}
        documents, exclusions = extract.prepare_documents([row], BBOX)
        self.assertEqual(exclusions['featurePointLimitInEarthCells'], [99])
        self.assertTrue(all(not json.loads(data)['elements'] for data in documents.values()))
        self.assertEqual(len(row['geometry']), 2049)


@unittest.skipUnless(importlib.util.find_spec('osmium'), 'Optional pyosmium dependency not installed')
class Wire(unittest.TestCase):
    def test_real_osm_references_holes_and_crossing_way(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / 'fixture.osm'
            fixture(path)
            out = root / 'region'
            receipt = extract.write_region(path, out, BBOX)
            rows = json.loads((out / 'osm.json').read_bytes())['elements']
            self.assertEqual({(r['type'], r['id']) for r in rows},
                             {('node', 15), ('way', 20), ('way', 21), ('relation', 30)})
            road = next(r for r in rows if r['id'] == 20)
            self.assertEqual(road['nodes'], [1, 2])
            self.assertEqual([p['lon'] for p in road['geometry']], [6.095, 6.125])
            forest = next(r for r in rows if r['id'] == 30)
            self.assertEqual([m['role'] for m in forest['members']], ['outer', 'inner'])
            self.assertTrue(all(len(m['geometry']) == 5 for m in forest['members']))
            self.assertEqual(receipt['cellExclusions']['multipolygonsUnsupportedByEarthCells'], [30])
            self.assertEqual(receipt['sourceElements'], 4)
            self.assertTrue(receipt['cellFiles'])
            self.assertTrue((out / 'ATTRIBUTION.txt').exists())
            self.assertTrue(any(item['elements'] for key, item in receipt['cellFiles'].items()
                                if key.startswith('buildings/')))
            with self.assertRaisesRegex(ValueError, 'never overwritten'):
                extract.write_region(path, out, BBOX)

    def test_receipt_mismatch_prevents_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / 'fixture.osm'
            fixture(path)
            path.with_suffix('.receipt.json').write_text('{"sha256":"wrong"}')
            with self.assertRaisesRegex(ValueError, 'hash differs'):
                extract.write_region(path, root / 'region', BBOX)
            self.assertFalse((root / 'region').exists())

    def test_incomplete_selected_multipolygon_refuses_source(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'fixture.osm'
            fixture(path)
            path.write_text(path.read_text().replace('ref="23" role="inner"', 'ref="999" role="inner"'))
            with self.assertRaisesRegex(ValueError, 'unresolved/non-way members'):
                extract.read_extract(path, BBOX)


if __name__ == '__main__':
    unittest.main()
