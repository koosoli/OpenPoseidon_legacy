# Cached Luxembourg imagery pyramid

The optional Earth path uses three explicitly prepared ACT orthophotos around
Luxembourg. Regional imagery reaches both the far and resident near meshes.
NASA remains the global overview outside prepared coverage. This is not
continuous worldwide detailed imagery or a complete playable globe.

## Prepare and enable

```powershell
./scripts/Prepare-EarthSurface.ps1 -Region Cell
./scripts/Prepare-EarthSurface.ps1 -Region District
./scripts/Prepare-EarthSurface.ps1 -Region Context
./scripts/Launch-WholeWorld.ps1 -Latitude 49.61 -Longitude 6.13 -Altitude 2000 -LocalSurface -CachedBuildings -CachedFeatures
```

Each missing region makes one manual JPEG WMS request, capped at 16 MiB, with
separate 60-second header/body deadlines and no retries. Verified image/receipt
pairs are reused. No imagery HTTP runs inside the game. Files and SHA256/source/
bounds receipts stay outside Git in `%LOCALAPPDATA%/OpenPoseidon/world-stream/surface/`.
Restart after preparing data; sources load once. Invalid sources preserve NASA.

## Source and coverage

Administration du cadastre et de la topographie (ACT), Luxembourg,
**ortho_2023, CC0 1.0**: [official orthophoto dataset](https://data.public.lu/fr/datasets/orthophoto-officielle-du-grand-duche-de-luxembourg-edition-2023/)
and [service/licence dataset](https://data.public.lu/fr/datasets/bd-l-ortho-webservices-wms-et-wmts/).
WMS 1.3 EPSG:4326 uses latitude/longitude axis order; image row zero is north.
White JPEG background connected to image edges is masked and feathered;
isolated white roofs remain valid. Regional coverage falls back to NASA.

| Region | South, west, north, east | Pixels | Approximate span |
| --- | --- | --- | --- |
| Cell | 49.60, 6.12, 49.62, 6.14 | 2048² | 2.2 × 1.4 km |
| District | 49.52, 6.00, 49.70, 6.26 | 4096² | 20 × 19 km |
| Context | 49.40, 5.70, 50.20, 6.60 | 4096² | 89 × 65 km |

National coverage has an irregular boundary inside those rectangles. Prepared
files are respectively 1,637,387 / 5,964,635 / 2,889,144 bytes. SHA256 receipts:

- Cell: `9a6c5fbae7bf4683a9485c51b152f2346c0dba4c09194406cca34ed656ce4c3b`
- District: `652bd6baa38bb74dbbf2c25d068d08132e4bab5f5e8c467589cc39f5a6655e67`
- Context: `8f1fce28f5634ccd83a00202b2aca169889025d14a2f55313141b715784cdd06`

Geographic files are external caches and are not committed or redistributed.

## Rendering and cost

A cache worker composites paired 2048² RGBA near/far atlases, coarse first.
Source borders fade over 10% of their smaller extent (100–5000 m). Near detail
fades over its final 200 m into the same far atlas sampled by far geometry.
Local samplers clamp both axes; NASA wraps longitude only. Textures and UV
meshes publish only for the current height generation and geographic revision.
Cancellation is checked each row; remote regions allocate no atlases.

Existing near/far geometry and topology stay the same: at most 256,000 triangles.
This adds one split draw and two mipmapped imagery textures, no fullscreen pass.
Ordinary OFP rendering is unchanged. JPEG metadata, hashes and receipts are
validated. Only three recognized source names are loaded, each at most 4096²
RGBA. The decoded-source budget is 256 MiB, not a process RAM limit.

Actual prepared sources use 144 MiB plus 32 MiB resized NASA. Paired atlases add
32 MiB, with another pair possible during renderer handoff. Decode, compressed
input, no-data flood-fill/distance buffers and allocator scratch add temporary
memory. Accepted maximum source dimensions can exceed 330 MiB transient CPU
buffers. Paired mip chains use about 42.67 MiB GPU instead of the previous
21.33 MiB single atlas; a replacement briefly overlaps another pair. Existing
NASA globe textures, meshes and driver retirement are separate baseline costs.

## Ground and verification

The near patch also publishes immutable, revision-tagged triangle heights for
infantry ground/contact. Far horizon/globe geometry is presentation only.
Missing coverage rejects ground teleports and holds infantry at the resident
boundary. This does not provide global vehicle physics, building collisions,
navigation or seamless floating-origin travel.

`Test-EarthSurface.ps1` checks orientation, feathers, near/far colour convergence,
remote allocation avoidance, cancellation and corrupt receipt rejection. With
`-CacheDirectory`, it composites actual sources and checks national no-data
fallback. Installed runtime acceptance is recorded in
`docs/verification/integrated-earth-material-reshade-20261008.md`.
The full transfer matrix is in
`docs/roadmap/modernisation/WORLDGEN-STREAMING-FEATURE-AUDIT-20261008.md`.
