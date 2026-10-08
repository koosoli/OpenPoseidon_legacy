# Bounded offline regional OSM source

`scripts/Import-EarthOsmExtract.py` reads one local OSM PBF and selects one
geographic rectangle. It makes no network requests, calls no Overpass endpoint,
launches no game and generates no game assets. Use the regional JSON as actual
Worldgen input, then generate and mount that output through the separate
Worldgen preparation/launcher path.

The official [Geofabrik Luxembourg download page](https://download.geofabrik.de/europe/luxembourg.html)
provides a country PBF. On 2026-10-08 the dated `luxembourg-261006.osm.pbf`
was 47,655,019 bytes, below the approved 128 MiB single-country download bound.
The local external lab holds it at
`D:/OpenPoseidon-Worldgen-Lab-20261008/extracts/luxembourg-261006.osm.pbf`.
Its SHA-256 is
`9fe9c62dc9a3c1a9ab2c9ae573ca7f2e4708cac4b1e4bca5d2f3c0cb713847aa`.
The adjacent `.osm.receipt.json` records URL, HTTP modification date, download
timestamp, bytes and attribution. This is the only country download made for
this slice; importing another rectangle reuses the same local PBF.

OSM data is licensed under [ODbL 1.0](https://www.openstreetmap.org/copyright).
Keep `ATTRIBUTION.txt` and `SOURCE-RECEIPT.json` with prepared source/cells and
retain the appropriate contributor attribution and database licence when
sharing derived data. This repository contains the importer, not the extract,
stock game assets or generated regional outputs.

## Run

The optional [pyosmium](https://docs.osmcode.org/pyosmium/latest/user_manual/01-First-Steps/)
dependency can be installed in the separate lab dependency directory. It is
not a game or engine dependency. The verified local version is 4.3.1 for Python
3.11 on Windows. The importer does not install it automatically.

```powershell
$env:PYTHONPATH = 'D:/OpenPoseidon-Worldgen-Lab-20261008/python'
& 'C:/Program Files/Python311/python.exe' scripts/Import-EarthOsmExtract.py `
  --pbf 'D:/OpenPoseidon-Worldgen-Lab-20261008/extracts/luxembourg-261006.osm.pbf' `
  --bbox 49.59 6.00 49.64 6.08 `
  --output 'D:/OpenPoseidon-Worldgen-Lab-20261008/lux-mamer-5km-extract-complete'
```

The output directory must be new. The command scans the country a bounded three
times: tagged multipolygon references, complete regional ways/nodes, then full
member geometry for selected multipolygons. It copies values from pyosmium's
temporary object views. It selects bbox-overlapping features without clipping
their geometry, retaining road node references, outer/inner member roles and
all original tags. Large polygons containing the region and road segments with
both endpoints outside it remain available. The geographic selection may
include features whose bounding boxes overlap but whose actual geometry does
not; the downstream generator applies its own spatial clipping.

Outputs:

- `osm.json`: full regional geometry JSON in Worldgen's existing Overpass-shaped
  input format. The shape of this file does not imply a network query.
- `features/<latCell>_<lonCell>.json` and
  `buildings/<latCell>_<lonCell>.json`: `.02` degree cells using floor addressing,
  compatible with existing prepared-cache readers and a separately hosted
  optional HTTPS content provider. This tool does not install them in a cache,
  overwrite prepared priority cells or expose a server.
- `SOURCE-RECEIPT.json`: input hash/provenance, source/family counts, output hashes,
  explicit cell exclusions and geographic bounds; `ATTRIBUTION.txt` accompanies it.

Pass `osm.json` to the existing generator's `--osm-json` option and use the same
`--bbox`. Generate a real heightmap with that rectangle and the chosen grid;
the importer supplies no invented elevation. Worldgen's `--asset-root` must
reference the user's local stock installation. Use `--input-mode meters` and
`--heightmap-grid game-terrain-vertices` for the corresponding cached Terrarium
preparation contract. Never omit `--osm-json`, because upstream's default route
can fetch Overpass. Mount the resulting generated addon and point the Earth
adapter at its manifest to render the actual generated P3D placements. Regional
OSM cells alone are input features, not Worldgen models.

## Bounds and explicit limitations

One invocation accepts <=128 MiB input, <=12 km per rectangle axis, <=64 addressed
cells, <=8 million input node locations, <=30,000 indexed multipolygons and
<=200,000 member references. Regional output is <=100,000 source elements,
<=2 million geometry points and <=64 MiB JSON; individual ways are <=32,768
points. Exceeding these limits aborts; it never takes the first 6,000 records.
The importer does not loop over or generate a country.

Earth cells are checked against the current reader's 4 MiB limit and element
counts (6,000 features or 2,000 buildings). Overflow aborts before output creation.
`--worldgen-only` explicitly produces just the complete regional source if a
dense urban cell cannot fit the existing renderer's limit. It does not increase
runtime budgets. Missing-node ways are counted; a selected incomplete/nested
multipolygon aborts instead of silently dropping an outer or a hole.

The current Earth procedural cell readers do not handle multipolygon relations,
buildings with unsupported rings/vertex counts or full geometries outside their
`.06` degree cell-centre margin. The receipt names these exclusions; their
complete geometry remains in `osm.json` for Worldgen. The importer does not
fabricate clipped feature IDs or fill relation holes with trees. Ordinary OSM
tags not used by the cell presentation readers are still retained for Worldgen.
There is no automatic global output catalogue, regional model streaming or
worldwide populated dataset claim.

`tests/test_earth_osm_extract.py` checks real OSM node/way references, a crossing
road with no vertex in the selected bbox, a forest multipolygon with an inner
hole, explicit unsupported-cell reporting, overflow refusal, input receipt hash
mismatch and preservation of existing outputs. Tests with actual OSM parsing
require the optional pyosmium dependency; pure limit tests do not.

## Actual regional CPU receipt

The command above produced the Mamer/Bertrange region on 2026-10-08. Its complete
`osm.json` is 7,828,152 bytes with 17,777 source elements (5,620 nodes, 12,122 ways
and 35 multipolygons). The actual upstream Worldgen parser at `b46124fc` accepted
it, identifying 3,691 roads, 148 gravel roads, 5,514 building polygons, two building
points, 185 forest areas, 1,636 individual trees, 79 tree rows, 65 water areas,
179 watercourses and 28 place points, among its other supported families.
The local `WORLDGEN-PARSE-RECEIPT.json` records all parsed family counts.

All 40 emitted stream files fit current budgets: 5,400,459 bytes total,
752,948 bytes maximum per cell, 1,522 maximum building elements and 1,514 maximum
feature elements. The receipt lists 35 multipolygons unsupported by the current
procedural cell readers and two cell assignments outside their vertex margin;
their geometry remains complete in regional source. The input scan visited
4,237,239 country nodes and selected 112,791 regional geometry points.
All 40 files also passed actual `EarthContentSource::ValidCell` and the existing
C++ feature/building consumers using a synthetic resident 300-metre patch.
Per-cell totals were 5,615 buildings, 4,529 trees and 3,755 roads; these include
duplicated boundary features and are not unique regional or runtime counts.

An earlier city-adjacent rectangle `[49.59, 6.08, 49.64, 6.16]` was refused before
output creation: one cell contained 3,874 building ways, above the current 2,000
reader limit. This refusal is intentional and no source records were truncated.

The existing preparation script's 16,000-source-element cap is lower than this
complete region's count. Reuse this `osm.json` directly with a matching prepared
heightmap, or deliberately adapt that preparation contract; do not silently
remove records to fit it. No 5 km game region was generated or visually accepted
by this import check. Generated output must also respect the Earth Worldgen
adapter's separate 131,072-source-object and regional asset budgets; denser output
may need bounded partitioning before any wider geographic provider work.
