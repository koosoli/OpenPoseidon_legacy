# Upstream recheck, September 11

Fork inspected: asset-obs-001 `ba808734` (also safety/consolidated-v2).
Fresh `git fetch upstream main` and `git ls-remote upstream refs/heads/main`
both resolve to `3541affa8027e92e072a5ef899b588a31823100b`.
There are NO new main commits since the September 8 review. Git author dates
are not merge/committer dates; use the SHA, not the dates printed by default log.
This is a targeted recheck of the previously identified candidates, not an
exhaustive audit of all historical upstream changes or open PRs.

## Recommended order

1. Adapt `078bb2f7` + `4c6aff84` cursor geometry/hotspot fixes together.
   Still missing: UI/Map/UIContainers.cpp ControlsContainer::DrawCursor scales
   horizontal size and hotspot with viewport width but vertical size with height.
   CursorLayout/LayoutCanvas are absent. This preserves the aspect-dependent
   distortion mathematically; visual reproduction has not been run this pass.
   Port the shared cursor math/tests, preserving our AspectRatio.cpp full-width
   UI policies. Test 4:3, 16:9, 21:9, menu hotspots, map guide lines, world aim
   cursor and resolution changes. Benefit is UI correctness, not FPS.
2. Benchmark the remaining opaque-sort path before adapting `cc4bd699` and the
   reusable bucketing tests/helper from `3541affa`. SceneDraw.cpp already has
   packed SortDrawMergersByShape keys, filtered surface work and alpha radix sort.
   Upstream O(n) grouping is not installed as such; the similarly named local
   Bucket struct is only a one-shot shape census. Preserve retained coverage,
   shadow-only objects, conform planes and transparent ordering. Compare the
   existing sortUs/submitUs counters and whole-frame tails on identical routes.
   Upstream's reported 10-15x sort improvement is NOT a measured fork FPS gain.
   Existing installed c22aa78b Reforger evidence shows the worst 220.947 ms frame
   had 216.244 ms residency, including 189.144 ms bank-table preparation; reducing
   that work remains ahead of speculative sort replacement for loading hitches.
3. Keep GL33-only maintenance separate and low priority for the WGPU objective:
   `6f0d7fec`, `bc3014a9`, `a918a023`, `315733ab`, and `ed6119f9` fix GL buffers,
   binding/heightmap state or the GL-specific cached detail texture quartet.
   They may be useful if maintaining GL33, but are not WGPU performance ports.

## Already represented / do not transplant

- `a61130ca`: Foundation/Platform/Shutdown.cpp UnloadGameData calls
  ScenePreloader::Instance().Shutdown(). Adapted previously; the September 8
  report contains its installed remount evidence. Not retested today.
- `7ca93a57`: SceneDraw.cpp rejects DeformsSharedShape for both batch head and
  subsequent instances. Do not call this missing or claim it fixes every report
  of glitching destroyed buildings.
- `9a298d09`: upstream's nested animated-bounds bug is not the same implementation
  as our conform path; earlier review remains applicable.
- `c551e783`: GL light-index/UBO instancing must not be copied into WGPU's own
  local-light path. CPU fallback opportunities require a separate profile.
- Land-clipped vegetation instancing series: compare our conform and destruction
  predicates, not replace them with GL heightmap handling.

No engine changes, cherry-picks, builds, deployments or new performance claims
in this review. Recommend selective adaptation, not a merge of upstream/main.
