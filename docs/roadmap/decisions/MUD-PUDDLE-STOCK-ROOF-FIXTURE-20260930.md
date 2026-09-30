# Stock tent rain-cover fixture — source evidence and candidate runtime procedure

The installed stock class `CampEastC` is a defensible real roof candidate over
the already validated native Dirt_02 patch. This document does **not** claim a
runtime roof capture, retained target draw or terrain pixel acceptance. The root
agent owns the installed runs; this investigation used file/config inspection,
CPU asset tools and pure mocked-TCP checks only.

## Actual installed class and model

Installed `BIN/CONFIG.BIN` defines `CampEastC: Camp`, with `model="stan_eastC"`.
Its inheritance is `Camp -> Strategic -> Building -> Static -> All`, and Static
uses simulation `house`. It is a real stock class, not a proposed custom class.

`Data3D.pbo` contains `stan_eastc.p3d`, ODOL revision 7, seven LODs. The first
visual LOD has 470 points and 265 faces, independently matching PoseidonTools
inspection. The central 2×2 metre square has no low horizontal triangle whose
projected AABB overlaps the square. All nine rays at model X/Z -1, 0 and 1 hit
only roof fabric, at model Y 0.73984–1.81137; minimum clearance above the model
lower bound is 3.54989 metres. The model bounds are approximately
`[-4.58742,-2.81005,-5.30261]` to `[4.58742,2.81005,5.30261]`.

This proves a useful central floor-free region in the first LOD, not the absence
of every mat, threshold or floor fragment throughout every LOD. Close covered
cameras are inside the model's bounding sphere; ordinary retained distance
selection near-clamps to zero and therefore selects the fine LOD. Any runtime
LOD override still needs to be recorded and checked.

The corresponding floor-bearing candidates are unsuitable: `CampEmpty` uses
`stan_inside.p3d`, which has a cloth floor at Y -1.518 across those probes;
`CampEast` uses `stan_east.p3d`, with a wooden floor at Y -2.622. The pure audit
rejects CampEmpty as a positive falsifier of its floor-free criterion.

Roof texture `data/stan2_east_side.pac`, extracted read-only from `Data.pbo`, is
classified **OPAQUE, depth-write** by PoseidonTools. Seven other first-LOD
textures were not classified in this bounded extraction; no whole-object
coverage claim is made from the single opaque texture.

| Source | SHA-256 |
|---|---|
| Installed CONFIG.BIN | `DA9B790900357450D06CB27FD0CDBCF671E6316D6A6D4D0C9982D358AFF703CA` |
| Installed Data3D.pbo | `10528EF9B5112896898AD8667E83601FF47F95231652C73409D57B7227A8A00F` |
| Extracted stan_eastc.p3d | `C7505A2B08231A540147515D429AF3083CDCCB9123CB93F3B0B5FCBFA6B8D696` |
| Extracted roof PAC | `7D2A84019B8B08CF69FD5351303DDF063FAA5C634CF142831DA7C7C006D059EF` |

An engine `LODShapeWithShadow::LoadOptimized` / `SaveOptimized` roundtrip retained
the same first-LOD bounds and central hits. ShapeFile transfers `_pos` directly;
there is no load-time centering translation in this path. The roundtrip reports
348 welded engine points and saves the 470 vertex records again. Extracted model
assets and roundtrip data stay in the isolated ignored build directory; they are
not added to Git or installed into the game.

## Source path into sky depth

Building construction sets `_static=true`. `NewNonAIVehicle` changes the object
type from Primary to TypeVehicle while retaining that static flag; this permits
the ordinary setPosASL and deleteVehicle commands on the spawned mission object.
`createVehicle` adds it through `World::AddBuilding -> VehiclesDistributed::Add
-> VehicleList::Add -> Landscape::AddObject -> EngineWgpu::SceneObjectCreated`.
The latter admits intact static opaque-rigid geometry into retained instances.
The opaque roof section is therefore eligible for retained sky-depth rendering.
Actual registration and draw remain runtime checks, not consequences we claim
from inheritance alone. An unsupported section can leave partial coverage.

Sky-depth views cull the retained scene and use the ordinary main camera's
position for distance/LOD selection with their own orthographic frusta. Ordinary
instances have no private-main-only geometry bit; those private-only records are
excluded from sky culling. Each map clears to far depth and draws retained solid
and cutout sections. No new geometry or renderer API is required for the fixture.

## Candidate runtime procedure

Use the installed matched EXE/DLL, the shared lock, an isolated mission/profile,
the terrain-only diagnostic controls chosen by root, and the existing normal
TCP exit/provenance/hash checks. Do not change the owner's profile or stock PBOs.
The root stability script is reserved for root and was not modified here.

Dot-source the separate helper in a harness that already defines `Send`:

```powershell
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
$fixture = New-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }
$fixture | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'stock-roof-fixture.json')
```

The helper uses runtime `water_bathymetry` centre samples to query terrain Y at
the tent centre X/Z `(9481.25,3006.25)` and both camera positions. It places the
origin at centre ground height minus model minimum Y `-2.8100528717041`, plus
0.02 metre clearance. It resets heading to zero/upright and verifies actual
getPosASL within 0.1 metre. It verifies class `CampEastC` and the actual object
debug basename `stan_eastc` (Object debug names remove the extension).

