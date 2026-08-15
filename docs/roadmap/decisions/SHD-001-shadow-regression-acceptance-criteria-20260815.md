# SHD-001 — Acceptance criteria for the shadow regression, written from the owner's reference shot

**Date:** 2026-08-15
**Branch:** `feat/texture-streaming`
**Status:** **Criteria only — written BEFORE the verification run, on purpose.**
**Related:** the cascade gate fix (`bc59281`), owner feedback §2 in `HANDOVER-PROMPT.md`

## Why this file exists

The gate fix is judged by eye, and "does this capture look right?" decided *after* seeing the
capture is not a test — it is an invitation to accept whatever came out. The owner supplied a
"before" screenshot of Arma 2 with correct shadows. These criteria are written from that image
while it is the only evidence in hand, so the run either meets them or does not.

## The reference

Owner-supplied, Arma 2, mud-brick architecture (Takistan-family). Camera is **inside** a room
looking out through a doorway/window opening.

What it shows, in the order these matter:

1. **The interior is dark, and it is dark by occlusion.** The room is lit only where the opening
   admits light. The walls are blocking the sun. This is the single thing the regression destroys:
   an inactive gate does not dim shadows, it stops filling `cameras[i].shadow`, so every receiver
   in the frame goes unshadowed and sun lands on the floor *through* solid walls.
2. **A hard-edged sun shaft on the interior floor**, bottom-left, thrown through the opening. Its
   edge is crisp — a few pixels of penumbra at most.
3. **Ground shadows outside the doorway.** A dark region with a straight edge sits on the terrain
   beyond the threshold, cast by the building. The owner called these out specifically
   ("foremost the ground shadows").
4. **The door reveal is shadowed on its inner faces** — the frame's own thickness is casting.

## Why point 3 is the load-bearing one

Terrain is a **receiver, never a caster**: it does not feed `_shadowCasters` (whose only two
feeders are SceneShadowPass.cpp:600 and :653). So a shadow on the ground depends entirely on the
cascade matrices the gate withholds. Ground shadows are therefore the cleanest possible read on
whether the gate is open — they cannot be produced by any partial or fallback path.

## Acceptance criteria

A capture at a matched pose, on Arma 2, default settings:

- [ ] **A1 — Interior is occluded.** No sunlight on floor or walls except through the opening.
- [ ] **A2 — Sun shaft present, and its edge is crisp.** Not merely "a bright region": the edge
      must be comparable to the reference, not a soft gradient.
- [ ] **A3 — Ground outside carries the building's shadow**, with a straight edge.
- [ ] **A4 — `WGR_SHADOW_GATE_TRACE=1` reports `active=1`** at that pose, and the log records
      `cpuCasters` and `gpuInstances` so the *reason* it is active is on record.

And the A/B that makes it mean something:

- [ ] **B1 — `WGR_SHADOW_LEGACY_GATE=1` at the same pose reproduces the BUG** (sun through walls).
      If the legacy arm also looks correct, the gate was never the cause here and A1-A3 passing
      is a coincidence of pose. **This is the criterion most likely to be skipped and it is the
      one that makes the rest evidence rather than decoration.**
- [ ] **B2 — `cpuCasters=0` in the failing arm.** If it is non-zero, the gate is exonerated and
      the fault is inside the GPU-driven cascade path. Report that outcome loudly; it invalidates
      commit `bc59281`'s stated mechanism even if the change is harmless.

## The trap this fix sets for itself

In the reference shot the building was almost certainly **CPU-cast** — the gate was open, so
`_shadowCasters` was non-empty, so the building had not yet moved to the retained path. After the
fix that same building is **GPU-cast**, through `draw_gpu_driven_shadow` instead of
`AddShadowCaster`. **These are different code paths producing the same picture.**

So "shadows came back" is NOT sufficient. If A1 and A3 pass but A2 fails — shadows present,
shaft edge soft, or the shadow offset from where geometry says it should land — then the gate fix
worked and the GPU-driven cascade path has a defect of its own underneath it. The prime suspect
in that case is `_smCamPos`: it was **latent** under the old gate (stale only on frames the old
gate refused to render anyway) and the fix makes it **live**. That is why the two halves shipped
together, and it is the first thing to check if the shadows return in the wrong place.

## Not yet established

- Nobody has observed `_shadowCasters.size()` in a failing frame. Every claim in `bc59281` about
  *why* the list empties is static call-graph reading. A4/B2 exist to convert that into a
  measurement.
- The reference is Arma 2; the gate is world-independent, but retention coverage is not, so a
  pass on Arma 2 does not license a claim about Reforger or Arma 3. Per the house rule that a
  default is a claim about every world, at least one more world should be checked before this is
  called fixed everywhere.
