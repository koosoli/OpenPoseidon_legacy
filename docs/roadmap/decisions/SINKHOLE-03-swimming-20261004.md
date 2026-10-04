# SINKHOLE-03 — W3: swimming (players and AI, riding the waves)

Branch: `dec/sinkhole-w3-swimming`, on top of `dec/sinkhole-w1b-cave-lighting`.
Ledger: `SWIM-W3-001`. Owner requests (2026-10-04): "move to swimming"; "since Tidewater has
large waves, the swimming AI and players need to move with them like real life; splash effects are
missing from hands, legs etc"; "bring across the animations"; "W3 had most fixed in Malprave".

## Source

Malprave's swimming (read only, `G:\Malprave` patches `swimming`, `swimBoatsAI`, `swimMoves`,
`swimTurn`, `swimRoutes`, `swimAvoid`, `boatExits`; `swimPaths` is a backed-out record and was
skipped). The core was ported by hand into `SoldierOldSwim.cpp` (Oli's water is different); the AI,
route and boat patches applied nearly as they are (744e1d3e).

## Animations: local content, not committed

`G:\DEC-CWA\AddOns\swim.pbo` (16 RTMs) plus Malprave's `CfgMovesMC` swim block, packed as a
`swim` addon into `G:\CWA-OLI\AddOns\swim.pbo`. The RTMs were retargeted by IK from Arma 1 key
poses, so their provenance rules out committing them (option A). A config without the swim
states keeps the old seabed walk.

Two faults in the RTMs, measured against the stock CWA animations and repaired by
`scripts/swim/fix_swim_rtms.py` (committed; it rebuilds the local pbo):

| Fault | Swim RTMs | Stock | Repair |
| --- | --- | --- | --- |
| Feet bent back (owner: "feet are backwards") | foot behind the shin in the shin's frame, 89–136° against it | 0–50° | mirrored in front of the shin, twist-free swing (≤ 60°) |
| Neck looks stretched (owner) | shoulder locked to the chest (0°), upper arm 102–151° | shoulder 30–55°, arm 38–98° | shoulder takes 35% of the arm's swing (≤ 50°), arm follows |

The local pbo also sets `head="headNo"`: the look turn on top of the flat stroke bent the neck back.
Note for Malprave's maintainers: `M:\ForestClaude\malprave-swim-animation-fixes.md`.

## Behaviour

* **In and out:** swims where the mean water level (sea, or a registered water body) is ≥ 1.45 m
  above the bed and the man is in it; stands up below 1.2 m. Not in vehicles, on ladders or in
  terrain holes. Dead men float face down.
* **Riding the waves:** a spring-damper (1.2 s period) holds the water line on the surface,
  averaged over five samples across the body; the waves' orbital velocity carries the swimmer
  (85%); the body leans with the wave face (70%, 0.35 s). A living swimmer's water line follows his
  eyes (`zamerny`, smoothed), which ride 0.12 m over the water whatever the move.
  (Malprave's fixed 1.42 m line put the eyes 0.2 m under while treading.)
* **The surface:** the CPU wave predictor (`QueryWaterSurfaceScaled`, the boats' sea). It matches
  the Tidewater sea in scale, not crest for crest: up to ±1.1 m apart in play, enough to put the
  player's eyes under drawn crests. Near the camera, swimmers ride the surface as drawn instead.
  The W9a probe grid is now read back whole: 16 m square, the drawn mesh's displaced vertices,
  1–3 frames late. It is handed to the simulation (`wgr_water_drawn_grid` → `SetDrawnWaterGrid` /
  `DrawnWaterHeightAt`). This is presentation for the local view only; the Current OP water mode
  and everything outside the grid keep the predictor.
* **Camera:** the player's view was held above the flat mean sea. In a trough that lifted the
  first-person camera into the swimmer's head (owner: "head is transparent when the camera bobs
  under water"). On a swimmer there is no water floor in first person, and in third person the
  floor is the wave surface (`SwimView.hpp`, `World.cpp`).
* **Splashes:** finger and toe tips (bone matrices on the CWA rest skeleton) throw a splash where
  they slap in, pull out or sweep along or just under the surface. This applies to every swimmer
  within 150 m of the camera. A new water-interaction kind (6) carries the wave height and the
  limb's throw. The Tidewater impact spray draws drops, a few strands, and white water only from a
  hard slap. The Current OP water ignores the kind.
* **AI:** turn requests are held to the swim moves' turn speed, and the move is picked by speed
  while swimming. Routes go through water, for soldiers that can swim, and float at the surface.
  Swimmers board a boat where they touch the hull and leave it over the side into deep water. In
  avoidance, swimmers skip free soldiers.

## Captured (Everon bay, `.tw/swim`)

| Run | Result |
| --- | --- |
| Treading, stroke (W held) | eyes 0.06–0.18 m over the water; kicks and strokes splash |
| First person in the swell, after the drawn-surface fix | camera 0.45–0.5 m over the drawn water (was under it in half the frames) |
| Neck A/B | headDefault: head thrown back, neck long; repaired RTMs + headNo: normal |
| Malprave swimai2 steps (`SwimAI.Eden`) | three AI swim 70–140 m out; a crew of three swims to a boat and boards (99 s); a1 swims 130 m on; b3 goes over the side and swims |

## Open

* No diving, breath or drowning (W4). Weapons are disabled in the water.
* Swimmers pass through each other. `swimAvoid` only stops the spinning.
* AI planning uses the coarse geography water classes (sea-based); lakes are not routed.
* Swimmers outside the camera's probe grid ride the predictor, so AI heads can dip under drawn
  crests at a distance.
* Splashes are Tidewater only. The legacy Current OP ripples for swimmers are not emitted.
* The `ValidateOutVehicle` assertion seen when boarding is pre-existing.

## Integration-owner provenance correction, 2026-10-04

Oliver initially reported original Claude authorship during integration. He now
reports that the swimming animations are based on Arma 1 after all, consistent
with the original local-content record above. The current integration no longer
labels that absent pack as original generated animations. This is corrected
owner-reported provenance; the pack is unavailable for independent examination.

A fresh integration-tip check found no tracked swimming RTMs or swim addon. The
RTM/swim-PBO pathname history across all available refs contains only the two
existing `tests/fixtures/rtm/{actor_motion,marker_motion}.rtm` parser fixtures,
not a swim animation pack. Tracked PBO names are test fixtures. The repair script
contains transformation algorithms and reads animation phases and skeleton
joints from external files; it embeds no swimming keyframe payload. The earlier
broader local/PBO-index search remains recorded in the feature test guide.
No swim animation assets were committed, copied into the installed game or
substituted from the local original Arma 1 archive during integration. This
finding concerns the swimming assets; it is not a licence clearance of every
file in the repository. The owner's running fog/ragdoll smoke session is left
untouched; this update changes documentation only.
