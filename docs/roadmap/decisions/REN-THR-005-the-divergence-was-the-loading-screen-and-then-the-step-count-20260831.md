# REN-THR-005 — Every earlier measurement was of the loading screen; the real cause is the step count

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Supersedes the experimental sections of:** REN-THR-002, REN-THR-003

Phase 5 step 6 needs proof that a restructured producer publishes the same frames. Three
documents chased why two runs of one scene diverge. This one closes it, and most of what it
closes is my own measurement error.

## The error, and how it was caught

REN-THR-004 recommended logging the slice **lengths** next to the hashes. Done, together
with the fixed-step count. The first three rows of the parting frame answered the question
immediately, and not in anyone's favour:

```
idx 30:  run1 steps=0 vert=6   cmd=1   d3d=0     <- and runs 2, 3 identical
idx 31:  run1 steps=0 vert=0   cmd=0   d3d=0     <- runs 2, 3 still vert=6
idx 32:  run1 steps=8 vert=132 cmd=219 d3d=210   <- runs 2, 3 still vert=6
```

`vert=6, d3d=0, steps=0` is the **loading screen**. Every frame in the window was still in
the mission load, and what looked like divergence was one run finishing loading before the
others. The world starts at absolute frames **432, 452 and 455** in three runs of the same
build — a spread of 23 frames.

**So every number in REN-THR-002 and REN-THR-003's experiments measured how long the loading
screen lasted.** The 24-versus-19 that "refuted" lockstep pacing; the 51→59 that "confirmed"
the LOD governor; the 46 that made lockstep look worse a second time; the total absence of
shared hashes at frame 800. All of it. The window was moved to frame 400 precisely *because*
the first attempt was measuring the load — and 400 was still inside it.

## Aligned on the world starting, the picture is different and much better

Take the first frame with `d3d > 0` in each run as the origin and compare from there.

| | wall-clock paced | `POSEIDON_LOCKSTEP_HZ=60` |
|---|---:|---:|
| comparable frames | 41 | 40 |
| frames where all three runs took the **same step count** | 25 / 41 | **40 / 40** |
| **fully identical published frames** | 2 / 41 | **11 / 40** |
| camera and `draws3d` hold identical for | **2 frames** | **12 frames** |
| `terrain_nodes` ever differing | **never** | **never** |
| `grass_batches` ever differing | **never** | **never** |

Two things fall out.

**The cause is the step count, exactly as REN-THR-002 first supposed.** At world+0 and
world+1 all three runs take 8 fixed steps and publish **bit-identical** frames — same
`draws3d`, same camera, same 2D. At world+2 the counts are 2, 1 and 1, and that is the frame
the hashes part, with the **camera** parting first. The camera is a direct function of how
many ticks ran. Nothing exotic was needed: not a readback, not the HUD, not the LOD governor.

**Lockstep does what it was built to do.** The step count becomes constant in every frame of
every run (40/40), identical frames rise from 2 to 11, and the world holds for six times as
long. The claim in REN-THR-002 that it was *measured not to help* is now doubly withdrawn:
first for comparing incomparable amounts of world time, and now for comparing loading
screens.

**Terrain and grass never diverge in either arm**, across 41 and 40 frames. Whatever remains
is not in the terrain or the grass placement.

## Correction to the table above: count the disagreeing frames, not the first one

"First parting" is the wrong statistic and it flattered the result. `verts` appeared to part
at world+1 under lockstep, but that is a **single-frame blip** — run 2 differs at world+1 and
all three agree again at world+2 — and treating it as the parting point produced the reading
that the 2D path was now the sole remaining problem. It is not. Counting the frames in which
the three runs disagree, per slice:

| slice | wall-clock (41 frames) | lockstep (40 frames) |
|---|---:|---:|
| `cameras`, `draws3d`, `palette`, `shadow_casters` | 39 | **28** |
| `verts` | 39 | 29 |
| `water_nodes` | 39 | **0** |
| `clear`/`fog`, `batches`, `cmds`, `terrain_nodes`, `terrain_batches`, `water_batches`, `grass_batches` | 0 | 0 |

So lockstep **removes the water-node divergence outright** (39 → 0) and takes everything
position-dependent from 95% of frames disagreeing to 70%. That is a large improvement and it
is not a solution: 28 of 40 frames still differ.

What never differs in either arm is as informative: the terrain nodes and batches, the grass
batches, the 2D batch and command structure, and the clear and fog scalars. Everything that
does differ is downstream of **entity positions**. So the residue is inside the simulation
rather than in the renderer's assembly of the frame — with an identical step count and an
identical starting state, an identical simulation should produce identical positions, and it
does not.

SIM-813 removed the one wall-clock budget in the AI stage earlier the same day and that fix
is in this build, so it is not that. Finding what else inside a step is a function of real
time, or of thread scheduling, is the next question, and `StateTimeline` (SIM-808) is the
instrument for it rather than this one — it reports a first divergence by tick, record and
field, which is one level below what a frame hash can see.

## What is left, and it is small and named

Per the correction above, the residue is **in the simulation, not the 2D path** — `verts`
disagrees in 29 frames where `draws3d` disagrees in 28, which is the same divergence reaching
both, not a separate 2D clock. REN-THR-004's HUD-ramp candidate is also ruled out
arithmetically: it reads `Glob.uiTime`, which accumulates `noAccDeltaT = deltaT`
(`World.cpp:174`, `:399`), and lockstep pins `deltaT`, so `uiTime` advances by a constant in
this arm and cannot be what varies.

The "2D path leads the world by one frame" that REN-THR-003 reported, and that REN-THR-004
was commissioned to explain, was an artefact of comparing runs at different points in their
load. There is no lead. Aligned and counted, `verts` and `draws3d` disagree in 29 and 28
frames of the same 40 — one divergence reaching both slices, arriving in the same frames.
An agent spent a full audit on a phenomenon that a correctly placed window would have shown
did not exist, which is the cost of a measurement stated more confidently than it was taken.

## What this does not say

- **One scene, three runs per arm, one machine.** The step-count mechanism is structural and
  will hold anywhere; the specific frame counts will not.
- **It does not make the frame stream reproducible.** 11 of 40 identical is not 40 of 40,
  and step 6 still cannot take its proof. It does say what the remaining work is.
- **The LOD governor is un-confirmed, not disproven.** Its evidence was the 51→59 shift,
  which was load duration. Whether `_lodInvWidth` reaches the published frame is once again
  an open question, and REN-THR-003's static argument for it stands on its own.
- **The 23-frame spread in load duration is itself unexplained** and is not obviously a
  defect — it is I/O — but it is why every window must now be placed by the `d3d > 0` marker
  and never by a frame number.
