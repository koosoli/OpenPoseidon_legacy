# PERF-002 — The Arma 3 frame is the water reflection

Date: 2026-08-13. The owner reported Arma 3 performance as bad three times across
the session. This is the first measurement of it made under conditions worth
trusting, and it names one dominant cost.

## Why earlier numbers in this session are void

Two separate defects made every earlier attempt meaningless, and both are worth
knowing before anyone quotes a frame time again.

**1. Most `WGR_*` ablation gates are inert.** The C++ side rebuilds and pushes the
settings structs *every frame*, so an env var read once at construction is
overwritten immediately. Measured as inert: `WGR_GRASS`, `WGR_GRASS_DENSITY`,
`WGR_GRASS_DISTANCE`, `WGR_SKY_VIS*`. `sky_vis.rs` says so in a comment — "C++
pushes every frame and wins, so this only decides frame 0". A gate that silently
reverts is worse than no gate: it produces a plausible number for the wrong
configuration. `WGR_ABLATE` was added for this reason and applies at the push
site, where nothing downstream can undo it.

**2. The machine was not quiet.** Three identical captures — same binary, same
scene, same settle — returned 18.99 / 20.91 / 55.59 ms, a **2.9× spread**, purely
because other work was compiling. The renderer was not varying.

A third, smaller trap was in the capture harness itself: `-Width`/`-Height`
parameters that were declared and never appended, so every "1080p" capture
silently ran at 800×600. That one cost a wrongly-reverted change.

## Method

Interleaved A/B/C/… (`A1 B1 C1 A2 B2 C2 …`, never `AAA BBB`), 6 arms × 3 rounds,
`--screenshot-delay 300`, Stratis at the owner's camera
(`--test-world-freefly 2446.86 4413.66 67.97 115.5 -8.2`), **1920×1080**, nothing
else running. Interleaving matters because GPU clock ramp and thermal drift move
monotonically with wall-clock time: blocked, that drift is charged entirely to
whichever arm ran second; interleaved, it lands in every arm as spread.

## Result

| arm | median | min | max | vs base |
|---|---:|---:|---:|---|
| base | **34.36** | 30.40 | 48.00 | — |
| `WGR_ABLATE=planar` | **26.42** | 22.52 | 35.87 | **−7.94 ms (23%)** |
| `WGR_ABLATE=ao` | 31.37 | 27.48 | 69.36 | −2.99 ms |
| `WGR_ABLATE=clouds` | 32.31 | 27.71 | 71.13 | −2.05 ms |
| `WGR_ABLATE=grass` | 39.53 | 28.72 | 59.37 | +5.17 ms (noise) |
| `WGR_MSAA=1` | 67.41 | 59.55 | 72.64 | **+33.05 ms** |

Named regions from a base run, which corroborate the planar result independently:

```
30.395  GPU frame total        1.135  Planar: clouds
 3.137  Planar: terrain        0.884  Planar: objects
 1.664  Grass colour           0.734  AO bilateral blur
 1.468  AO horizon march       0.636  Grass prepass
```

`Planar: terrain + clouds + objects` = 5.16 ms of named work against a 7.94 ms
ablation; the difference is the resolve and the mip chain, which is consistent.

**34 ms at 1080p is under 30 fps. The owner is right, and the water reflection is
the single item large enough to matter on its own.**

## Why it costs so much, and why that is the wrong shape

The planar pass re-renders terrain, the retained object set, and a full cloud
march from a mirrored camera, at half resolution but with **4× MSAA**, then
resolves it and reduces it to a roughness mip pyramid before the water shader
samples it.

Two things are wrong with that independently of its absolute cost:

1. **MSAA on a target that is immediately blurred buys almost nothing.** Only mip
   0 is read by near-mirror-flat water; everything rougher samples a blurred mip.
2. **None of the cost scales with how much water is on screen.** At the camera
   above, the sea is a distant strip on a hillside view and the frame pays full
   price for it.

## Two anomalies, unexplained and not to be acted on until they are

- **`WGR_MSAA=1` is 33 ms SLOWER than 4×**, consistently across all three rounds
  (min 59.55). That is not plausible as a real cost. Something in the 1-sample
  path is pathological — a missing fast path, a resolve that degenerates into a
  full copy, or a pipeline rebuilt per frame. Worth finding regardless of MSAA
  policy, because whatever it is may also cost something at 4×.
- **`WGR_ABLATE=grass` measured slower than baseline.** The ranges overlap almost
  entirely, so this reads as noise — but it means there is still **no measured
  cost for grass**, and if the gate is not actually removing the work that is a
  defect in the gate.

## What to do, in order

1. **A coverage gate on the planar pass.** Estimate the water's screen coverage on
   the CPU from the water CDLOD nodes — they are non-overlapping flat quads at sea
   level, so projecting their corners and summing clipped screen areas is a real
   number costing microseconds, and it is available *before* the pass is recorded.
   An occlusion query would be exact but answers a frame late, which is the wrong
   shape for a gate that decides whether to render at all. Fade the contribution
   out across a band *before* skipping the pass, so the handover to the
   environment-map reflection completes before the target stops updating; a bare
   threshold would pop exactly where a shoreline crosses the horizon.
2. **Drop the planar target to 1 sample.** Note this is not a one-line change: the
   reflected passes reuse the main pipelines, whose `MultisampleState` is baked at
   creation, so it needs a second pipeline set at 1 sample.
3. **Reflect at a lower fraction than ½**, and consider dropping the reflected
   cloud march in favour of the sky env map for all but the smoothest water.

A partial implementation of (1) — the coverage estimator and the faded gate — is
saved at `planar-coverage-gate.partial.patch` in the session scratchpad. It
references `PlanarConfig`, `screen_coverage_of_water` and a `planar_dbg_last`
field that were not written before the work was interrupted.

## The lever that exists today

`WGR_ABLATE=planar` removes the pass and returns the 23%. It removes planar water
reflections entirely, so it is a quality trade rather than a fix — but it is
measured, available now, and the right thing to reach for on a machine that
cannot hold 60 fps.
