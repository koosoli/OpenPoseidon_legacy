# TW-WATER W8–W12 — what changed after the W7 report

Tidewater Native (`WGR_WATER_BACKEND=1`) from the W7 parity report (`docs/tw-water-w7-report.md`)
to W12d. Each milestone is its own branch on the safety mirror (`origin`, `dec/tidewater-*`), each
on top of the one before; none is merged into trunk. Captures are in `.tw/shots-<label>` (the
harness: `.tw/test-float.ps1`, `.tw/test-play.ps1`, missions from `.tw/float/gen.py`).

```
Last milestone: dec/tidewater-w12d-mesh-waterline 3cbe1b52
Deploy stamp:   dec/tidewater-w12d-mesh-waterline 3cbe1b52 2026-09-28_20:36 deploy-ps1
Status:         each fix checked with harness captures on one machine; owner play-tests pending
                for W10–W12 (see "Open")
```

## Milestones

| Milestone | Branch | Commit | What |
| --- | --- | --- | --- |
| W8a | `dec/tidewater-w8a-…` | 1f295e49 | Shells, rockets and bombs hitting the water send the engine's water-interaction events with their ammo (`indirectHit`, `indirectHitRange`, `hit`) to the renderer. `WGR_TW_LEGACY_WATER_FX=1` keeps the legacy fireball / droplet sprites. |
| W8b | `dec/tidewater-w8b-…` | c8f1491d | The water plume (spray particles), ring wave, collapsing cavity and foam patch of a burst (`impacts.rs`, `tw_impact.wgsl`). Plume height from the cube root of `indirectHit x indirectHitRange` (~4 m for 20 mm, ~15 m for 120 mm HE, ~50 m for a bomb). Visual only. |
| W9 | `dec/tidewater-w9-water-reports` | f2bc233d | One waterline for the views above and below: a probe pass (`probe.rs`, `tw_probe.wgsl`) evaluates the drawn surface around the camera for the underwater composite and the engine's engage gate (the C++ gate no longer uses Current OP's reference waves in this mode). The sea to the horizon (16 CDLOD levels; the ground beyond the map goes deep over 400 m). Wind raises the waves (fetch with wind, whitecap growth bounded). `WGR_TW_WIND`. |
| W9d | `dec/tidewater-w9d-view-side` | eed5c731 | OP's world is left-handed: `front_facing` is the view from below. Fixed the flat blue sheet at the waterline; exact reversed-Z scene distance from far away. |
| W9e | `dec/tidewater-w9e-meniscus` | 794eb826 | A quiet waterline across the lens (the rim brightens what is there by up to 30 %). |
| W9f | `dec/tidewater-w9f-far-bed` | 118d8478 | Beyond ~2 km the sea bed seen through the water comes from the heightmap, not the depth buffer (the lines seen from 8–15 km up). |
| W9g | `dec/tidewater-w9g-no-ssr-high` | bc2b9915 | No screen-space reflections from high up (fading out 400–1200 m over the sea). |
| W10a | `dec/tidewater-w10a-lip-facing` | 5e4a9eea | The breakers' lips shaded from the side they are seen from. |
| W10b | `dec/tidewater-w10b-surf-glare` | 82b06f45 | No glare band and no bloom star along the breakers (lip and spray alpha clamped, the spray's forward-scattering lobes capped). |
| W10c | `dec/tidewater-w10c-lip-switch` | 936e199b | `WGR_TW_LIP` (lip opacity for A/B captures). |
| W10d | `dec/tidewater-w10d-backlit-surf` | a56a59a3 | Backlit surf foam and lips within OP's exposure (transmitted lobe x0.45, lip glow x0.5). |
| W10e | `dec/tidewater-w10e-spray-veil` | 7751729b | The breakers' spray as a veil, not a wall (dense spray / sheet / mist opacities 0.45 / 0.5 / 0.12). |
| W10f | `dec/tidewater-w10f-surf-texture` | ce00dd24 | The surf whitewater keeps its lace and lumps out to a 0.3 m footprint (was a flat white stripe from the beach); the swash-front foam is drawn as lace, not an opaque rope. |
| W10g | `dec/tidewater-w10g-far-triangles` | 178e7e61 | The triangle's facing decides the view side only within 25 m of the camera (pale wedges and tiny triangles over the distant sea from the beach); the spray lobe cap 0.9 → 0.35. |
| W11a | `dec/tidewater-w11a-wake-render-pose` | fd090c8f | The wake, bow spray and spray collision body follow the hull as it is drawn (render interpolation), not the latest 60 Hz tick (the water jittered against a fast hull: "boats are stuttering"). |
| W11b | `dec/tidewater-w11b-boat-planing` | 8fc4a4dd | **Physics (owner-approved).** Small boats' planing lift 20–26x (was 20–120x). |
| W11c | `dec/tidewater-w11c-boat-draft` | 01a0bd92 | **Physics.** Planing lift 9x at rest, 12x flat out: the hull sits ~0.25 m deeper on average. |
| W11d | `dec/tidewater-w11d-buoyancy-diag` | 0ec4f70c | `POSEIDON_BOAT_DIAG` also logs the buoyancy (contact points, submersion, gain, lift/weight). Logging only. |
| W11e | `dec/tidewater-w11e-boat-drag` | bccf4b40 | **Physics.** The water drag reads the depth the legacy lift would give. Top speed is still ~10–15 % lower than legacy at 24 s (PBR 14.7 vs 17.1 m/s; patrol boat 15.8 vs 17.2); see "Open". `POSEIDON_BOAT_LEGACY_PLANING=1` restores the legacy lift exactly. |
| W12a | `dec/tidewater-w12a-spray-after-clouds` | ad347ecd | The spray sprites (breakers, bow spray, water explosions) get their own pass after the cloud, god-ray and smoke composites (the clouds were painted over plumes standing against the sky). |
| W12b | `dec/tidewater-w12b-spray-ab` | d56f91b3 | `WGR_TW_SPRAY_EARLY` (the pre-W12a order, for A/B captures). |
| W12c | `dec/tidewater-w12c-no-drowning` | 28a74e96 | **Gameplay (owner request).** Soldiers no longer drown (the 1999 rule killed AI deeper than 1.2 m, 0.6 m prone; the player was already exempt). `POSEIDON_INFANTRY_DROWN=1` restores it. |
| W12d | `dec/tidewater-w12d-mesh-waterline` | 3cbe1b52 | The waterline on the lens follows the drawn mesh exactly: the probe grid is the finest CDLOD lattice and holds the displaced vertices (rgba32float); the composite takes the plane of the mesh triangle over the lens point. Removes the strip of unfogged sand across the screen when the camera cuts the water. |

## Switches added

| Switch | Where | Effect |
| --- | --- | --- |
| `WGR_TW_WIND=<m/s>` | Rust + C++ bridge | Tidewater's local wind for the session (0.5–40), over the weather's (W9c). |
| `WGR_TW_LIP=<opacity>` | Rust | Breaker lip opacity (0–2), over the Water tab (W10c). |
| `WGR_TW_SPRAY_EARLY=1` | Rust | Spray drawn inside the water pass, before the clouds (the pre-W12a order; W12b). |
| `WGR_TW_LEGACY_WATER_FX=1` | C++ (`Collisions.cpp`) | Keeps the legacy fireball / droplet sprites of hits on the water in Tidewater mode (W8a). |
| `POSEIDON_BOAT_LEGACY_PLANING=1` | C++ (`Ship.cpp`) | The 2001 planing lift (20–120x) and its drag, exactly (W11b–e). |
| `POSEIDON_BOAT_DIAG=1` | C++ (`Ship.cpp`) | Once a second per boat: position, forward speed, thrust (existing) and, since W11d, the buoyancy line (points in water, submersion, gain, lift/weight, height, sea level). |
| `POSEIDON_INFANTRY_DROWN=1` | C++ (`SoldierOldSim.cpp`) | The legacy drowning damage for AI soldiers (W12c). |

All are also in `engine/WgpuRenderer/docs/env-switches.md`.

## Harness additions

- `.tw/test-play.ps1`: `test-float.ps1` that also plays — holds an SDL scancode (`-HoldKey`,
  26 = W) through the harness `key` / `key_up` commands between `-HoldFrom` and `-HoldUntil`
  (mission seconds), with the SQF probe every `-ProbeStep` s. Used to walk the player into the sea
  (`twwalkin`, `twwalkin1p`).
- `.tw/test-describe.ps1`: dumps the harness's command list (`describe`) to `describe.json`.
- Missions (`.tw/float/gen.py`): `twcoastprobe` (west-coast surf), `twbeachboom` (explosions
  offshore seen from the sand), `twspraytop-boat{e,w}` (boats abeam), `twboatrest`, `twwalkin`,
  `twwalkin1p`, `twaiwade`, `twface`, `twswim`, `twswim-side`.

## Showcase mission

`dev-missions/tw-showcase.eden` (added to this branch at W12e): the owner's edit of the Tidewater
demo (LST further out, positions, weather), with Seventh – I'm Gonna Fly as the music and
`shells.sqs`: 120/125 mm HE rounds into the water in front of the camera every 2.5–4 s from 4 s
to 110 s, each at least 60 m from every boat, man and aircraft, 130 m from the LST, and on open
water. Harness run (`.tw/shots-showcase-v2`, W12d build): 27 rounds, no damage to anything, all
24 soldiers alive at 112 s. (The copy on the `codex/*` branches, a6defd61, is an older version.)

## Findings that are not water

- **Boats sit high.** The 2001 planing lift (`Ship.cpp`) holds the hull near the mean sea level at
  speed with any water backend. W11b–e lower it; the owner wants the boat physics reworked to
  match Tidewater later.
- **Infantry in the sea.** The player walks down the sea bed (to −14 m in `twwalkin`) and the
  camera code keeps the view at the surface (`CamEffects.cpp`, `SurfaceYAboveWater`); the float
  model only acts while falling. Left for the swimming work.
- **Pale faces.** Soldiers' faces render pale bluish-white on dry land too, with either water
  backend (`twface`): a character material issue.

## Open

- W11 boats: top speed 10–15 % lower than legacy above ~9 m/s; the owner parked the boat physics.
- The surf band at 20 m/s still clips to white (surface whitewater and spray; the scene's exposure).
- The boats' bow spray draws white dashes across the hulls (Tidewater mode).
- Documentation of W5b–W7 decisions stops at TW-WATER-12 (W5a); this report and the W7 report
  cover the rest.
