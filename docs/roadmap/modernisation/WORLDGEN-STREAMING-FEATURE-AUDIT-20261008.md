# CWR Worldgen and whole-Earth streaming: actual feature audit

Audit date: 2026-10-08. Upstream inspected from a fresh shallow checkout at
[`b46124fc9c810773b5402633168af4829240f143`](https://github.com/svamp3717/CWR-Worldgen/tree/b46124fc9c810773b5402633168af4829240f143)
(2026-10-03, `Test custom junction surface stacking`). Engine baseline inspected:
`0ff5618a`. This is a comparison of the actual upstream feature families, not a
claim that every upstream configuration option was enumerated or tested.

**All Worldgen features have NOT been transferred into whole-Earth streaming.**
Worldgen produces bounded playable islands offline; the Earth mode is a separate
runtime streamer. Reading its geographic inputs or rendering some equivalent
geometry is not the same as porting its asset generation, playability or output
pipeline. Existing WRP/P3D support can consume generated islands through the
normal island route; that does not instantiate their content in streamed Earth.

## License and evidence

The upstream package explicitly declares `GPL-3.0-or-later` in
[`pyproject.toml`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/pyproject.toml).
Its Python implementation files also carry the GPL SPDX identifier. The absence
of a standalone LICENSE file must not be reported as an absence of a license.
[`THIRD_PARTY_NOTICES.md`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/THIRD_PARTY_NOTICES.md)
describes MIT-derived OSM House Modeler behavior, provenance, procedural detail,
styles and textures. A future reuse must preserve applicable copyright/license
notices and verify each copied component's provenance. This implementation copies
**no upstream Worldgen code, country styles, textures or game binaries**; it uses
the existing engine coordinate/cache interfaces and independently generates
simple presentation meshes from locally cached OSM data.

Feature evidence is pinned to actual implementation/configuration:

- [`milestone9.py`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/src/cwr_worldgen/milestone9.py): forests, vegetation, barriers, street furniture, bridges, Overture, infill, bus stops, cemeteries, packaging and deployment.
- [`procedural_infrastructure.py`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/src/cwr_worldgen/procedural_infrastructure.py): road families, junctions, bridges, fences and generated model LODs.
- [`semantic_features.py`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/src/cwr_worldgen/semantic_features.py): bus-stop signs, sports/parking surfaces, graves and site grounding.
- [`source_pipeline.py`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/src/cwr_worldgen/source_pipeline.py), [`osm.py`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/src/cwr_worldgen/osm.py), [`procedural_buildings.py`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/src/cwr_worldgen/procedural_buildings.py): source acquisition, geographic conversion, playability, stock/procedural placement and generated buildings.
- [`README.md`](https://github.com/svamp3717/CWR-Worldgen/blob/b46124fc9c810773b5402633168af4829240f143/README.md): intended island workflow, elevation, coasts, materials, overview and RVW4/PBO outputs.

## Feature matrix

“First increment” below means the new bounded cached `EarthFeatureStream` helper
and its renderer integration, once built and runtime verified by the parent
session. CPU tests establish geometry/cache behavior only.

| Worldgen capability | Earth baseline | First increment / remaining gap |
|---|---|---|
| DEM acquisition, regridding, terrain | Independent streamed terrain height meshes | Existing Earth terrain retained; no Worldgen terrain pipeline port |
| Coastline, sea, beaches, shore transitions | Global ocean/terrain presentation | Separate renderer/ocean work; no Worldgen shoreline/material pipeline |
| Inland water | No geographic feature geometry | Simple closed OSM water ways and river/stream ribbons; no relations, holes, hydrology, terrain flattening or water simulation |
| Paved/dirt/gravel road geometry | No geographic roads | Separate paved/unpaved terrain-draped ribbon batches; heuristic width/classification and explicit width tags |
| Road junction topology, alignment and terrain grading | Absent | Absent; overlapping ribbons do not solve junctions or grades |
| Bridges and crossings | Absent | Absent; tagged bridges and tunnels are excluded rather than represented as ground roads |
| Roadway collision, navigation, AI road network | Absent | Absent; rendering road strips creates no AI route or physical Roadway LOD |
| OSM building footprints/heights | Bounded cached ways, plain solid extrusions | Existing building renderer retained; relation/multipolygon coverage remains absent |
| Overture building supplementation | Absent | Absent |
| Country/regional procedural architecture and materials | Absent | Absent; no Worldgen facade/roof/style generation port |
| Enterable buildings, floors/stairs/doors, Geometry/Paths LODs | Absent in streamed presentation | Absent; existing render shells are not playable building entities |
| Original/addon P3D building catalogues and measured fit | Absent in streamed Earth | Absent; normal island loading of P3Ds is a separate capability |
| Building foundations, grading and road clearance | Conservative shell grounding only | No building-road clearance or terrain pad solver |
| Forests and individual trees | Absent as geographic streamed features | Mapped forest/wood polygons receive bounded deterministic procedural tree proxies; imported explicit tree nodes supported |
| OFP forest blocks / local P3D vegetation assets | Absent in streamed Earth | Absent; prototypes are independent low-poly geometry, not retail assets |
| Undergrowth, forest borders, gap infill, bushes, rural vegetation | Absent geographically | Absent |
| Meadow grass, ditch grass, reeds, haybales | Engine has general grass features, no Worldgen mapped passes | No mapped vegetation passes added |
| Traffic signs / street furniture / bus stops | Absent geographically | Generic traffic-sign, stop and give-way post proxies; no sign text/symbols, orientation, street furniture or bus-stop asset generation |
| Fences, barriers, hedges and sidewalks | Absent geographically | Absent |
| Agricultural areas, parks and mapped ground uses | Imagery may depict them | No semantic land-use material/placement passes |
| Runways, parking, sports-field treatment | Imagery may depict them | No physical/semantic surface generation |
| Cemeteries and graves | Absent geographically | Absent |
| Terrain-material/vegetation style presets | Separate Earth imagery/material path | No Nogova/Kolgujev/Malden/Everon Worldgen texture-style pipeline |
| Town/place information and overview presentation | Named Earth origin presets, geographic navigation | No upstream place/overview asset generation |
| Deterministic cache/regeneration verification | Earth caches and focused CPU tests | Bounded geometry deterministic for a fixed origin/ground/cache; no Worldgen regeneration verifier port |
| RVW4/WRP, P3D/PAA, PBO generation and mod deployment | Engine can load existing island formats | No upstream exporter/package/GUI pipeline port; Earth feature meshes are transient |

## Bounded runtime contract

`EarthFeatureStream` reads only `features/<latitude-cell>_<longitude-cell>.json`
under the existing local world-stream cache. Cells are 0.02 degrees. A 3x3 cache
window is processed centre first. Geometry is generated on a worker; publication
uses request, origin revision and terrain generation tags so relocation cannot
publish obsolete coordinates. No runtime feature download is enabled.

Five independently tintable batches are exposed: paved roads, unpaved roads,
trees, signs and inland water. Every cell is capped at 4 MiB and 6000 visited
elements; the neighborhood shares 120000 triangles, 4000 tree instances and
16000 forest placement candidates. Ways are bounded to 2048 points; polygon
water/forest processing is limited to 256 points. Forests use fixed 32 m proxy
spacing. Roads follow sampled resident terrain in segments no longer than 8 m;
unsupported portions outside the resident patch are omitted. This is visual
content, with no entity collision, destruction, AI navigation or gameplay state.

Tree placement does not yet exclude road/building footprints. Water polygons
have one level estimated from shoreline terrain and reject more than 8 m of
shoreline relief; they may still be partially occluded by the unmodified DEM.
Waterway ribbons follow terrain and are illustrative, not hydrologically valid.
Polygons with relations/holes are intentionally unsupported. No textures or
asset LOD system are imported. Pole faces are generic and do not encode the
mapped sign's actual traffic instruction.

## Data preparation and service limits

Use `scripts/Prepare-EarthFeatures.ps1 -PrepareOnly` to inspect a request without
sending it. An explicit run prepares one cell. `-Neighborhood` explicitly expands
the one request to a 3x3 bounding box; it does not loop over cells. A central
neighborhood cache is retained while the camera enters adjoining cells. Dense
areas may exceed the cell byte/element limits; rejected output is not a runtime
cache. There is no automatic retry. `-InputFile <OSM-Overpass-JSON>` imports a
bounded local export without HTTP. `-IncludeIndividualTrees` adds mapped tree
nodes and can substantially increase density/size; forest polygons already
populate procedural trees without that flag. At most 64 runtime cell files are
prepared by the script. Raw cache files and generated geometry are not committed.

OSM data is [ODbL 1.0 with contributor attribution](https://www.openstreetmap.org/copyright).
The importer writes attribution and source/query/byte/time receipts. The runtime
presentation must retain visible OSM credit and access to the license link.
[Overpass's operator documentation](https://dev.overpass-api.de/overpass-doc/en/preface/commons.html)
explicitly rejects world scraping and public-server application backends. A
future global deployment needs licensed/regional extracts or an operated data
service and a normalized versioned feature-cell store; this prototype does not
make those public services its streaming backend.

Local experiment: a requested 3x3 Luxembourg area (49.58..49.64 N,
6.10..6.16 E, approximately 6.7 by 4.3 km) first exceeded the 6000-element
output limit when explicit tree nodes were included. One explicitly authorized
correction excluding tree nodes still returned exactly 6000 elements and
3976296 bytes at 2026-10-08T06:32:07.3820330Z. Both were rejected, not silently
claimed complete or published. The second request has a local attempt receipt;
the first predates receipt-on-rejection support and its exact byte count was not
retained. No bulk query/retry loop was run. Existing cached buildings cover only
49.60..49.62 N, 6.12..6.14 E and are independent from feature coverage.

After diagnosing the dense-way count and obtaining authorization for one narrower
request, `scripts/Prepare-EarthFeatures.ps1 -Latitude 49.61 -Longitude 6.13`
prepared the complete **centre-cell selection only**, 49.60..49.62 N,
6.12..6.14 E (approximately 2.2 by 1.4 km). No further requests were made. The
selection includes full geometries of intersecting ways, so vertices can extend
outside that selection; it does not imply complete neighboring-cell coverage.
The cache is `%LOCALAPPDATA%/OpenPoseidon/world-stream/features/2480_306.json`:

- Prepared UTC: `2026-10-08T06:34:13.8291390Z`.
- Size: 2179796 bytes. Elements: 3392 (19 nodes, 3373 ways).
- Mapped source families: 3259 road ways, 79 forest/wood ways, 35 water-feature ways and 19 sign nodes.
- SHA256: `78FC20B2A9FC57023509AD8DEFC32A051F0D63B39CA9AFEED30A2D0A3F8692DE`.
- Query/attribution/size/UTC receipts accompany the cache. The two rejected
  neighborhood responses were discarded by the previous importer version; their
  raw tags cannot be retrospectively inspected. Future rejections retain a
  bounded diagnostic response outside runtime cell filenames.
- CPU conversion with synthetic 300 m ground produced 1021 paved road features
  (22818 triangles), 1525 unpaved road features (26712 triangles), 438 procedural
  trees (3504 triangles), 19 signs (304 triangles), and 25 inland-water features
  (1166 triangles). Skipped unsupported/oversized/outside-resident features mean
  those counts are lower than the source selection. Runtime terrain can alter
  acceptance and counts; these numbers do not establish visual appearance.

The geographic feature coverage target of 5–10 km was therefore **not achieved**
in this increment. Neighborhood residency preserves this cached area as the
camera crosses a cell boundary but does not invent missing geographic data.

## Verification

`scripts/Test-EarthFeatures.ps1 -VcpkgDirectory <configured build dependency path>`
compiles the actual helper independently and checks terrain sampling and clipping,
bridge exclusion, mapped forests, explicit trees/signs, water geometry, finite
vertices, triangle winding, deterministic indices, shared geometry/tree budgets,
3x3 residency, cross-cell OSM type/ID deduplication, cancellation and worker
relocation revision safety. An optional `-CellFile` parses the installed cache
against a clearly labeled synthetic flat resident terrain; it is not evidence
of in-game grounding or correct appearance.

Full build, deployment and runtime verification belong to the parent session,
the sole game-folder owner. This audit/helper commit alone is not a deployed or
visually accepted feature.
