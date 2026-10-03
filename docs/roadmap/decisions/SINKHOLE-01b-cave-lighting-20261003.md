# SINKHOLE-01b — W1b: cave lighting, and a handheld flashlight for soldiers

Branch: `dec/sinkhole-w1b-cave-lighting`, on top of `dec/sinkhole-w1-terrain-holes`.
Ledger: `LGT-CAVE-001`. Approved by the owner on 2026-10-03 as a milestone **before** W2
("the cave lighting system pretty much doesn't exist and will need to be working well before the
forest caves even work properly").

## What The Forest does (for reference, not ported)

Unity 5.6 deferred, HDR, linear colour, no lightmaps; uSky ambient; an in-cave state switch at
the entrances; `CaveLight` objects; `FlashLightControler` / `LighterControler`. DayZ does the same
job with authored `cfgundergroundtriggers.json` (Outer / Transitional / Inner triggers, eye
accommodation 0..1). Oli has neither; W1b derives the same from the hole geometry instead of
authoring it per cave.

## The rule

Light enters a cave only through its openings (the terrain's OPEN holes, the same edges the terrain
pass discards). Per fragment (`conform.wgsl` `cave_daylight2`, group(4) binding 3, a uniform
because the GPU-driven path has no storage slot left):

* **direct sun** only inside an opening's footprint (0.3 m soft edge), fading over the first 4–12 m
  of depth -- a roof a few metres from a stairwell is under rock and must not catch the sun or the
  grass shadows above it;
* **sky / bounce ambient** fades with the reach from the nearest opening:
  `1 - smoothstep(1, 9, nearest + 0.35 * depth)`;
* nothing changes above the terrain or in the 0.05–0.25 m contact band.

`shading.wgsl` takes the sun from `.x` and the ambient (`mix(1, 0.03, ...)`) from `.y`; GI and the
sky-reflection term are scaled the same way, in `shader3d` and `gpu_driven` alike.

Per camera (c4302856): `EngineWgpu::SetTerrainHoles` evaluates the ambient rule once at the camera
and queues it (REN-THR-013) to `wgr_set_camera_underground` (0 open air .. 1 deep). Sun shafts and
the surface-scattering term scale by `1 - underground`; the exposure ceiling rises toward
`WGR_CAVE_EXPOSURE_MAX` (default 6) underground -- the shipped profile's small ceiling kept a
campfire or a torch from ever reading -- and is untouched above ground.

## The flashlight (test item)

`SoldierOldFlashlight.cpp`: a spot (`LightReflectorOnVehicle`, 20 m, 0.45 rad, warm white,
daylight-visible) on the left-hand proxy bone (`lruka`), aimed along the eyes. The player toggles it
with the headlights action / "LIGHT ON"; AI switch it on underground (more than 1 m under the
terrain inside a hole), and at night only with `POSEIDON_FLASHLIGHT_AI_NIGHT=1` so legacy night
missions are unchanged. `POSEIDON_FLASHLIGHT=0` disables it. It takes a local shadow slot through
the existing LGT-010/015 selection (sorted near/bright first; the holder is inside the 1.2 m
self-radius and does not shadow his own torch). No hold pose yet: the light rides the left hand of
whatever animation plays.

## Captured (ugcave on Everon, `.tw/holes/c1`, `c3`)

| What | Result |
| --- | --- |
| Roof near an opening (owner round 1: "the roof of the cave is bleeding light") | dark; direct sun only in the footprint (17c38f76) |
| Soldiers underground (owner round 2: "soldiers also illuminated") | dark unless a light reaches them |
| AI flashlights underground; player "LIGHT ON" | on; beam on the pillars, rock behind them dark |
| Chamber with campfire + torch, A/B `WGR_CAVE_EXPOSURE_MAX=1.1` | near-black and soldiers invisible -> dark but readable |
| Surface view, same A/B | identical |
| Night, AI torches in the chamber, surface above | no light on the ground above (weak test: beams were horizontal) |

## Owner play-test round 1 (2026-10-03, evening)

* **Crash on loading UgCaveTest** from the owner's missions folder: FIX-COCKPIT-001's
  `_drawInsideVehicle` was an `OLink`; constructing it faulted in first-person play (every harness
  run had used a scripted camera). Now a plain compared-only pointer (28226fb9). A first-person run
  is part of the cave check from now on.
* **Soldiers glowing white**: the torch had an ambient term (3% of its colour); at 0.1 m from the
  bulb that saturated the holder. Ambient is now black (54c74bff).
* **"Green flashing dots in the corners"**: pinhole cracks in the test model showing the sky,
  multiplied by the underground exposure. Underground despeckle in the tonemap (2cb65ab4): 27 -> 1
  dots over ten first-person frames. `WGR_CAVE_DESPECKLE=0` turns it off. Dots the owner saw on the
  entrance model ABOVE ground are not covered (the despeckle is underground-only).

## Open

* **Parked by the owner (2026-10-04, "sort the flashlight later")**: torch brightness and aim in play
  (6b8ac86d now aims the player's beam at the point 12 m down his view; in the harness the player's
  own torch lit nothing visible in the cave while AI torches did -- unexplained); the left-hand proxy
  is never found (`hand=0` for every soldier, torch at the chest fallback); the hold pose.

* The stairwell seen from its foot reads very bright -- probably the sunlit opening and correct, but
  surfaces inside an opening's footprint and under a slab do get full sun/sky from the rule. Owner to
  judge in play.
* Local lights are not occluded by the terrain heightfield itself (only by objects in their shadow
  views); not observed to leak, not proven not to.
* Off-hand hold pose for the torch (Malprave's swim-animation retarget tool is the planned route).
* No fog/aerial-perspective change underground; nothing looked wrong in the captures.
