# Newly merged Dec features and how to test them

This inventory compares the integration to `codex/streaming-residency` at
`31b73f95`. All 98 active Dec tips are included, but the older Tidewater
series was already in that baseline. Ocean FFT/surf, underwater rendering,
boat wakes/planing/wrecks and the later owner showcase are not new gains from
this merge. Existing weather, grass, muzzle and corpse work is preserved.

## Swimming animation availability and provenance

The branch contains swimming engine code, **no swim animation files or addon**.
Its author's decision record says the local 16 RTMs were retargeted from Arma 1
poses. Oliver subsequently confirmed Claude made them and authorised their
inclusion. We accept that instruction; the absent files and creation history
prevent independent verification of whether they were original or retargeted.
The earlier attribution should not be treated as a verified finding.

The search covered all 632 available Git refs, reachable object/path history,
hidden/ignored local source directories apart from build/cache directories,
and directory indexes of 147 installed/package PBOs. No CWA-ready
`swim.pbo` or matching 16-animation pack was found. The referenced G:/ and M:/
locations are not mounted here.

Original Arma 1 swim RTMs do exist in the local official licensed-data archive.
They use a different skeleton and cannot replace the missing CWA retargets.
`fix_swim_rtms.py` repairs an already-retargeted pack; it does not perform that
retarget. Nothing was installed as a substitute.

