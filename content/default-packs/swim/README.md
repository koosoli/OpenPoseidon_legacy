# @OP_Swim — swim moves

The animations and move states that make soldiers swim. The engine side is
`engine/Poseidon/World/Entities/Infantry/SoldierOldSwim.cpp` (ticket SWIM-W3-001, decision record
`docs/roadmap/decisions/SINKHOLE-03-swimming-20261004.md`).

* In water too deep to stand (1.45 m of mean water level, sea or a registered water body), a soldier
  switches to these states and swims. He rides the waves, splashes with his hands and feet, and stands
  up again below 1.2 m. A launcher or a machine gun weighs him down: about 20% / 10% slower, lower in the
  water. Firing is off while swimming; he can fight again once he stands.
* Falling: after a quarter second in the air dropping faster than 3 m/s a soldier plays `Fall` -- upright,
  rifle held lowered in both hands -- and is kept upright unless a blast or a hit throws him. A hard landing
  crouches (`FallLand`); into deep water he plunges with his speed and comes up in `SwimIn` (the rifle from
  the hands to slung). Water hurts above ~14 m/s (a ~10 m drop), lightly at 25 m. A soldier put on the water
  (a mission placement within 1 m over deep water, getting out of a boat) just starts swimming.
* Without this pack, nothing changes: soldiers walk the sea bed and tumble rigidly when falling, as before.
* `--no-swim` disables the pack. `POSEIDON_SWIM=0` disables the engine side (swimming and falling).

## Files

| Path | What |
| --- | --- |
| `config.cpp` | `CfgPatches swim`; the `CfgMovesMC` action maps (`SwimActions`, `SwimForwardActions`, `SwimDeadActions`, `SwimOutActions`, `SwimUnderActions`, `FallActions`, `FallLandActions`) and 25 states |
| `rtm/*.rtm` | 24 animations for the CWA `MC` skeleton (`tread`, `swim`, `swimfast`, `swimb/l/r/lb/rb/lf/rf`, `swimturnl/r`, `swimstart`, `swimstop`, `swimout`, `swimdead`, `swimdeadf`, `fall`, `fallland`, `swimin`; diving: `swimdive`, `swimunder`, `swimunderidle`, `swimsurface`) |
| `mod.json` | Default content pack manifest (`defaultContentApi` 1) |

## The surface stroke

