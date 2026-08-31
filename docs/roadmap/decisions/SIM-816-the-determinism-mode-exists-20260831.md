# SIM-816 — The determinism mode exists: 902 ticks, zero divergence, and what it took

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Follows:** REN-THR-005 (the residue "in the simulation"), SIM-815 (the tick-hash instrument
and the ranked candidates)

Phase 5 step 6 needs two runs of one scene to be comparable. REN-THR-005 ended with 28 of 40
frames differing under lockstep and the residue located "inside the simulation". SIM-815
built the instrument that could name it. This document is the third act: the causes, each
confirmed by moving the first divergent tick, and the environment under which the simulation
is now **bit-deterministic across an entire 902-tick run**.

## The hunt, in the order the instrument forced it

The tick-hash streams of two runs, joined on tick index. Every arm below is two fresh runs
of `perf_combat.eden` under `POSEIDON_LOCKSTEP_HZ=60 POSEIDON_TICK_HASH=1`.

| arm | first divergent tick | column | disagreeing ticks |
|---|---:|---|---:|
| baseline | 68 | `rng` alone (+2 draws) | 898 / 902 |
| + LOD governor pinned | 68 | `rng` alone | — |
| + radio on nominal duration | 68 | `rng` alone | — |
| + **fixed RNG seed** | **12,852** | `rng`+`clouds` together | **147 / 902** |
| + governor pinned again | **NONE** | — | **0 / 902** |

Three details that made this converge:

**Tick 67 ended at exactly 42,589 draws in all six early runs.** The draw *count* was
deterministic right up to the divergence — which reads as "a value-dependent branch takes a
different number of draws", i.e. the *values* differ from the start. That inference was
checkable one directory down: `RandomGenerator`'s **default constructor seeds the shared
sequential stream from `GlobalTickCount()`** (`randomGen.cpp`), and
`World::InitLandscape`-time code **re-seeds it from the wall clock again**
(`WorldInit.cpp:142`). Two "identical" runs never had the same random stream. The second
site is why the constructor fix alone measured as no change — the reseed silently undid it,
and only gating both moved the divergence three and a half simulated minutes.

**The radio was real but not first.** SIM-815's C2 (the drain waits on audio playback, a
real-time device) is a genuine nondeterminism and is now gated: under lockstep the radio
takes the accelerated-time branch that already existed — nominal message duration, counted
in simulation time. Fixing it shifted the draw baseline without moving tick 68, which is
what "true cause, wrong rank" looks like in this instrument.

**The last 147 ticks of divergence carried the C1 signature** — `rng` and `clouds` parting
in the same tick — and pinning the LOD governor (`WGR_LOD_GOVERNOR_RANGE=1`) removed them
entirely. That closes the chain REN-THR-003 documented statically: GPU frame time →
`GetAvgFrameDuration` → `_lodInvWidth` → `GetSmokeGeneralization` → cloudlet spawn cadence
→ shared-stream draws.

## The mode

```
POSEIDON_LOCKSTEP_HZ=60        # pins the frame deltaT; also switches the radio drain and
                               # the RNG seed to their deterministic paths
WGR_LOD_GOVERNOR_RANGE=1       # freezes the LOD governor's framerate feedback
POSEIDON_TICK_HASH=1           # the per-tick oracle (off by default)
```

Under it: **902 aligned ticks, zero divergence, every column** — positions, velocities,
camera, RNG draw count, shapes, entities, cloudlets. Normal play is untouched: the wall
clock seed (missions should not repeat) and the audio-clocked radio remain the default; every
gate reads `LockstepPacingActive()` or its env equivalent.

`POSEIDON_RNG_SEED=<n>` fixes the seed independently, and `POSEIDON_RNG_TRACE=lo:hi` prints
the return address of every sequential draw in a counter window — the tool that would have
named the consumer had the seed inference failed.

## And one level up, at the published frame

Tick-aligned (the mission load costs a variable number of frames, so frame indices do not
correspond — every earlier comparison stumbled over this), over 901 frames:

| slice | disagreeing frames |
|---|---:|
| cameras, palette, shadow casters, terrain, water, grass, 2D structure, scalars | **0** |
| `verts` (HUD) | 1 |
| `draws3d` | 882 |

And the rendered images are **98.93% pixel-identical with a maximum channel delta of 12** —
visually the same picture. The `draws3d` divergence is therefore **labeling, not content**:
the draw records carry mesh *handles*, handles are assigned in creation order, and creation
order rides asynchronous load I/O. The simulation is deterministic; the renderer's id
assignment is not, and does not need to be — but the oracle must learn to see through it.

## What remains for step 6

One refinement: a hash mode that masks (or translates to content-stable keys) the volatile
handle fields in `WgrDraw3D`, so "same frames" means "same content" rather than "same
labels". With that, the equivalence proof for every producer restructuring — the queue
slices, the thread — is a two-run diff that can actually reach zero.

## What this does not say

- One mission, one machine, 902 ticks ≈ 15 simulated seconds per run. Longer runs can
  surface slower clocks; the instrument is cheap enough to leave running.
- The multiplayer question (same stream on two MACHINES) is untouched; SIM-812's stable
  entity ordinal remains its prerequisite.
- The radio's nominal-duration path slightly changes *when* queued messages drain under
  lockstep versus normal play. That is confined to the diagnostic mode by construction.
