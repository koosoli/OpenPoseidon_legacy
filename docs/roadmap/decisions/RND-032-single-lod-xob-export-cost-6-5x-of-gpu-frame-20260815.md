# RND-032 — The XOB export wrote one LOD per model, and that cost 6.5x of the GPU frame

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Status:** PARTIAL — performance closed and measured; near-field fidelity capture still owed.

## The finding

`XobCommand` exported only the finest graphical LOD of each Arma Reforger model. Nothing downstream
was broken: with one LOD per model, `cull.wgsl`'s selector cannot run at all (it is guarded on
`model.lod_count >= 2`), so every instance drew at full detail at every distance. The renderer
reported this plainly and had been reporting it for some time:

```
Objects LOD histogram (main view): lod0=19388 lod1=0 lod2=0 lod3=0 lod4=0 lod5=0 lod6=0 lod7+=0
```

`XobCommand` now exports every valid graphical LOD, sorted finest-first by source threshold and
mapped to runtime resolutions by native rank.

## What was measured

Both captures use the same mission (`00training.abel`), world (`ev_DALL.wrp`), camera
(freefly `8500 800 170`, look-at `8500 1400 60`), hour 10, 800x600, and
`WGR_OBJECT_STREAM_MAX_OBJECTS=200000`. Critically they also match on **streaming residency at
capture time — 153,600 resident in both** — which is what makes this an A/B and not two runs.

| | baseline `99f0ff7` | multi-LOD `4b33b5e` |
|---|---|---|
| **GPU frame total** | **77.259 ms** | **11.850 ms** |
| Objects: prepass solid | 28.345 | 0.234 |
| Objects: colour solid | 16.625 | 0.672 |
| Shadow cascade 3 | 11.143 | 0.216 |
| main-view triangles | 115,962,974 | 575,807 |
| far-shadow (c3) triangles | 54,268,775 | 401,319 |

`GPU frame total` is the only figure quoted that is re-measured every frame. One-shot regions
persist stale values in `--capture-metrics`, so they are not quoted here.

## Export correctness

A census parsed all 1132 models in both the new tree and the pre-change backup, with byte accounting
closed on every file (1132/1132 parsed in each, zero failures):

- 1117 models gained LODs; mean 4.90 LODs/model. The 15 that did not had single-LOD sources.
- LOD0 is **bit-identical**: SHA-1 over the point table, normal table and every face record matches
  on all 1132 models. The finest LOD was not disturbed.
- LOD0 triangles 8,197,099 in both trees; all-LOD total 14,943,272 in the new tree.
- Written resolution ladders are clean prefixes of `1,2,...,9` with no gaps or duplicates.

## Two conclusions that looked right and were retracted

1. **"The rank ladder is too compressed and should be geometric."** Native Arma uses the *same*
   linear ladder. `model inspect` on Arma 3's `t_pinuss2s_b_f` (pine) and `cargo_house_v1_f`
   (building) each give exactly **4 visual LODs at resolutions 1, 2, 3, 4**; everything else is
   >= 10000 (special LODs). That is exactly `VisualLodResolution(rank, base) = base*(rank+1)`.
   Native A3 also collapses to a **13-face** pine and a **76-face** building at resolution 4, so
   stub meshes at distance are the RV convention, not an export defect. `Tracks.cpp`'s
   `lodCoef *= 2` is a different mechanism and is not evidence for a geometric ladder here.
2. **"The coarse trees are an artifact of the 800x600 capture."** The mechanism is real —
   `cull.wgsl` selects the coarsest LOD whose `resolution <= dist * lod_inv_width * Camera::Left()`,
   and `Scene.cpp:785` sets `_lodInvWidth = lodCoef*2/Width()`, so a narrow window switches LODs
   ~2.4x nearer than 1920 does. **Tested and refuted:** at matched streaming state (registered ~82k)
   the histograms are effectively identical — 22.2 vs 25.9 tris/instance, lod4 dominant in both,
   lod0 ~0%. Most instances are *clamped* at their model's coarsest rung, so moving the switch
   distance does not un-clamp them.

What is actually true: the reference camera views objects 600-3000 m out, where `dist * k` runs
~8-40, past the coarsest rung for any model, native or converted. Cone trees in that frame are
correct LOD behaviour. **The baseline's 116 M triangles was the defect** — full LOD0 detail held out
to 3 km.

## Scope: nothing to port to the other compatibility layers

The LOD path was never broken; only the XOB producer was. Native ODOL content spreads across buckets
normally, and in each case the buckets sum exactly to `main inst`, so the readback is not partial:

