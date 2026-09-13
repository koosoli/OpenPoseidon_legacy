# PERF-032 - Dense infantry cell index and far simulation rate

Date: 2026-09-13

Status: implemented and installed-game measured on `asset-obs-001` from
baseline `ad181620`; final pre-commit build stamp `ad181620+dirty`.

## Problem

The community `@63units/63MAX.west_ls2` mission starts 63 groups and 708
soldiers. Upstream CWR-CE plays the scene smoothly, while this fork entered a
fixed-step catch-up spiral. The first valid baseline measured 94.615 ms per
60 Hz simulation tick, including 81 ms in vehicle/man simulation and about
19,965 footprint-related calls per tick. Rendering alone did not explain the
failure.

`Mark` entities used for footprints are `TypeTempVehicle` and share the
landscape cell's full object list. A dense moving formation therefore made
soldier collision and roadway queries repeatedly walk thousands of unrelated
marks. Marks that expired together also made `World::MoveOutAndDelete` shift a
`RefArray` tail once per deletion, producing quadratic cleanup work.

The fork also runs a deterministic 60 Hz fixed world step. The old remainder
pass called `Entity::SimulateRest` at full rate for every distance/visibility
bucket. This differs from upstream's variable-frame simulation and made a
temporary overload self-amplifying: one rendered frame requested several
expensive catch-up ticks, reached the eight-step cap, and discarded time.

## Implementation

### Spatial query mirrors

`ObjectListFull` now maintains compact, non-owning mirrors for:

- entries whose type is exactly `TypeVehicle`;
- entries whose shape provides a roadway level.

Add, delete and clear update the mirrors alongside the owning cell list.
Delete removes mirror pointers before releasing the owning `Ref<Object>`, so a
cell-owned static object cannot leave a dangling pointer during teardown.
Debug assertions compare the TypeVehicle mirror, cached count and authoritative
full scan.

The mirrors are consumed by:

- `Landscape::ObjectCollision(..., onlyVehicles=true)`;
- `Landscape::PredictCollision`;
- `Landscape::GroundCollision`'s roadway pass;
- both `Landscape::RoadSurfaceY` overloads.

General object queries still use the authoritative complete list. Footprints,
roads and collision behavior are not globally disabled.

### Linear entity-list cleanup

`World::MoveOutAndDelete(VehicleList&)` now performs one stable compaction.
Survivor order is preserved, and delete/move-out/convert cleanup calls retain
their original order. A local strong reference keeps the current entity alive
while its world references are removed.

### Importance-based physical rest simulation

`World::SimulateOnly(VehiclesDistributed&, ...)` keeps `VisibleNear` on the
unchanged full-rate path. For `VisibleFar`, `InvisibleNear` and `InvisibleFar`,
only a requested `Entity::SimulateRest` is routed through the existing
`Entity::SimulateOptimized` precision accumulator. Other simulation member
functions are untouched. Each entity's existing `SimulationPrecision` remains
the source of its interval and deterministic catch-up order.

`POSEIDON_FAR_SIM_FULL_RATE=1` restores the former full-rate remainder path for
comparison or emergency rollback. The default is the optimized path.

This does not throttle group or unit AI Think. It does not change the global
60 Hz fixed step, random ordering, save format, render quality, view distance,
LOD selection or footprint lifetime.

## Measurements

All figures below are measurements, not projections. They use the same mission,
WGPU, 1280x720 window, VSync off, real-time pacing, and the installed matched
EXE/DLL. The mission is loaded by absolute path. The A/B control differs only
by `POSEIDON_FAR_SIM_FULL_RATE=1`.

| Arm / phase | Simulation ms/tick | Vehicle/man ms/tick | Pacing |
|---|---:|---:|---|
| `ad181620` valid baseline | 94.615 | about 81 | catch-up spiral |
| final code, full-rate control, early active windows | 26.322-50.177 | 24-48 | 32-40 ticks/s; 26-40 steps/s discarded |
| final code, optimized, early 708-unit windows | 6.471-7.892 | 3.361-4.980 | 60-61 ticks/s; zero discarded after load |

The final installed verification is
`build/ai-63groups/63groups-final-verified`. Its first reporting window still
contains mission loading and discarded 25 steps. Every subsequent reported
window sustains 60-61 ticks/s with zero discarded steps. During the initial
full-unit window it reports all 63 group Think calls and all 708 unit Think
calls. The 256-frame settled capture reports:

- 175.67 average FPS;
- 5.692 ms average and 6.622 ms p95 CPU frame time;
- 0.234 ms average and 0.959 ms p95 `sim:step` time;
- 4.345 ms captured GPU frame time.

The settled frame is not used to claim the initial-scene simulation speedup:
mission state and camera/player position evolve differently when one arm falls
behind. The causal comparison is the early per-second simulation log above.

An ablation with footprint creation disabled reduced the old baseline to about
15 ms/tick and established footprints as the source of the spatial-query
regression. Disabling footprints is not the shipped fix.

## Verification

- RelWithDebInfo WGPU build passes.
- Targeted CTest cases pass: swept broadphase, regional terrain collision,
  landscape upload/layout and three landscape ID-cache tests.
- Installed mission run exits cleanly and produces metrics and screenshot.
- No validation error, panic, assertion or fatal error is present in the final
  run log.

The complete CTest discovery currently also contains one pre-existing malformed
aggregate command. It reports `BAD_COMMAND` when selected by a broad regex; the
individual relevant tests above pass and this record does not claim a globally
green suite.

## Risks and remaining acceptance

- Exact-`TypeVehicle` collision filtering is intentionally narrower than the
  old `!Static()` proxy. A focused regression mission should still exercise
  moving vehicle, soldier, roadway and temporary-effect contacts together.
- Far entity body simulation now follows its declared precision. Multiplayer
  authority, save/load and long-distance scripted-object behavior need a
  dedicated deterministic replay comparison before changing precision values
  themselves. This change does not change those values.
- A normal mission with nearby combat should be compared with the rollback
  switch to prove that `VisibleNear` behavior is pixel/gameplay equivalent.
- The first load second still has a transient backlog. Asset/loading work is a
  separate problem; this change prevents that one second from becoming a
  sustained simulation spiral.

Do not replace this with a new scheduler or lower the global tick rate without
new measurements. The result comes from avoiding irrelevant work and using an
existing deterministic mechanism.
