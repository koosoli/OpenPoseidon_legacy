# SINKHOLE-04 — W4: diving (the player goes under, holds his breath, comes up)

Branch: `feat/swim-dive`, on top of `feat/arma-original-ui`. Owner requests (2026-10-17): "make with blender for
the new ofp swimming animation also a diving animation"; the owner chose the underwater swim over a jump-in dive;
after the animations: "go ahead with the engine side so I can dive in game".

## Animations (made in Blender)

Four moves in the `@OP_Swim` pack, made by Claude on a Blender rig of the CWA `MC` skeleton
(`scripts/swim/blender_swim_rig.py`, `dive_moves.py`; see the pack README): `SwimDive` (a pike surface dive from
treading into the glide), `SwimUnder` (the underwater breaststroke), `SwimUnderIdle` (holding still) and
`SwimSurface` (glide to treading), on the action map `SwimUnderActions`. The first version was rejected by the
owner ("still looks shit"); the second fixed a 0.58 m pop at the end of the dive, a dive that rolled over instead
of piking, a rigid stroke with a third of the cycle dead, and every joint stopping on the same keys.

## Behaviour (SoldierOldSwim.cpp)

* **In:** a swimmer the player steers dives on **move down** (Z) where the water is at least 2.2 m deep, once he is
  settled (not in the fall's soft entry, `SwimIn` or `SwimSurface`). `SwimDive` plays and he sinks with it.
* **Under:** no heave and a third of the wave drift. The keys take him down (Z) and up (Q) at 0.9 m/s (reached in
  0.5 s), and he holds his depth with neither. He stays 0.45 m under (body middle) unless surfacing, and 0.25 m
  off the bed. The `SwimUnderActions` map plays the stroke forward (1.6 m a stroke, slower under a heavy load as
  on the surface) and the hover otherwise. The body pitches up to 20° with his climb or sink, turned about its
  middle (the flat moves lie 1.05 m over the model origin), and the offset eases back out after surfacing.
* **Out:** going up within 0.6 m of the surface: `SwimSurface`, and the surface heave takes him again, softly. A
  dead diver floats up face down like any dead swimmer. Shallow water stands him up as before.
* **Breath:** 40 s with the head under, a hint at 10 s ("Running out of air"), then 0.12 damage a second; at the
  surface it refills four times as fast.
* **Camera:** a camera on a diver may go under the sea, held only above the bed (`SwimmerDiving`, World.cpp); on
  a swimmer at the surface it is as before.
* **Keys:** while swimming, move up / move down no longer go to the stance actions (they did nothing in the
  water anyway).

## Not done

* AI swimmers do not dive.
* No look-direction steering under water (the keys only).
* The swim sounds (`946d6d6a6`: going under, gasps, the rebreather) are on another branch and not in this one.
* No breath indicator beyond the hint.
* Multiplayer: the dive state is not sent over the network.

## Verified (2026-10-17)

`tests/integration/ingame/units/swim_dive.test.sqf` (Trident, retail data, Everon bay via `swim-bay.Eden`), run
against this branch's own `dist\x64-win-rwdi` build with its staged pack (`--mods-dir <dist>\Mods --mod @OP_Swim`;
the game folder was not deployed to -- another agent owns it): **passed** (16.5 s). `POSEIDON_SWIM_LOG=1` trace:

| Step | Result |
| --- | --- |
| Z (move down) at the surface | `SwimDive` plays; sinks at 1.1 m/s for its 1.6 s, body middle 2.1-2.7 m under |
| Z held after the dive | keeps sinking at 0.9 m/s |
| no keys | `SwimUnderIdle`; the body's middle steady to the centimetre for 18 s while the swell (+-0.4 m) passes over |
| breath | 40 s counting down under water |
| Q (move up) | rises at 0.9 m/s (2.4 -> 0.7 m in 2.5 s), `SwimSurface`, then treading with the heave |

Not exercised in game yet: running out of air (the hint at 10 s, the drowning damage); the runs used ~6 s of it.

Screenshots: treading; the pike head-down with the camera following under water (the underwater compositor
takes over); the hover over the sea bed; back to treading.

The first runs failed for test reasons, not the dive: a 1.0 m threshold against a 0.98 m gain (the body origin
already rides 1.5 m under when treading), and a script without `triEndTest`. The `tri.exe` in the main checkout's
`target\` predates the `OpenPoseidon.exe` rename and cannot find the binary; build `tri` from the branch.