The candidate covered camera uses X/Z `(9481.25,3004.75)`, its own queried ground
height +1 metre, azimuth zero, elevation -55 degrees. The exposed control is
12 metres east, X/Z `(9493.25,3004.75)`, with its own ground +1 metre. The earlier
fixed guesses `205.72` camera / `207.53` roof are superseded by runtime heights.
An actual camera's world height is never inferred from the old close camera at
Z 2994.25. The covered view aims at the audited central roof region. It remains
a candidate until inspected for visible terrain, fabric, wall clipping and the
actual puddle pattern.

For exact **retained model** evidence, enable existing
`POSEIDON_INTERIOR_SKY_PROBE` before launch at a sufficiently late frame (for
example 1800 with the 60 FPS fixture cap), spawn early and hold the covered pose
until its probe has fired. Capture the line beginning `Interior sky probe at`:
it names the three nearest models from the actual `_gpuInstances` map. Require
`stan_eastc.p3d` among them at the expected nearby distance. Retain the following
`interior sky maps (probe...)` per-direction coverage report. This diagnostic
readback is intentionally expensive; keep it outside all benchmark windows.
The frame number is a diagnostic trigger, not a wall-clock guarantee; if it
fires before spawn or at the wrong camera, that evidence is invalid and needs
a fresh bounded run. Aggregate map coverage alone does not identify the tent.

Required pixel controls at the **same covered camera**:

1. Roof absent: dry/wet response must be visible in a verified terrain ROI.
   This rules out a naturally empty puddle pattern, source material mismatch or
   slope admission failure.
2. Roof present: dry/wet captures must suppress the response in that same visible
   terrain ROI, while the exposed dirt control retains a response.
3. Delete the roof and capture the original covered camera again. The wet ground
   response must return; this ties suppression to the actual roof rather than
   loading, a hidden floor or a changed camera.

```powershell
$null = Send @{cmd='eval';code=('triFreeFlyPose "'+$fixture.coveredCamera+'"')}
# Settle/capture with the enclosing runner's existing PNG validation.
Remove-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }
$null = Send @{cmd='eval';code=('triFreeFlyPose "'+$fixture.coveredCamera+'"')}
# Settle/capture the same ground again, then exit normally.
```

The displayed diffuse sky debug view remains useful supporting evidence but is
not the zenith rain query. Do not accept suppression from a dark room or a
floor-covered camera. Root should inspect these candidate images and retain
the material, identity, transform and removal controls before a roof claim.

## Reproducible CPU evidence

### Relocation after the empty-pattern runtime control

The first installed roof campaign at centre `(9481.25,3006.25)` identified the
retained stock model, but its absent dry/wet images had zero changed pixels at
the existing threshold. That is an invalid positive control for pixel shelter
acceptance. Reproducing the production integer hash and smooth world noise
gives a field of approximately 0.383 there, below the full-wet 0.64 threshold.

`New-TerrainPuddleStockRoof` now accepts finite `-X` / `-Z` centre coordinates
and optional `-ControlX` / `-ControlZ` open control centre coordinates. Defaults
preserve the original centre and 12 metre east control. Every camera still
queries its own runtime terrain height; covered and open cameras sit 1.5 metres
south of their respective centres, at local ground +1 metre. Class, model,
minimum model Y, heading, actual ASL placement checks and removal are unchanged.

Next **candidate**, requiring actual material, flatness and visible-pixel checks:

```powershell
$fixture = New-TerrainPuddleStockRoof -SendCommand {param($request) Send $request} `
    -X 9486 -Z 3006 -ControlX 9475.25 -ControlZ 3018.25
```

The roof centre's field is approximately 0.775. The independent open centre
has field 0.901 and is 16.3 metres from the roof centre, outside the audited
4.59 by 5.30 metre half-extents. Its camera looks north from Z3016.75 toward
field values around 0.81–0.88. These are CPU candidate predictions, not shader
pixel acceptance: height slope, material mixture, shelter coverage and actual
camera pixels still need the absent/present/removed controls.

Mock checks cover a relocated centre, explicit independently positioned open
control, different queried camera heights, and rejection of nonfinite centre
or control coordinates before any runtime request.

`scripts/Inspect-TerrainPuddleStockRoof.py` reads the bounded first visual LOD,
verifies BIS compressed-array checksums and reports actual rays, the conservative
continuous floor check, bounds and source hash. It is an audit, not a general
asset importer. Run it against extracted stock data:

```powershell
python scripts/Inspect-TerrainPuddleStockRoof.py <extracted-stan_eastc.p3d> --output <audit.json>
```

CampEastC returns `centralFloorFreeRoofCandidate=true`, points 470/faces 265,
minimum clearance 3.54989. CampEmpty returns false, points 736/faces 340,
minimum clearance 0.09472. The engine-roundtrip CampEastC also returns true.
`Test-TerrainPuddleStockRoof.ps1` passes pure mocked-RPC checks for runtime-derived
heights, exact class/model/position, deletion and refusal of wrong class/model
or displaced placement. No game/GPU/full build/deployment was performed.
