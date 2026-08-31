# REN-THR-002 — The frame the renderer is handed, hashed, and why that is not yet a proof

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Roadmap:** Phase 5, step 6 — *"prove lockstep equivalence before enabling overlap"*

Phase 5 says the producer and the renderer may only be allowed to overlap once it has been
**proven** that restructuring the producer did not change what the renderer receives. Every
remaining step in the phase — the command queue, the instance table crossing the boundary,
the handoff itself — is a restructuring of the producer, so the proof is the tool the rest
of the work depends on, not a formality at the end.

This built that tool and then used it to find out that the proof Phase 5 asks for is not
currently available. The tool stays; the finding changes the plan.

## The instrument

`EngineWgpu::HashPublishedFrame` hashes the exact bytes of `WgrFrame` at the moment of
publication — the seventeen slices plus the scalars, each slice's length folded in ahead of
its contents so an empty slice and a missing one differ and two adjacent slices cannot be
mistaken for one longer one. Off unless `WGR_FRAME_HASH=1`, because it walks every
published byte.

**It is not vacuous, and that was checked before anything was concluded from it.** Over one
run: 1,163 frames published, **881 of them differing from their immediate predecessor**. A
hash that reported a constant would have been a hash of nothing, which is the failure SIM-808
records for the simulation timeline and the one this kind of instrument fails at silently.

## The finding: the published frame stream does not reproduce across runs

Three runs, same build, same scene, same settings, same machine, back to back.

The first published frame is **bit-identical in all three** — `0x75598166fc8800fd`. That is
worth stating because it means the producer's construction of a frame is deterministic given
identical inputs; nothing about pointer values, iteration order or uninitialised padding
leaks into what gets published.

Everything after it diverges:

| | |
|---|---:|
| frames hashed per run | 96 |
| longest identical aligned prefix, run 1 vs 2 | **3 frames** |
| longest identical aligned prefix, run 1 vs 3 | **2 frames** |
| distinct hashes appearing in all three runs | **8 of 96** |
| equal at the same index, run 1 vs 2 | 37 of 96 (mostly repeated loading frames) |

An intermediate reading of an 8-frame sample suggested the runs were merely *phase-shifted* —
the same content sequence offset by a frame or two of loading — because a run of four
consecutive hashes did appear in all three at different offsets. **That was a coincidence of
the loading phase and it does not survive 96 frames.** Searching every offset from −8 to +8
finds no alignment better than three frames. Recorded because it is the wrong conclusion the
short sample invited, and a longer sample was what refuted it.

## Why, and what it means for the plan

The simulation runs a fixed 1/60 s step but takes a **variable number of steps per frame**
(`FixedStepAccumulator`, `World.cpp:904-916`), and the run is ended by a wall clock rather
than a frame count — the three runs published 1,163, 1,232 and a third figure. So frame *N*
of one run and frame *N* of another have not simulated the same amount of world, and there
is no reason for their contents to match. The nondeterminism is in the **pacing**, not in
the frame construction, which is exactly what the identical first frame shows.

So: **a run-to-run frame hash cannot be the equivalence proof Phase 5 step 6 asks for**, and
no amount of care in the producer restructuring will make it one. The prerequisite that
nobody had listed is a **deterministic replay mode** — a fixed number of simulation steps per
frame and a fixed frame count, with no wall-clock input to either — under which two runs
publish the same number of frames with the same contents. With that, this instrument becomes
the proof immediately; without it, it measures the clock.

That prerequisite is now the next step of Phase 5, ahead of the queue, because the queue is
the first change that needs proving and there is currently no way to prove it. It is also
cheap relative to what depends on it, and it composes with SIM-813, which removed the last
wall-clock input to the *simulation* stage earlier today for the same underlying reason.

## What this does not say

- **It does not say the engine is nondeterministic.** It says the frame *stream* is not
  reproducible under wall-clock pacing. Those are different claims and the identical first
  frame is evidence for the narrower one.
- **One scene, one machine, three runs.** A scene with less loading might align further; it
  would not change the conclusion, because the pacing argument is structural.
- **The hash covers what `WgrFrame` carries.** Renderer state pushed through the twelve
  `wgr_set_*` calls is not in it, and neither are resource uploads — REN-THR-001's other two
  shapes. A restructuring that moved only those would look equivalent to this instrument
  while changing what is drawn.
