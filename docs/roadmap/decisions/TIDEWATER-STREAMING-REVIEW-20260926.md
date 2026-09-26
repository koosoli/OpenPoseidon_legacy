# Tidewater streaming and LOD research — deferred implementation

Owner requested read-only architectural research while the streaming plan continues.
Water and waves remain parked. This review does not authorize changing their defaults.
OpenPoseidon comparison baseline: `codex/streaming-residency`, `860fb1e2`.
Tidewater source baseline: `4811ba48d795197de5621985f404e765c0b7c0ef`.
The Tidewater agent-order document is a future proposal, not runtime evidence.

## Transferable ideas

- Model-specific hemisphere colour/normal/AO impostors could improve distant tree
  silhouettes and top-down coverage. Tidewater blends atlas views, but deliberately
  thins distant crowns and enlarges survivors; its bake depth buffer is not a runtime
  parallax atlas. An original OFP tree should be the first future comparison fixture.
  [Impostors source](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/src/world/vegetation/Impostors.js).
- Sorting the same selected opaque terrain nodes front-to-back is a bounded
  efficiency experiment. Preserve node membership, geometry, morph values and
  coverage, then compare CPU sorting cost with GPU terrain time. Our upload ordering
  at `TerrainWgpu.cpp` is separate from selected-node draw ordering.
  [CDLOD source](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/src/core/CDLOD.js).
- First-use pipeline instrumentation could guide safe precreation. Browser async
  compilation and mission warmup frames are not directly transferable: advancing
  simulation during warmup has gameplay effects. OpenPoseidon already caches pipeline
  objects in `gfx3d/mod.rs`.
  [GPU source](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/src/engine/gpu/GPU.js).

## Mechanisms to retain in OpenPoseidon

Our compute visibility compaction and indirect draws should remain. Tidewater's grid
queries, movement margins and buffer reuse are useful patterns, but collapsing rejected
vertices and retaining far vegetation do not replace disk preparation, bounded GPU
admission, lifetime accounting or gameplay residency. Candidate/capacity truncation and
immediate refreshes after large movement cannot establish complete coverage for our maps.
[InstanceLOD](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/src/world/vegetation/InstanceLOD.js),
[Vegetation scheduler](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/src/world/Vegetation.js).

Existing OGG streaming, PCM caching and background audio preload are preferable to
copying complete WebAudio buffer loading. Contextual anticipatory demand is the useful
idea. [SoundScape](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/src/audio/SoundScape.js).

Tidewater is MIT licensed; substantial copied code requires its notice. Asset rights
must be checked separately. No code or assets were copied during this review.
[License](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/LICENSE),
[credits](https://github.com/dgreenheck/tidewater/blob/4811ba48d795197de5621985f404e765c0b7c0ef/CREDITS.md).

## Sequence

Finish current resource convergence and ownership gates first. Later select the terrain
ordering experiment if terrain overdraw is measured, or pipeline instrumentation if
first-use stalls dominate. The impostor comparison needs separate visual acceptance:
top-down views, zoom, transitions, alpha coverage, shadows, memory and temporal stability.
None of these ideas independently fixes owner-side preparation stalls or proves a
performance improvement. No Tidewater implementation or water work is reported here.
