# Eden church fixed-roof snow

Source candidate based on `6cf9973e`. CPU proof passed; installed visual acceptance
and combined engine/shader compilation remain root-owned and pending.

The saved `build/forest-snow/far-normal-headroom-20261001-033249-86a852/Eden-tree1/enabled-distance500.png`
shows the foreground church roof brown while neighbouring buildings and the
forest receive snow. This identifies a valid partial observation, not a passing
forest campaign: that campaign separately failed a Noe restored-image file lock.
The Eden view is `5025 3725 144.545459747314 0 -8.53076560994813`; tree target
is `5025/4225`. Stock `Worlds/eden.wrp` object census identifies the only church
within 500m as object **4867**, `data3d/kostel3.p3d`, at approximately
**5056.6/32.9/3920.0**. These are CLI rounded coordinates, not an installed exact
transform query. This is separate from the previously discussed distant Nogova
church placement.

The direct source gate refuses `GetAnimated(mesh)`; `Church::IsAnimated` always
returns true. The retained source gate separately refuses every Church because
procedural clock animation lacks a reliable stored whole-model pose proof.
`Church::Animate` rotates only the eight named `hodinova1..4`/`minutova1..4`
selections. A blanket Church exception would incorrectly license those moving
parts. Fountain and unknown Church subclasses remain excluded.

Actual retail `Dta/Data3D.pbo` member `kostel3.p3d` has decoded SHA256
`14fb625d820383b5e32b1e45d563c4eeace8caaed44c2d699bd0a3f64af9484c`.
The five visual LODs are:

| Resolution | Vertices | Clock vertices | Roof corners | Clock corners on roof | Clock vertices inside roof bounding span |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2 | 1394 | 24 | 30 | 0 | 16 |
| 4 | 655 | 24 | 30 | 0 | 24 |
| 7 | 370 | 24 | 30 | 0 | 24 |
| 9 | 185 | 0 | 30 | 0 | 0 |
| 12 | 123 | 0 | 19 | 0 | 0 |

The main roof texture is `data/tasky_tmavsi.pac`. Its actual source normals have
positive up components, minimum 0.465–0.508 depending on LOD. No slope or snow
material tuning is included. The vertices are interleaved with clock vertices:
testing a conservative vertex span would reject the near roof despite zero
actual animated corners. The special proof therefore reads actual face corners.

## Implementation boundary

Only exact `Church`, static/intact, MapChurch, no configured geometry animations
and no shared-shape destruction deformation qualify. All selected visual LODs
must be loaded, without keyframe geometry animation, and fit one 16,384-vertex
budget. Their finite current positions/normals must exactly match original data
on every nonclock vertex. Source data is saved once during registration; existing
`SaveOriginalPos` also saves normals. Clock changes cannot validate themselves by
matching a neutral pose: every registered section intersecting a clock selection
receives the explicit `WGR_MODEL_SECTION_NO_OBJECT_SNOW` veto.

Direct complement draws prove clock-disjoint actual face corners and original
positions/normals for that section run. This override applies only to Church's
whole-LOD animation hint. Owner, skin/palette, depth-writing material, conform,
proxy, vegetation, reflective/glass/cockpit and facing/exposure gates remain.
Actual corner work is capped at 16,384, with no per-frame heap allocation.

Bit 128 is unused existing `WgrModelSection::flags` storage; no public structure
size, varying count or storage binding changes. C++ enum, pure policy constant,
Rust FFI constant and WGSL constant agree. The shared retained receiver checks
the bit for both colour and normal prepass, preserving alpha/discard and model
ownership. A matched updated exe/DLL pair is required: an old renderer would
ignore the new semantic bit even though the byte layout still matches.

This changes snow admission only. It does not repair or re-own retained clock
animation, extend fountain support, admit animated subclasses, alter artistic
AO, change roof rain exposure, or claim every model/map now works.

## Reproducible CPU evidence and pending runtime

`pwsh -NoProfile -File scripts/Test-ChurchSnowSurfacePolicy.ps1` passes clock
intersection including neutral clocks, interleaved fixed corners, moving clocks,
position/normal mismatch, missing/nonfinite original data, invalid selections,
proof budgets, and clockless LOD checks. Its source contract requires the shared
material veto in both retained colour and normal receiver paths.
`scripts/Test-ObjectSnowSurfacePolicy.ps1` also passes unchanged.

`python scripts/Inspect-ChurchSnowStockSource.py --game-dir "D:/SteamLibrary/steamapps/common/ARMA Cold War Assault" --out build/church-stock-cpu-proof`
uses the existing strict ODOL7 reader, verifies the retail hash, then compiles and
executes the actual new C++ helper with all five real source roof/clock arrays.
Every roof passes with clocks moving; every clock corner fails even neutral;
changing an actual roof normal fails. Output is factual JSON and ignored CPU
probe source, not redistributed P3D assets. This is source/CPU evidence, not a
game run or physical-shelter proof.

Root must run combined shader composition and engine compilation, then a matched
installed Eden original view plus near/200m/500m views with snow ON/OFF and fixed
weather/pose controls. `WGR_SNOW_SURFACE_FIXTURE=1` now includes `kostel` models;
direct trace adds `churchFixedSection`, retained trace reports actual pose
admission. Require actual owner/model/receiver evidence and inspected roof pixels;
clock hands must remain uncoated and interior/underside controls dry. Unknown or
missing physical maps retain existing conservative exposure behaviour. No
installed appearance acceptance is asserted by this commit.
