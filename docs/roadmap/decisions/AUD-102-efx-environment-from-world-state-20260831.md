# AUD-102 — EFX environment from world state: what was already there, and what was wrong with it

Date: 2026-08-31
Branch: `agent-audio-efx` (from `asset-obs-001`)
Roadmap: `docs/roadmap/POST-DLSS-MODERNISATION-ROADMAP.md` § 1.4 "Small audio wins", **item 2**.
Companion: `AUD-101-hrtf-probe-and-modes-20260831.md` (item 1).

## The honest starting position

This work was briefed as "drive the existing EFX environment from world state". It was
already driven, and the roadmap's own phrasing understates how long that has been true.

- **The outdoor classifier is 2001 code.** `World::PerformSound` has always read the
  terrain cell under the camera and picked `SEForest` / `SECity` / `SEMountains` /
  `SEPlain`, with sizes and densities derived from `GeographyInfo` and the surface
  height. Nothing about it was constant or unwired. Four of the roadmap's five
  classifications shipped in *Operation Flashpoint*.
- **The fifth classification and the blend landed the day before this session**, in
  `c2ade293` ("SERoom gets a producer, and reverb transitions blend instead of
  cutting"): an `IndoorRoomAt` query at the camera produces `SERoom`, and
  `TickEnvironmentBlend` eases the reverb parameters instead of switching.
- **And they were parked hours later**, in `ba26ba05`, after the owner's A/B verdict:
  *"klingt nicht richtig und nicht gut"*. Both now sit behind
  `POSEIDON_EXPERIMENTAL_EAX=1`, default OFF, which restores the pre-2026-08-30 path
  exactly — four environments, hard preset switches, no interior query.

So the item was not unimplemented. It was implemented, rejected by ear, and shelved.
Writing it again would have produced the same sound. This change therefore does three
different things: it makes the two halves testable against the code the engine actually
runs, it names one concrete defect in the parked version that is very likely part of why
it sounded wrong, and it writes down what the engine genuinely cannot hear.

**Nothing here has been listened to.** The parked feature is still parked and still
default-off. See "What needs the owner's ears".

## What drove the reverb before this change

`World::PerformSound` → `IAudioSystem::SetEnvironment(SoundEnvironment{type, size, density})`
→ `SoundSystemOAL::DoSetEAXEnvironment` → a named preset from `EFXPresets.hpp`, decay
scaled by `size / preset.defaultSize`, density overwritten as `size / 100`, pushed to a
single auxiliary effect slot.

The decision table, unchanged by this work, in priority order:

| Condition on the cell under the listener | Environment | size | density |
|---|---|---|---|
| inside a building room *(experimental only)* | `SERoom` | 2.0 – 8.0 (room preset scale) | preset's own |
| `forestInner \|\| forestOuter` | `SEForest` | 38 | 0.5 |
| `howManyHardObjects > 0` | `SECity` | `(4 - n) · 15` | `n / 3` |
| `surfaceYAboveWater > 170 m` | `SEMountains` | `clamp(y - 120, 50, 100)` | 0.5 |
| otherwise | `SEPlain` | `75 - objects · 15` | 0.5 |

Two long-standing oddities are preserved deliberately, because changing them changes
shipped audio and neither is what the owner reacted to: `SEForest` maps to the **city**
preset (1:1 with `soundDX8.cpp:2574` — it was wrong in 2001 too), and the size/default
ratios are large enough that forest and dense city ask for decay times of several
seconds. Both are candidates for the tuning pass; neither is touched here.

## What this change actually does

### 1. The classifier is now one testable function

`Audio::ClassifyEnvironment` in `engine/Poseidon/Audio/EnvironmentClassify.hpp` takes an
`EnvironmentSurroundings` (indoors? forest? how many hard objects? surface height?) and
returns the `SoundEnvironment`. `World::PerformSound` fills the struct and calls it. The
branch bodies moved verbatim — this is a lift, not a redesign, so the tests that pin it
are pinning shipped behaviour rather than a new opinion.

This matters beyond tidiness. The pre-existing `test_eax_efx.cpp` pins the
`SE*` → preset mapping against a **local re-implementation of the switch**, so the engine
could drift away from its own test without a single case failing. The new tests call the
function the engine calls.

### 2. The blender is now one testable object

`Audio::EnvironmentBlender` in `engine/Poseidon/Audio/EnvironmentBlend.hpp` is a
fixed-capacity float-vector ease. `SoundSystemOAL::TickEnvironmentBlend` gathers the 26
blended EAX reverb scalars into an array, ticks the blender, scatters the result back and
writes the effect slot. The easing arithmetic is identical to what `c2ade293` inlined;
it now lives somewhere that does not need a sound device to exercise.

Deliberately not blended: `iDecayHFLimit`, which is a limiter on/off flag. Half a flag is
not a sound; it takes the target's value.

### 3. One concrete defect in the parked version, fixed

`ComputeEFXTarget` overwrote the preset's density with `envSize / 100`. That is
dimensionally sane for every outdoor environment — plain 75, forest 38, city 15–45,
mountains 50–100 all land at a plausible 0.15–1.0.

**`SERoom` does not live in that scale.** Its size is expressed in the `room` preset's
own units (default 1.9, capped at 8 by the producer), so `envSize / 100` handed the
reverb a modal density of **0.02 – 0.08**, against the **0.4287** the ROOM preset was
authored with. Near-zero EFX density is the hollow, coloured, ringing end of that
control. Every interior in the parked build was running a reverb roughly twenty times
thinner than the preset intends.

That is a specific, checkable reason for "klingt nicht richtig" that does not require
anyone's ears to state. Interiors now keep the preset's authored density and the size
scales decay only. The branch is unreachable without `POSEIDON_EXPERIMENTAL_EAX`, so the
shipped path is byte-for-byte unchanged.

It is **not** a claim that this was the whole complaint. "Nicht gut" is a judgement about
a whole sound; this fixes one thing that was measurably wrong inside it.

Also corrected: the room-size floor was 1.9, below the `[2, 100]` clamp
`DoSetEAXEnvironment` applies, so it was never reachable. It now says 2.0.

## The blend time constant

`kEnvironmentBlendTauSeconds = 0.6f` — seconds, exponential ease, one constant for all
26 parameters.

0.6 s sits between two audible failures. Much shorter and crossing a doorway is still
heard as a step, which is the hard cut the roadmap asks us to remove. Much longer and the
reverb lags the picture: you are well inside a building before it sounds like one, and a
listener who steps in and straight back out hears a slow filter sweep instead of a room.
An exponential ease is ~95% converged after three time constants, so 0.6 s completes a
transition in roughly two seconds — about the time it takes to walk through a door and
keep going, which is the transition this exists for.

Two guards around it, both with named constants:

- `kEnvironmentBlendMaxStepSeconds = 0.25f` caps the frame delta. Without it, one long
  frame after a loading hitch advances the ease to completion and reintroduces exactly
  the hard cut being removed.
- The ease factor is `1 - exp(-dt/tau)`, which is in `[0, 1)` for every input including
  zero and infinity. That bound is why each parameter's trajectory is monotonic and never
  overshoots — the property the tests assert.

One time constant for every parameter is a simplification, not a claim. Decay time and
high-frequency gain do not have to move at the same rate to sound right. If the owner
reports that the transition *sweeps*, splitting the constant per parameter group is the
first thing to try.

## What the engine genuinely cannot distinguish

Stated plainly, because the roadmap's five names imply more resolution than exists:

- **Forest and city are real classifications**, read from the terrain's per-cell
  `GeographyInfo`, which the world builder computed offline. The engine is not inferring
  them at runtime and cannot second-guess them.
- **"Mountains" is not a terrain classification at all.** It is a bare altitude test:
  surface height above 170 m. A high plateau reads as mountains; a deep valley between
  two peaks reads as plain. Nothing measures enclosure, slope, or how far away the rock
  actually is — the one cue that would make mountain reverb correct.
- **The classifier sees one cell.** Standing at the edge of a wood, in a courtyard, on a
  street versus in a square, or at the mouth of a valley are all invisible to it. There is
  no notion of how far the nearest reflecting surface is in any outdoor direction.
- **`SECity` counts objects, not geometry.** Three hard objects in a cell is treated as
  a tight space regardless of whether they are a terrace of houses or three fence posts.
- **Only the interior test is a real query.** `IndoorRoomAt` plus `IndoorClearanceAt` do
  look at building geometry, which is why `SERoom` can size itself and the outdoor
  environments cannot. (`IsSheltered` was deliberately rejected as the indoor test: its
  documented crudeness puts a tree canopy or a bridge underside into room reverb.)
- **There is one reverb for the listener, no per-source occlusion.** A shot fired inside
  a building and heard from outside gets the outdoor reverb, and vice versa.

Doing better than this is `AudioWorld` work, not roadmap 1.4.

## Test coverage added

`tests/unit/engine/Poseidon/Audio/test_environment_blend.cpp`, tags
`[Audio][EAX][EnvClassify]` and `[Audio][EAX][EnvBlend]`. Against the production
function and the production object, not copies of them. No sound device required.

Classifier: the priority order (indoors beats every outdoor cue; forest beats altitude),
the 2001 sizes, that city gets tighter and denser as hard objects accumulate, that the
mountains threshold is exclusive, and — pinned so it stays visible — that the mountains
size band saturates, so a 300 m hill and a 3000 m peak sound identical.

Blender: the first application snaps (there is nothing plausible to fade in from);
a settled blend costs nothing and writes no device state; every parameter approaches its
target monotonically and never overshoots; settling takes more than one time constant and
less than twelve; **a change mid-blend retargets from where the ear already is rather
than restarting**, including the step-in-step-straight-back-out doorway case; a 30-second
stalled frame cannot teleport the blend, and moves it exactly as far as a
`kEnvironmentBlendMaxStepSeconds` frame; a zero-length frame moves nothing and does not
divide; and `Invalidate` (what `DeinitEFX` calls) forces the next application to snap.

## What needs the owner's ears

The feature is still parked. Nothing below happens on a default build.

1. Run with `POSEIDON_EXPERIMENTAL_EAX=1` and EAX enabled in Options → Audio.
2. **Interiors, which is where the fix is.** Walk into a small building and stand still.
   The previous build made every interior thin, hollow and ringing — a near-zero modal
   density. It should now sound like a room with walls. If it still rings, the density
   was not the problem and the room preset itself needs replacing.
3. **Interior size.** A stairwell against a barn hall or a hangar. They should differ, and
   nothing indoors should sound like a church. If everything indoors sounds the same, the
   clearance mapping is too narrow; if a barn sounds like a cathedral, it is too wide.
4. **The doorway transition, which is what the 0.6 s constant is for.** Walk in, then walk
   straight back out. Two failure modes to listen for and they need different fixes: a
   *step* means the constant is too short (or blending is off — check the env var), and a
   *sweep* — an audible filter movement rather than a change of place — means it is too
   long, or that one parameter should not be moving at this rate.
5. **Outdoors, the pre-existing behaviour, which nobody has ever tuned.** Forest still
   uses the *city* preset (2001 parity), and forest and dense city ask for multi-second
   decay times. If open terrain sounds wrong, that is older than any of this work and is
   the next thing to change — say so and it becomes its own item.

Point 5 is the honest headline: the parts of item 2 the roadmap describes as new have
existed for twenty-five years and have never had a listening pass. The blend and the room
are the small new parts around them.
