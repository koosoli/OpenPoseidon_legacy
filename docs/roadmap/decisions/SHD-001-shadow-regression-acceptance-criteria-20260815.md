# SHD-001 — Acceptance criteria for the shadow regression, written from the owner's reference shot

**Date:** 2026-08-15
**Branch:** `feat/texture-streaming`
**Status:** Criteria written before the run; **RESULTS ADDED — the fix is UNVERIFIED, the bug was not reproduced.**
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

---

# RESULTS (2026-08-16, added after the runs)

**Verdict: the fix is UNVERIFIED. The bug was not reproduced, on either world tried.**

Four arms, 1600×900, `WGR_SHADOW_GATE_TRACE=1`, binary 2026-08-16 00:55.

| world | pose | arm | cpuCasters | gpuInstances | active |
|---|---|---|---:|---:|---|
| Takistan (A2) | 6770.7, 6099.8, alt 312.9, hour 8 | fixed | **7** | 55 | true |
| Takistan (A2) | same | legacy | **7** | 55 | true |
| Everon (Reforger) | 5200, 7000, alt 220, hour 8 | fixed | **5** | 128 | true |

**`cpuCasters` was never 0 in a real exterior view.** Criterion **B1 fails**: the legacy arm did
*not* reproduce the bug, because 7 surviving CPU casters hold the old gate open just as well as the
new one. The two Takistan captures differ on 2.1% of pixels with identical means — cloud and TAA
jitter, no shadow difference. Criterion **B2 fails** for the same reason.

So this is a genuine null, and it cuts two ways:

- **No regression.** The fix is inert where the old gate was already open, which is the safe
  outcome and is worth having established.
- **No confirmation.** The claim in commit `bc59281` that "a view whose visible set is entirely
  retained submits zero CPU casters — an interior does that" is **still only static reading**. It
  was not demonstrated, and the owner's screenshot is still unexplained.

## Two traps this run walked into, recorded so the next attempt does not

1. **The third trace line in every run is teardown, not steady state.** Each run ends
   `active=false cpuCasters=0 cascadesValid=false`, which looks exactly like the bug. It is not:
   the screenshot timestamps land **8–32 ms before** it (Takistan fixed 00:58:49.467 vs
   00:58:49.499; Everon 01:03:36.983 vs 01:03:36.993). Reading that line as a result would have
   produced a confident, wrong confirmation.
2. **`--test-world-freefly` takes X Z Y, but the log prints (x, altitude, z).** The first attempt
   copied a pose straight out of a log and put the camera at 6099 m, photographing the tops of the
   clouds. Nothing errored, and `cpuCasters=0` there — trivially, because nothing was in view. That
   near-miss would have "confirmed" the mechanism from an empty frame.

## What the exterior data actually suggests

An ordinary exterior keeps a handful of CPU casters alive (5 on Everon, 7 on Takistan) even at
97%+ retention, so **the knife-edge is harder to reach than the mechanism implied**. The residual
casters are presumably the objects that fail retention — proxies, non-GPU-owned sections, or
whatever the foliage/cutout bail still rejects.

That makes the interior hypothesis *more* specific, not less: an interior is one of the few views
whose entire visible set can be a single retained building. But it now needs to be demonstrated,
not assumed.

## The run that would settle it, and why it was not done tonight

An **interior camera on Takistan**. The freefly harness only takes a world coordinate, and placing
one inside a specific building needs a known building position, which is not derivable from the
world file in the time available. Two workable routes:

1. The SQF camera built for the moving-camera work (`scripts/farfield-movecam.ps1` generates an
   `init.sqs` that `camCreate`s a real camera) can be positioned and re-aimed freely, and could be
   walked to a building interior.
2. Force the condition directly rather than finding a pose for it: a debug env that skips the CPU
   caster submission entirely (`_shadowCasters.clear()` before the gate) would prove the
   *consequence* — whether an empty list really kills all cascades — without needing to find a
   natural view that empties it. That is a cheaper and more direct test of the actual claim.

