# Bounded soft-soil mud contact and height contract

Oliver requested real mud footprint depressions similar to snow. This source
slice adds independent mission-local CPU geometry state and real step producers;
it does not claim installed rendering or collision acceptance.

`SnowField` supplies the established storage/window pattern. Its renderer
already refines close CDLOD tiles to 4m patches (32 quads, 12.5cm spacing) and
displaces terrain vertices from its contact field. Its gameplay height queries
remain bare terrain, with separate snow wading/body presses. Reusing the snow
buffer's address space must therefore leave snow evaluation unchanged and add
an independent signed mud evaluation shared by geometry and physical terrain.

## CPU geometry and source admission

`MudField` stores positive depression metres in 16×16-cell chunks at 12.5cm
spacing. `HeightOffsetAt` returns their negative, cell-centre bilinear height;
`HeightGradientAt` supplies the same evaluation's analytic x/z derivatives.
Smooth boot kernels overlap by maximum rather than additive digging. Depth
never exceeds 6cm. A gradually increasing saturation produces a gradual 0..6cm
contact depth, rather than forcing a minimum sink on the first rainy frame.

`World::SimulateCloudlets` advances soil saturation in actual simulation seconds
using finite/clamped maximum of actual weather rain and effective liquid-particle
density. Snowflake overrides alone cannot wet the soil. Fill/dry constants are
30/180 seconds; the constant-exposure integrator is solved exactly. Pause is a
no-op. Drying limits new contacts but does not erase existing geometric tracks.
`World::InitLandscape` explicitly resets this mission-local store. No save or
network fields have been added; reused save/network behavior remains unaccepted.

`SoldierOldMove` invokes the producer at actual animation foot-contact edges,
including when decorative marks are culled. Toe and heel are oriented along
the soldier's direction. Admission uses `Landscape::SurfaceAt` point/quadrant
metadata: exact stock cultivated soil `Field` + `files=pol` + `sound=dirt` +
empty clutter character. A 3×3 stencil guards the full boot and interpolation
support against nearby hard or unknown surfaces. This can admit a positive
soil quadrant even when an old renderer's all-four-quadrants material flag is
zero; that flag is not consulted or guessed from terrain colour.

RoadSurfaceY's actual supporting-object output blocks roadways and elevated
floors even when their height equals the underlying field. Contact height,
finite moderate slope, sea margin, existing snow cover and a self-ignored
physical ViewGeometry roof ray provide additional negative gates. Native
Reforger and authored A3 multi-layer masks currently fail closed until an exact
point-mask producer is connected. Unknown/mod aliases, grass, village mud-like
textures and hard surfaces remain undeformed. Optional `POSEIDON_MUD_TRACE=1`
logs changed admitted `MUD_STEP` source identity, wetness, depth, revision and
chunk count. Source policy is not proof that any particular installed Nogova
placement passes it; actual source/contact rows are still required.

## Renderer and physical integration API

`GMud().Snapshot(cameraX,cameraZ)` returns exactly `4 + 512*512` floats:

```text
[originX, originZ, 0.125, 0.06, signedOffsets[512*512]]
```

Offsets are row-major at world cell centres. Disabled or invalid snapshots
have header w=0 and zero offsets. `Revision()` changes only when stored geometry
changes or resets, so revision plus snapped window origin identifies unchanged
uploads. Camera movement never evicts imprints. At 8,192 chunks the store rejects
new allocations, counts rejection and still permits deeper edits in existing
chunks; live tracks are not silently removed to make room.

Root owns the pending renderer/private-buffer/CDLOD and authoritative terrain
collision integration. Both terrain and conform consumers must evaluate the
same signed field; close geometry needs the established refinement rather than
a colour decal. Ground height and derivatives must apply the field once, while
road/structure support retains independent ownership. No broad collision or
renderer edit is included here, and no real rendered depression is claimed yet.

## Source verification

`scripts/Test-MudField.ps1` compiles and executes the actual header test with
strict C++20 warnings. It passes wetting/frame partition, paused/nonfinite inputs,
signed depth cap, repeat/max compression, negative chunk coordinates, analytic
gradient checks, snapshot/camera-return persistence, bounded allocation without
eviction, disable/reset and exact stock metadata negatives. Independent packed
sampling agrees with CPU samples within 0.1µm for binary grid fractions. An
initial 0.1µm assertion for nonbinary positions failed due to subtracting the
32m snapshot origin in float; separately checked nonbinary samples bound that
roundoff below 2µm. This is float arithmetic evidence, not a GPU/runtime result.

No engine/game build, GPU launch, deployment or installed appearance/collision
check was run by this agent. Required next controls include real source-positive
stock soil steps, visible geometry/profile and physical height agreement,
hard/road/roof/snow negatives, pause and camera return, reset and bounded cost.