Keeping an addon local does not change its licence. The official
[APL](https://www.bohemia.net/en/licenses/arma-public-license) permits
noncommercial Arma adaptation and explicitly includes Cold War Assault.
That establishes a possible licensing route for covered material, not the
provenance or terms of the absent pack.

## Active AI and path changes

These source changes are active with normal launches. The imported plan's
statement that *all* behavior switches are CfgAIFork keys is inaccurate.
None of this is a replacement of the entire pathfinding system.

| Feature | Owner test |
| --- | --- |
| Gunfire hearing and approximate unseen-shooter knowledge | Put an enemy behind a squad with an obstruction between them; fire several shots and observe contact/orientation reactions. Hearing thresholds still apply. |
| Gunfire aimed at friendly units helps establish hostility | Have a previously unidentified shooter attack the squad; watch the response. |
| Wider, weaker peripheral vision within 150 m | Place an enemy to an AI soldier's side, around 90 degrees from facing; compare reaction with a frontal enemy. |
| Increased LOS refresh budget: 16 tests/32 calculations rather than 4/4 | Use a populated firefight and compare detection and performance. This is a budget change, not a guaranteed latency/FPS improvement. |
| Near-miss suppression | Fire bullets within 4 m of an enemy squad from over 10 m away; watch danger/prone reactions, recovery and spreading suppression. Own squad is excluded. |
| Suppressed AI accuracy reduction | Compare actual outgoing fire before/during suppression; spread can increase up to 3×. `POSEIDON_AI_SUPPRESSION_LOG=1` records suppression. |
| Orders act before queued radio speech finishes | Rapidly issue move/stance/formation/fire/target orders to your squad; verify action starts while previous radio lines are playing. |
| Living reporter selection, kill-score bonus and corrected 50 m flee-distance comparison | Exercise contact reporting, kills and fleeing in an existing mission; inspect diagnostics where necessary. |
| Dead crew cease being vehicle threats; dead crew/cargo do not prevent boarding | Kill all enemy crew, then approach and attempt to board the empty vehicle. |
| Up to 256 groups per side and wrapped group names | Requires an editor/script stress mission creating more than 63 groups; not visible in a normal small mission. |
| Squadmates choose distinct cover at least 5 m apart | Engage a stationary squad near several cover objects; inspect their selected positions. |
| Friendly splash protection for AI indirect fire | Put friendly soldiers near a target/selected cover; watch whether the AI refuses unsafe splash fire. |
| Convoy order uses distance to leader with hysteresis | Give three trucks a road route, change their relative positions and observe reordering without continual swapping. |
| Refreshed convoy follow points stay on roads | Use a long winding-road route rather than just the initial formation placement. |
| Tank lookahead and speed reduction at sharp bends | Route a tank through winding roads and compare cornering. |
| Runway-axis circuit/takeoff/landing alignment | Order an AI aircraft through takeoff and landing on a known runway. |
| Runway-elevation approach height and horizontal-distance phase transitions | Repeat at an elevated runway; observe approach and touchdown. These cases still need broad map acceptance. |
| Parachutist bullet collision and AI aiming | Shoot a descending parachutist, and have AI engage one. Check that a killed pilot remains on the ground after landing. |
| Underground building Paths destinations and follower positioning | Requires authored cave content with roadway/Paths LODs. Give orders down stairs and within the same cave object. |
| Terrain-hole route admission and containment | Requires cave content. Check exits through valid paths and rejection of walking sideways through earth. Routing between separate cave pieces remains unfinished. |
| Sea swimming routes, animation-limited turning and avoidance fixes | Requires compatible swim animations. Route AI across sea water; lake routing remains incomplete. |
| Swimming boat boarding and deep-water exits | Requires swim content. Have AI reach a boat hull, board, then disembark in deep water. |

## Optional experimental AI

The following mechanisms are compiled but **off by default**:

| Feature | Test after enabling |
| --- | --- |
| Gradual visual spotting | Move an enemy into view naturally and compare day/night detection. The 2 s build value is a tuning baseline, not a guaranteed delay. Hearing/manual spotting are separate. |
| Hold → flank → search after losing a known target | Use an AI-led combat/stealth squad with independent movement; establish real contact, then move the enemy behind cover. Player-led groups and protected waypoints are excluded. |
| Five-second squad grenade interval | Compare actual grenade release timestamps. The imported author record reports a suspected failure of this limiter. |
| Exposed body-part visibility and aiming | Put a soldier partly behind a wall/vehicle and compare enabled/disabled runs. |

A config-only optional test addon is provided under
`tests/perf/addons/dec_ai_experimental`. Prepare it from this integration checkout:

```powershell
.\scripts\Prepare-DecAiTestAddon.ps1
```

It writes a PBO to `build/dec-ai-test/@OP_DecAiExperimental/AddOns`, outside
the installed game. Add `--addon-root "<that absolute AddOns directory>"`
to the **installed OpenPoseidon.exe** launch arguments for an experimental run.
Omit that argument for stock settings. This addon is not part of default packs.

The addon enables global `CfgAIFork` keys `gradualSpotting=1`, `hunting=1`,
`grenadeInterval=5`, `exposedParts=1`. A mission's `description.ext` does
not enable these global settings. **Restart after changing them**, because
configuration is cached per process. `POSEIDON_AI_HUNT_LOG=1` logs phase
changes and reasons a unit cannot hunt.

This is a test option, not promotion to accepted behavior. Search completion,
grenade timing and broad mission compatibility retain open author tests.
The normal grenade interval default is 0 (no limiter), not 5.

## Tunnels, caves and flashlight

| Feature | Test/content requirement |
| --- | --- |
| Visible terrain openings | A cave model's memory LOD supplies `terrain_hole` or `terrain_hole1..8`; inspect entrance holes instead of solid terrain. |
| Covered underground footprints | Corresponding `terrain_hole_hidden*` selections admit below-ground interiors while retaining the terrain roof. |
| Underground roadway floors/stairs and ordinary movement/collision | Walk down and around a cave with correctly authored floor/wall geometry. |
| Hole-aware bullet/LOS/explosion handling | Shoot and engage through an opening, then compare with solid intervening earth. This does not make every penetration/physics path hole-aware. |
| Underground camera floor and decal placement | Use first/third-person gameplay and fire onto cave floors/walls. |
| Terrain colour/depth/reflection cuts and grass exclusion | Inspect the entrance above and below; terrain/grass should not bridge a visible opening. |
| Opening-controlled direct sunlight | At noon inspect a chamber roof away from its opening: it should not receive outdoor sun/grass shadows. |
| Sky/bounce/GI/reflection falloff into caves | Compare entrance, corridor and deep corners. |
| Underground exposure and despeckling | Walk between daylight and darkness; inspect adaptation and bright noise. Outdoor post-optics probes refuse underground packets. |
| Soldier flashlight prototype | Use the existing headlights/light action on a standard soldier at night. AI automatically use it underground; `POSEIDON_FLASHLIGHT_AI_NIGHT=1` additionally permits normal AI night use. |

**Stock OFP maps do not gain automatically generated tunnels.** The author's
`ugcave.pbo` + `UgCaveTest.Eden` Everon demonstration is not present here.
Without it or another compatible authored cave, there is no selectable local
tunnel test mission. A cave needs model geometry, roadway and Paths LODs in
addition to its hole selections.

Current source accepts nine open and nine hidden selection names, convex hulls
of 3–16 points. Rendering selects at most 64 edges from nearby footprints
within 400 m. Imported descriptions of eight selections/permanent 25 m limits
are older than the code.

Box3D heightfields and `ShotShell::TryPenetrate` still use the original solid
terrain. The opt-in corpse prototype refuses deep caves when solver/engine
floors disagree. Separate cave-piece AI routing and Reforger CPU clutter need
more work. Diagnostic follow-camera raw-height clamping is unsuitable for
cave acceptance: use actual gameplay camera views.

Flashlight hold animation, dependable hand attachment and beam polish remain
prototype work. `POSEIDON_FLASHLIGHT=0` disables the light.

## Swimming engine gains — content required

| Feature | Test once the compatible addon exists |
| --- | --- |
| Deep-water entry and shallow-water exit | Standard soldier on Everon: enter water deeper than 1.45 m, return to shallows below 1.2 m. Vehicle/ladder/deck/hole exclusions remain. |
| Treading, forward/fast motion and animation-limited turning | Swim, accelerate, stop and turn; inspect pose, neck and feet. |
| Five-point wave following, spring-damped bobbing, orbital drift and lean | Test in swell while treading and moving. |
| Smoothed eye-height support | Inspect the head relative to waves without the old fixed waterline assumption. |
| Local drawn-surface wave support | Nearby swimmers follow a 16 m readback grid; distant swimmers retain the predictor. Compare camera-near and far swimmers. |
| First/third-person water camera corrections | Check troughs and crests in both views for head/camera clipping. |
| Hand/foot splash spray from limb motion within 150 m | Requires Tidewater; check strokes, kicks and slaps. Current OP ignores this new splash kind. |
| Face-down floating of swimming casualties | Test a soldier already in the swimming state. |

`POSEIDON_SWIM_LOG=1` supplies move/eye/surface diagnostics.
A/B controls: `POSEIDON_SWIM=0`, `POSEIDON_SWIM_DRAWN=0`,
`POSEIDON_SWIM_SPLASH=0`. No diving, breath/drowning simulation, complete
lake routing or swimmer-to-swimmer collision was added.

## Texture/import gains

| Feature | Test requirement |
| --- | --- |
| Native PNG RGBA and preserved alpha | Model/material referencing PNG; check opaque, cutout and blended examples. Four synthetic fixture PNGs are committed, but no playable import demo is supplied. |
| Correct image-source path shared by PNG/TGA/BMP/JPG | Compare an actual addon using those files; stock PAA-only assets will not expose this change. |
| DDS masked uncompressed 8/16/24/32-bit layouts, channel order, luminance and alpha | Actual DDS material using those encodings. |
| DXT2/DXT4 blocks and DXT1 punch-through detection | Appropriate DDS examples; inspect alpha/cutout edges. Plain DDS DX10 BC4/5/7 remains unsupported. |

## Imported fixes

| Fix | Owner test |
| --- | --- |
| Retained objects suppressed on loading/editor/unload frames | Enter/leave editor preview and load/unload islands; watch for objects drawn over missing/black terrain. |
| Terrain-change object/forest conform/skew invalidation | Revisit floating objects, change terrain grid and reload maps. This fixes stale caches; it is not proof every placement error is solved. |
| Subdivision cache stamped with source heights and level | Switch/reload islands and terrain detail; old incompatible caches should rebuild. |
| Edge reseating avoids outside-map −100 m samples | Inspect objects along map-edge strips. |
| Missing/empty soldier pilot memory selection handled safely | Spawn the affected addon soldier. The author's FML Vorhees content is absent here. |
| Original-loader fallback for all-SP3X OFP MLOD models | Inspect old skinned addon soldiers for torn/spiked geometry. `POSEIDON_SP3X_ADAPTER=1` restores the adapter route for comparison. |
| Camo net depth-writing cutouts enabled by default | Look at a soldier behind netting: solid strands should occlude, holes should remain open. |
| Buildings do not enter cockpit rendering merely because inside LOD equals LOD0 | Inspect interiors/floors from inside and outside, including the author's affected building if available. |
| Mostly opaque blended sections write solid-texel depth before blending | Inspect stacked floors through nearby blended surfaces. `WGR_BLEND_SOLID_DEPTH=0` is the A/B control. |

These changes do not implement collapsing buildings or automatic default
ragdolls. The existing building-rotation/native-destruction issue is separate.

## Diagnostics and observability

Launch this development build with `--dev --diag "<output directory>"`.
`--diag-draw` enables the overlay; `--harness <port>` enables commands.
The overlay requires the dev overlay to render.

New capabilities include valid JSON event logs and summary; projectile paths,
impacts, hit/damage/death events; watched animation/movement and AI state;
stuck/spin detection; script/addon/sound diagnostics; frame/performance
statistics; deterministic diagnostic seed; unit labels, trails, planned AI
paths and damage/impact markers.

The harness adds:
`diag_camera`, `diag_inspect`, `diag_groups`, `diag_near`,
`diag_geometry`, `diag_perf`, `diag_pause`, `diag_resume`,
`diag_step`, `diag_watch`, `diag_mark`, `diag_draw`,
`diag_stuck`, `diag_anim`, `diag_pose`, `diag_kill`,
`diag_movecheck`, `diag_scripts`.
Watch a named unit, shoot/move it and inspect its recorded state/path; pause,
step and resume for reproducible comparisons.

The bounded installed runner is `scripts/Test-DecDiagnostics.ps1`, invoked
through `scripts/with-game-lock.sh` with the installed commit. It checks
mission readiness, unit/group inspection, marker/event JSON, frozen clock,
five fixed ticks, screenshot and normal exit. Optional `-ExperimentalAi`
checks the test addon mount and actual hunting/interval configuration receipt;
it does not validate a firefight.

Diagnostic X-ray wireframes, render/model/skeleton/view-distance sweeps and a
general config-check command were not ported. Linux crash notification remains
absent.

## STALKER/X-Ray references and remaining roadmap

The source/documentation describes Malprave ports using selected OpenXRay
behavior ideas; it is not a full STALKER engine or asset import.
The referenced Malprave patch set is absent, so this audit cannot prove that
every earlier source fragment was independently authored. No direct import
of the X-Ray engine tree was found. Credits now explicitly identify the AI
and swimming ports rather than omitting them.

The current [OpenXRay licence](https://github.com/OpenXRay/xray-16/blob/dev/License.txt)
covers OpenXRay contributors' additions under MIT while distinguishing the
original GSC engine and its terms. It is not blanket permission to copy all
original GSC code. Naming STALKER as inspiration alone does not establish
infringement or clear the full upstream chain. Confirming that chain with the
author is still appropriate before claiming public-distribution clearance.

These remain **roadmap items, not newly implemented features**: enemy-switch
inertia/scoring, grenade avoidance, typed danger lists, squad enemy allocation,
peek/cover eviction, precomputed cover scoring, smooth view-axis range and
adaptive hearing/visibility HUD.
