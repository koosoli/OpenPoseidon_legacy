# Water interaction calm patches (2026-09-08)

Owner report: shooting water produces ripples followed by a circular patch with
no visible ocean waves.

## Cause and correction

The surface shader blended the ocean/shore normal toward a ripple-only normal,
up to 95 percent, using absolute interaction heights as the blend weight.
A nearly uniform residual height therefore replaced ocean wave detail with an
almost upright normal even after the visible ripple slope faded.

Add the interaction height gradient to the existing ocean height gradient
instead. In normal coordinates this is `normalize(n + slopeXZ * n.y * 0.95)`:
no division, no additional texture fetches, buffers or rendering passes. Keep
the existing two-scale ripple sampling/gain. Geometry already adds interaction
displacement to ocean displacement; neither FFT simulation nor wave amplitude
is disabled or changed. The same shared path handles bullets, wakes and wash.

## Verification

- All 17 `water::tests` pass, including real water shader composition.
- New analytical regression test covers flat residual offsets of either sign,
  calm/tilted/steep ocean normals, active gradient addition and ripple decay.
  It also checks that the production expression matches the tested equation.
  This is not a GPU pixel-output test.
- `PoseidonGame` and `PoseidonTests` build successfully.
- Installed before/after interaction demo smoke: pending deployment.
- Exact owner shooting sequence and visual acceptance are not yet verified.

Private capture fixture: `.tmp-rotor-smoke/ripple.ps1`, fixed Nogova water view,
wind 18, interaction demo enabled, 20-second capture. The demo emits object
impacts every three seconds, so it exercises the shared normal path but is not
a single-bullet decay replay. No frame-rate improvement is claimed.
