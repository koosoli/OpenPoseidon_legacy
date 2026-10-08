# Earth preview: live geographic relocation

Source checkpoint based on `84a667d4`, isolated branch
`codex/earth-live-location-20261008`. No full build, deployment or runtime game
test was performed here; combined installed acceptance remains required.

## Controls and behavior

When Earth preview is enabled, Dev Tools has an **Earth** tab with latitude,
longitude, altitude and **Go**. Alps, Grand Canyon and dateline examples use the
same path. Latitude is limited to -84..84 degrees (elevation source coverage),
longitude to -180..180, altitude to 500..30,000,000 metres. Invalid values are
rejected before changing the camera or origin. +180 normalizes to -180.

Go resets the freefly camera to the local anchor at the selected altitude and
looks down. It changes only the presentation origin, never the finite mission's
Landscape or simulation objects. This is explicit geographic teleport, not
seamless globe circumnavigation or a worldwide playable/editor world.

The renderer rebuilds its overview sphere for the new origin and drops the
previous origin's near patch together. The overview remains while replacement
elevation streams in. The imagery texture is reused. Each worker request
snapshots its geographic origin/revision; new origins invalidate ready patches,
cancel obsolete HTTP transfers and prevent old results from publishing. Orbit
relocation cancels old work without requesting unnecessary detailed elevation.
Longitude UVs stay unwrapped across dateline triangles and use the existing
repeat sampler, avoiding a whole-map smear across that seam.

## Harness path

The existing `eval` harness can execute the same controls:

```sqf
triEarthLocation []
triEarthLocation [36.1, -112.1, 6000]
triEarthLocation [-16.8, 180, 12000]
triEarthLocation [46.6, 8.1, 18000000]
triEarthLocation [46.6, 8.1, 4500]
```

Success returns `OK:latitude=... longitude=... altitude=... revision=...`;
invalid inputs or unavailable cameras return `FAIL:...`. Query does not change
the revision. Each successful Go, including a repeated Go, increments it.
Patch upload logs include geographic origin and revision for runtime matching.
SQF scalars have their existing float precision; the UI and stored origin use
doubles.

## Source checks

- `scripts/Test-EarthCoordinates.ps1`: PASS, including valid/invalid location
  state, dateline normalization and continuous imagery UVs, revision rejection
  of superseded results, plus previous adaptive elevation/seam checks.
- Strict Clang C++20 worker translation-unit syntax check: PASS.
- Production Clang-CL `/Zs` checks of `TerrainWgpu.cpp`, `DebugOverlay.cpp` and
  `GameStateExtTestAudio.cpp` with isolated source/PCH headers and shared
  generated/dependency headers: PASS.
- `git diff --check`: PASS.

## Required installed acceptance

Relocate Alps -> Canyon -> dateline -> Alps within one process, inspect matching
sphere/terrain and receipt revisions, and confirm the camera resets. Issue
rapid sequential relocations before loading completes; only the final origin
may upload afterward. Reject `[90,0,4500]`, `[0,181,4500]` and `[0,0,499]` and
confirm the origin revision and camera are unchanged. Inspect the dateline
near/far imagery seam. Relocate at 18,000,000m and confirm no replacement DEM
downloads occur until descent. Check ordinary OFP play remains unchanged.