| world | format | histogram | lod0 share |
|---|---|---|---|
| Takistan | Arma 2/OA ODOL | 75 / 118 / 303 / 151 / 506 / 325 | 5.1% of 1478 |
| Stratis (1500 m) | Arma 3 ODOL | 208 / 160 / 1144 / 3926 / 305 / 955 | 3.1% of 6698 |
| Everon (before) | XOB-converted | 19388 / 0 / 0 / 0 / 0 / 0 | 100% |

## Supersedes

The performance section of `HANDOVER-20260815-reforger-renders.md` concluded that object draws were
0.5% of the frame and that fixing LOD selection was "near-worthless". That was measured on a 13.7 ms
frame — a far lighter object load than the full Everon set at budget 200000. The measurement was not
wrong; its scope was. Its cascade and residual findings stand; the "near-worthless" conclusion does
not.

## The near-field coarseness is the adaptive governor, not the ladder — and the frame is CPU-bound

The near-field capture showed only 21 of 11,195 instances at lod0/lod1, which looked like a tuning
defect. It is not. Two corrections, in order:

**`_lodInvWidth` does not depend on screen width in gameplay.** `Scene.cpp:785`
(`_lodInvWidth = ENGINE_CONFIG.lodCoef * 2 / GEngine->Width()`) sits inside `if (ObjViewer)` — the
object-viewer tool path. In gameplay the value comes from the framerate feedback loop in
`SceneDraw.cpp:1031-1161`, clamped to `[_qualitySettings, _qualitySettings*16]`, which scales toward
coarser LODs when frames are slow. The comment at `EngineWgpu.cpp:1797` describing the pushed value
as "≈ lodCoef*2/screenWidth" is therefore wrong for every non-viewer frame, and should be corrected.

**Confirmed by experiment.** Same camera, same assets, same 1920x1080; only the object budget changed:

| | budget 200,000 | budget 20,000 |
|---|---|---|
| LOD histogram | lod0=10 lod1=11, bulk lod4 | **lod0=3795 lod1=627 lod2=2** |
| lod0+lod1 share | 21 of 11,195 (0.2%) | 4,422 of 4,424 (**99.9%**) |
| main-view triangles | 415,617 | **12,954,484** |
| GPU frame total | 10.747 ms | 29.731 ms |
| wall clock per frame | 121.2 ms | 49.5 ms |
| **GPU share of frame** | **8.9%** | 60% |

So detail recovers completely once frames are fast. The governor was working as designed.

**The actionable finding is the last row.** At 200k objects the GPU is **8.9% of the frame** — roughly
110 ms per frame is CPU-side (object streaming creates 128 per update, residency management,
simulation). The governor only controls GPU geometry detail, so a CPU-bound frame makes it strip
12.95M triangles down to 0.42M for a saving of ~19 ms of GPU time that was never the constraint.
Visual quality is being spent on a bottleneck it cannot move.

Consequences worth acting on, in priority order:
1. **The next win is CPU-side, not GPU-side**, for every compatibility layer. Profile object
   streaming/residency before any further GPU work at scale.
2. The detail governor should not be driven by total frame time when the GPU is idle relative to it.
   Feeding it GPU frame time (already measured per frame) rather than wall clock would stop it
   trading quality for nothing.
3. Wall clock improved only ~27% (107.3 s -> 84.4 s over 1200 frames) across the LOD change even
   though GPU frame time fell 6.5x — the same CPU bound, visible from the other side.

## Not closed

- **Near-field fidelity.** The aerial pose cannot answer it: almost nothing in that frame is within
  200 m. A ground-level capture is owed. Partial evidence from a run killed mid-capture: shadow
  cascade 0 carried 183 instances / 428,864 triangles = **2,343 tris/instance**, between the
  reference spruce's LOD0 (3,216) and LOD1 (1,188), which indicates near-field detail is intact.
- **Owner review.** Not performed. Per this ledger's review model, that is what VALIDATED requires.
- **Attribution caveat for pixel differences.** The pre-change backup had only 1.5% of LOD0 faces
  carrying a texture string against 99.2% now, so image diffs between these two captures conflate
  the LOD change with the texture pass. Triangle counts and the LOD histogram are pure geometry and
  unaffected, and texturing would make frames slower rather than faster, so the 6.5x is not inflated
  by it.

## Operational note

The shared "stop PoseidonGame PIDs that did not exist before my launch" cleanup used by the capture
scripts **kills other agents' concurrent runs** — a run started after another agent's snapshot looks
new to that agent. Two captures died this way. Timing runs must be serialised regardless, since
concurrent instances sharing the GPU make the numbers meaningless.