`swim`, `swimlf`, `swimrf`, `swimstart` and `swimstop` are a heads-up breaststroke (2026-10-17): glide, outsweep,
insweep with the breath, hands together under the chin and forward, heels drawn up, whip kick into the glide. They
are made in Blender by `scripts/swim/surface_moves.py` (on `dive_moves.py`'s tools); steps, phase counts and the
`config.cpp` timings are the old moves', so speeds are unchanged.

The stroke's **arms are motion capture** (owner, 2026-10-17, compared side by side with the keyed arms: "i prefer
mocap"; "only for the arms"): the cleanest breaststroke cycle of the CMU Graphics Lab Motion Capture Database,
subject 126 trial 4 (mocap.cs.cmu.edu -- "The motion capture data may be copied, modified, or redistributed without
permission"; with thanks to CMU). `scripts/swim/cmu_mocap.py` reads the ASF/AMC take, finds the cleanest cycle and
writes the arms as directions in the chest's own frame to `scripts/swim/mocap_breaststroke.json`; `surface_moves.py`
sets them on our breaststroke body every frame (the body, head and legs stay keyed), timed so the hands reach full
stretch as the whip kick lands. The capture is a prone swimmer on dry land; its right arm sweeps ~20 deg wider than
the left, and is kept as recorded.

The sprint (`SwimFastF`, forward with the fast/turbo key) is a heads-up front crawl, `swimfast` (2026-10-17, owner:
"use the crawl animation for swimming when the player uses the running option" -- the pack had none; the sprint was
the breaststroke played faster): the arms in turn, each recovering over the water with the elbow high, the body
rolling toward the recovering arm, a six-beat flutter kick; 1.1 s and 1.5 m a cycle (both arms), the sprint's old
timing. `surface_moves.py` sets the arms on every frame from a hand path (`CRAWL_ARM`) rather than blending
shoulder angles, and slings the rifle along the spine for this move only (`crawl_sling`): where the other moves
sling it, up behind the right shoulder, the recovering right arm swept through it.

## Treading

`tread` (2026-10-20, owner signed off side by side against the old one in Blender) is motion from **ActionForge**
(actionforge.app, clip `swim_idle_loop`): Quaternius' treading-water cycle from the free Universal Animation Library,
**CC0 1.0** (public domain, no attribution required; the GLB's own metadata says so). It is not game data and comes
from no other game. `scripts/swim/retarget_glb.py` puts it on the `MC` rig: each soldier bone along the clip's bone
(joint to next joint), the twist from the two rest poses, the pelvis' travel dropped and its height matched to the
old tread, every joint held to the rig's limits except the upper arms; the rifle stays slung where the swim moves
put it. 3.33 s a cycle (`config.cpp`), 81 phases, an exact loop; within the joint limits, no collisions.

The four moves that start or end on treading were baked again on it, so they still join it exactly (to float
rounding): `swimdive` and `swimsurface` (`dive_moves.py`), `swimstart` and `swimstop` (`surface_moves.py`). Run
those two scripts in a fresh Blender session with the pack's `tread.rtm` imported as `tread`: a second `make_all`
in the same session bakes `swimstart`'s end off the stroke's first pose by 0.43. The turns, the back and side
sculling, `swimin` and `swimdead` are still `make_swim_rtms.py`'s and blend into the new tread rather than join it.

## Diving (W4)

Swimming, the player dives with **move down (Z)**, a tap is enough, where the water is at least 2.2 m deep. Under
water **Z** held sinks, **Q** (move up) held rises, and a tap of **Q** takes him all the way up; forward swims the underwater stroke, otherwise he holds still at his depth. Going up
near the surface, he surfaces into treading. He has 40 s of air under water (a hint at 10 s left), then drowns
within 12 s; at the surface the air comes back. The camera follows him under. AI swimmers do not dive. Engine:
`SoldierOldSwim.cpp` (`SimDive`, `UpdateBreath`), decision record
`docs/roadmap/decisions/SINKHOLE-04-diving-20261017.md`.

The moves: `SwimDive` (surface dive from treading: pike at the hips, the legs up overhead, into the glide; 1.6 s),
`SwimUnder` (the underwater breaststroke: long pull to the thighs, glide, frog kick, the body surging on the pull;
2.2 s, 1.6 m a stroke), `SwimUnderIdle` (holding still: flat, sculling, an easy kick) and `SwimSurface` (up from
the glide into treading, 1.3 s). `SwimDive` starts on `tread`'s first pose and ends on `SwimUnder`'s; `SwimSurface`
starts on `SwimUnder`'s and ends on `tread`'s (exactly). The engine blends to `SwimDive` and `SwimSurface`
(`BlendToMove`); under water the `SwimUnderActions` map picks the stroke or the hover, and `Man::IsSwimMove` counts
it as swimming.

`scripts/Build-Swim.ps1` packs them as `Mods/@OP_Swim/AddOns/swim.pbo` (prefix `swim`), with the
missions `dev-missions/swim-bay.Eden`, `dev-missions/swim-squad.Eden` and `dev-missions/swim-underfire.Eden` (god mode in 5.5 m of water under rifle fire and grenades, for the underwater ballistics). `Build-DefaultContent.ps1`
calls it, and `Deploy.ps1` installs the result.

## Provenance: made by Claude

* `tread` is now ActionForge/Quaternius motion (CC0, see Treading above). The other procedural moves are
  authored by `scripts/swim/make_swim_rtms.py` (Claude for Dec,
  2026-10-04, owner: "look like real soldiers swimming with gear on"): forward kinematics on the CWA
  `MC` rest skeleton, each joint turned about its own pivot, in the technique of a loaded soldier --
  upright tread with a breaststroke/eggbeater kick and sculling hands, a heads-up combat breaststroke
  inclined ~55° (the gear will not let the body lie flat), upright back and side sculling with
  scissor kicks, and a killed swimmer going limp into a face-down float; the rifle stays slung on the
  chest bone.
  `fall` takes its arms and rifle grip from the stock relaxed stance (`DTA\Anim.pbo` `savarestanistat.rtm`).
  The generator reads Malprave's moves (below) only for what the engine is tuned to: each move's
  step, phase count and mean head position, and the slung rifle's offset.
* `swimout` (climbing out) is still Malprave's move with the repair below.
* Malprave's moves were also made by Claude, for Dec in the Malprave fork
  (`G:\Malprave\tools\swimanim\v3`, September 2026).
* All 19 animations are Claude-made: the owner holds that in writing (confirmed 2026-10-10) and has
  cleared them for the repository. They are original work of this project, released with it.
* The four diving moves were made by Claude in Blender (2026-10-17, owner: "make with blender ... also a
  diving animation"). They were keyed on a rig of the `MC` skeleton (`scripts/swim/blender_swim_rig.py`), whose
  joint pivots `fit_mc_joints.py` fits from the stock animations into `mc_joints.json`. The key poses are in
  `scripts/swim/dive_moves.py` (curves through them, the hands, feet and head trailing, baked to every frame), and its `export_all` writes the RTMs. The actions are ordinary Blender keyframes,
  so they can be reworked by hand and exported again. The rig's RTM import and export round-trips `tread`/`swim`
  to 1e-6. The soldier mesh used for previewing is the game's `mc vojakw2.p3d` and is not in this repo.

## Repair of Malprave's moves (2026-10-04)

Malprave's builds had these faults, measured against the stock CWA animations (`DTA\Anim.pbo`).
`scripts/swim/fix_swim_rtms.py` fixes them; it now applies to `swimout` only (the generator also uses
its leg repair for that move).

| Fault | Malprave's RTMs | Stock | Fix |
| --- | --- | --- | --- |
| Legs face backwards (the cause of "feet backwards") | thigh and shin turned 158–180° about their own length: the front of each faces the body's back (−0.4…−1.0) while the joints sit in the right places | front faces the body's front (+0.4…+1.0) | each thigh and shin is turned back about its own axis (joints unmoved) so its front faces where a hinge knee puts it: the thigh away from the folding shin, the shin back up the thigh; a near-straight leg uses the pelvis's front |
| Feet | built on the twisted shin, toes behind it | foot square to the shin, toes forward | upright: the stock standing foot; lying in the water: pointed, 20° in front of the shin line; blended by the spine's tilt (25°–55°) |
| Neck looks stretched | shoulder bones locked to the chest (0°), upper arm 102–151° | shoulder 30–55°, arm 38–98° | the shoulder takes 35% of the arm's swing (≤ 50°); the arm follows |

`config.cpp` also uses `head="headNo"`. The engine's look turn on top of the flat stroke bent the neck
back further than the skin allows.

Two earlier foot repairs (same day) worked in the shin's own frame -- mirroring the foot to its front,
then pointing it -- and both still looked backwards (owner, second and third reports), because that frame
was itself turned round: the legs' twist was found by comparing with the stock animations.
