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

## Addendum — the sun and moon really were in front of the clouds

Asked to check this, the code reading said no: the sky pass draws the discs, the
depth-aware cloud composite runs after geometry and blends `One / SrcAlpha` with
`SrcAlpha = cloud.trans`, so clouds attenuate them. The owner said otherwise, and
the owner was right. A capture at Everon (600, 600), hour 9, with
`POSEIDON_TEST_RAIN=1` — which now forces the overcast as well as the density, so
there is a real deck to test against — shows a **sharp white solar disc through a
full rain deck** (`sun-overcast-before.png`).

Two causes, both arithmetic.

1. `march_clouds` early-outs at `trans < 0.01` and **returned that value**. So the
   deck's floor transmittance was 1%, never 0.
2. The sun disc's radiance is the solar irradiance divided by the solar solid
   angle — about **1.5e4x** the surrounding sky, which is what makes it read as a
   disc under HDR + bloom. One per cent of 1.5e4 is still 150x the sky. The deck
   was attenuating it correctly and it did not matter.

Fixed at both ends. The march rescales its accumulated transmittance so the cut
maps to exactly zero (continuous, monotonic, unchanged near 1), and the discs are
additionally attenuated by the SQUARE of their own ray's cloud transmittance —
cubic once the composite's own linear pass is counted. That is not a fudge: cloud
destroys the *coherent* disc long before it stops passing light, so a thin veil
still gives a bright hazy sun while a rain deck gives a diffuse glow and no disc.
The extra march runs only for pixels inside a disc — a few hundred — so it costs
nothing measurable.

`sun-overcast-after.png` is the same camera, same weather: a soft brightening
where the sun is, no disc, and the clear-sky gap at the frame's edge unchanged.

## Addendum 2 — and so were the clouds, one layer up

Same bug, same cause, different pass. The owner's repro: a Nogova pier,
`[4100.15, 3348.03, 7.06] az 253.3 el 5.0`. Every ornamental post of the iron
railing that stood against the sky was eaten by the cloud deck behind it; the
posts below the horizon, against the sea and the far shore, were solid.

The cloud composite is depth-aware — `fs_cloud` bounds its march at the resolved
scene depth so a cloud beyond a hill is not marched and a cloud in front of one
occludes it. Alpha-BLENDED draws write no depth. So for a pixel covered by the
railing against the sky, the resolved depth IS the sky, the march runs the full
distance, and `dst * trans + inscatter` paints cloud straight over the railing.

The composite now runs BEFORE the post-water transparent sub-pass rather than
after it, which is the engine's own order: `Landscape::Draw` submits terrain,
water and `DrawClouds`, the queues flush, and only then does
`Scene::ObjectsDrawn` emit the sorted transparents. Clouds are background.

The trade is explicit: a cloud NEARER than a transparent no longer occludes it.
In practice that means flying a cloud between the eye and a fence ten metres
away — against every near transparent in the game being erased by the sky behind
it. Not a close call.

Evidence: `nogova-pier-after.png` (the owner's camera, railing intact against the
deck) and `cloud-order-20260822-original-training-capture.png` (a normal frame,
unregressed — clouds still occlude terrain and the world renders).
