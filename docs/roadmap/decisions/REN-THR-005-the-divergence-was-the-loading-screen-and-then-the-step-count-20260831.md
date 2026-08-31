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

## What is left, and it is small and named

Under lockstep the world holds 12 frames but `verts` — the 2D path — parts after **1**. So
the 2D path now has a clock dependence that is *not* the simulation pacing, and it is the
only thing standing between here and a reproducible frame stream. That is REN-THR-004's
candidate 2 (the HUD dim ramps) or something beside it, and it is now testable in isolation
because everything else holds still: `POSEIDON_NO_HUD` at `World.cpp:1766` is the experiment.

Note the shape has inverted. Before alignment the 2D path appeared to *lead* the world by
one frame, and a great deal of thought went into explaining that. It was an artefact of
comparing runs that were at different points in their load. What is real is the opposite:
with the pacing fixed, the world is stable and the 2D path is not.

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