Route 2 is the better next step. It tests the mechanism rather than hunting for a pose.

---

# FINAL VERDICT (2026-08-16, after an INTERIOR pose was obtained)

**The cascade gate was not the owner's shadow bug. The fix is defensive hardening, not a repair.**

The owner supplied a coordinate for an unrelated complaint ("strange triangles") which turned out to
be **inside a building** — the interior view that could not be constructed from the freefly harness
all night, and the exact case the fix was built for. Re-run as a two-arm A/B there:

| pose | arm | cpuCasters | gpuInstances | active | mean luminance |
|---|---|---:|---:|---|---:|
| Takistan interior 6907.68 / 6017.04 / alt 313.98 | fixed | **8** | 19 | true | 12.58 |
| same | legacy | **8** | 19 | true | 12.45 |

Identical. 4.4% of pixels differ at all, and `dark%` (72.8 vs 72.9) and `bright%` (0.24 vs 0.24) are
the same to two decimals — TAA and animation jitter, not shadowing.

**So `cpuCasters` was never 0 across every pose tried:** Takistan exterior (7), Everon exterior (5),
and now a Takistan *interior* (8). The premise in commit `bc59281` — "a view whose visible set is
entirely retained submits zero CPU casters; an interior does that" — is **false in practice**. Even
indoors, enough objects fail retention (proxies, non-GPU-owned sections, the player model itself)
to keep the list populated and the old gate open.

The interior capture also shows **correct shadowing**: the room is dark, the walls occlude, and
there is a sun patch on the floor through the window — the reference behaviour from the owner's
screenshot, on both arms.

## What the fix is now worth

It is not worthless, and it should stay:

- The defect it removes is **real**: `shadowActive` gates `cameras[i].shadow`, so an empty CPU
  caster list genuinely would disable every cascade including the GPU-driven ones that render fine.
  Rust has been ready for that case since the multi-view slice; C++ simply never sent the frame.
- It is **measurably inert** where the old gate was already open — three poses, two worlds, no
  pixel difference beyond jitter. That is the safe outcome.
- It removes a latent knife-edge that a future increase in retention coverage would have walked
  straight into. Retention has been growing all session.

But it must **not** be reported as fixing the owner's regression. It does not, and the owner's own
follow-up ("might be that it is already fixed, so not seeing the same problem currently") suggests
his symptom is either intermittent or was addressed by something else.

## What is still unexplained

The owner's before/after screenshots showed sunlight on an interior floor **ignoring the walls**.
Nothing measured this session reproduces that. Possibilities not yet excluded, in order:

1. **Intermittent.** A transient state during streaming — note every run shows
   `cascadesValid=false` while `gpuInstances` climbs, and the world spends 100+ seconds there.
   A player moving through a fill-in could sit in a no-cascade state for seconds at a time. **This
   is the strongest remaining hypothesis and it is testable with the moving-camera harness.**
2. A different code path entirely (the projected accumulator, or a per-material lighting term).
3. Already fixed by another change this session — the decal bias, the foam work and the gate all
   landed together.

`WGR_SHADOW_FORCE_NO_CPU_CASTERS=1` was added to test the *consequence* directly rather than hunt
for a pose. Given `cpuCasters` is never 0 naturally, that test is now academic for this bug — but
it remains the right tool if hypothesis 1 is pursued.

## Not yet established

- Nobody has observed `_shadowCasters.size()` in a failing frame. Every claim in `bc59281` about
  *why* the list empties is static call-graph reading. A4/B2 exist to convert that into a
  measurement.
- The reference is Arma 2; the gate is world-independent, but retention coverage is not, so a
  pass on Arma 2 does not license a claim about Reforger or Arma 3. Per the house rule that a
  default is a claim about every world, at least one more world should be checked before this is
  called fixed everywhere.
