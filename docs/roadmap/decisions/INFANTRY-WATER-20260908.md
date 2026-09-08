# Floating infantry bodies (2026-09-08)

Base: `03946541`, worktree `asset-obs`, foreign Cargo.lock untouched.

## Implemented

Dead infantry now uses GroundWater contact points as a supporting surface,
with 0.12 m immersion and ascent limited to 1.5 m/s. Solid ground remains
authoritative when higher, preserving shore/deck support. The existing hide-body
offset is already included in collision depth and is not subtracted twice.

Water contact prevents the settled-body early-out and refreshes movement time,
so the frozen-corpse shadow cache does not freeze a floating pose. Fully hidden
bodies retain the normal cleanup path. Dead players no longer receive the living
player's competing spring buoyancy, and dead AI receives no repeated drowning damage.

This deliberately does NOT grant living AI swimming or change boarding logic.
The reported campaign deaths still need mission/save reproduction.

## Checks

- BodyWaterSupport: five assertions passed (equilibrium, ascent cap, zero/negative
  timestep and a receding surface).
- PoseidonGame and PoseidonTests build successfully.
- Installed matched EXE/DLL, `03946541+dirty`, deploy 19:39 via game lock.
- Nogova two-SoldierWB fixture: kill at t=2 s, one placed at sea level and one
  3 m below; both visible at the surface in the t=30 s capture.
- Same fixture at wave amplitude 3 and wind 18 m/s: both visible at t=35 s.
  Private captures: `.tmp-rotor-smoke/body-water.png`, `body-wave.png` and logs.
  No engine/script error was observed in these runs.

## Limits / follow-up

Water collision uses the existing CPU wave predictor, NOT GPU FFT readback.
It follows local wave scale and time but does not match every rendered crest.
These captures prove surface support, not perfect crest alignment, natural drift,
ragdoll motion, every death animation, multiplayer or mission-specific boarding.

Cloud trailing inspection found no independent history in godrays/cloud composite;
the temporal reactive mask only classifies clear-depth sky and water below mean
level. Clouds composited in front of opaque terrain are not explicitly represented
there. This is a candidate for controlled DLSS A/B, not yet a verified fix.

Upstream/main is `3541affa`. `a61130ca` scene-preloader reset is applicable to
the shared engine; `9a298d09`'s nested-bounds branch is absent in this fork's
AnimatedBSphere, so that patch must not be copied mechanically.
