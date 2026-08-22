# RND-042 — Water was composited over every alpha draw in front of it

Date: 2026-08-22. Owner report: *"water is in front of alpha channel stuff"*,
alongside *"still no ripples on the ocean"*. The two turned out to be unrelated
bugs that both hid the same thing — rain falling into the sea.

## The ordering bug

The engine's draw order is not ambiguous. `World::Draw` runs
`Landscape::Draw(scene)` — which submits the sea in `Landscape::DrawWater` — to
completion and flushes the queues, and only *then* calls `Scene::ObjectsDrawn`,
which is where the depth-sorted transparent objects, the cloudlets and the rain
streaks go. Water first, transparents after: exactly right.

The wgpu backend then ignored it. Water reconstructs the seabed by SAMPLING the
opaque prepass depth it also depth-tests against, so it needs a READ-ONLY depth
attachment, which the shared colour sub-pass (whose GPU-driven opaque pipeline
writes depth) cannot give. The frame therefore hoisted **every** `Plan3dOp::Water`
out of the sub-pass into a dedicated pass at the END of the segment.

That moved water past every alpha-blended draw in the same segment. None of them
write depth — they are blended sprites and sorted transparents — so nothing
stopped the sea painting straight over them: rain streaks falling into the water,
spray, smoke over a bay, glass, the alpha halves of trees on a shoreline.

## The fix

The sub-pass breaks where the op stream says it should. `render_frame` splits
`seg_ops` after the last `Water` op and replays the head in the existing colour
sub-pass, runs the water pass, then replays the tail in a second colour sub-pass
that LOADs what water left. The plan's order is preserved exactly; only the pass
boundary moved. The `ObjColorSegment` container now closes after the second
sub-pass, so the transparent replay stays inside the number that accounts for it
(it therefore also spans `WaterDraw`, which keeps its own row — containers
overlap their leaves here by construction).

## The ripple bug is a different one, in the same picture

`WaterWgpu` fed the interaction pass `Landscape::GetRainDensity()`. That number is
a slow random walk bounded by `overcast * 1.5 - 1` (`Weather::MoveClouds`): under
the dev panel's 0.95 overcast its CEILING is 0.425, its target inside that is
random, and it approaches it at about 0.01 per second. Meanwhile the particle
layer's own presets pin `densityOverride` to 1.0. So the sea was being rained on
at a fifth to a twentieth of the rain visibly falling into it, through a term
scaled LINEARLY by that density — which is why it read as no ripples at all.

Three changes: the water reads `GRain.EffectiveDensity()` (the density the drops
are actually falling at) or the weather's, whichever is louder; the shader's
response curve is `sqrt(density)` so a realistic 0.25 reads as half intensity
rather than a quarter; and the dev panel's "Rain ON" now forces Landscape's own
density too, because every consumer that is not one of the two render layers —
ripples, wet surfaces, visibility — reads that number and not `GRain`.

**Amplitude is bounded by the foam pass, not by taste.** `foam.wgsl` derives foam
from this field's height laplacian and slope flow, so a deep crater on a 1 m cell
injects foam whether or not the strike asks for any. At a -0.08 crater the whole
sea wore tan mottling that read as grit; cutting the strike's *own* foam term from
0.18 to 0.04 changed nothing, because that was never the source. Halving the
crater and kick is what quieted it.

## Evidence

Everon, `--test-world-freefly 600 600 25 45 -12`, noon, `POSEIDON_TEST_RAIN=1`:

- `docs/roadmap/evidence/rain-sea-20260822.png` — rain streaks visible ON the sea
  surface (they were behind it before) and the surface carrying rain chop.
- `docs/roadmap/evidence/norain-sea-20260822.png` — the identical camera with the
  hook off: smooth swell, no small-scale structure. The A/B is unambiguous.
- `docs/roadmap/evidence/rain-water-order-20260822-original-training-capture.png` —
  the pass split does not regress a normal frame.
