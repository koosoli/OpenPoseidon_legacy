# Owner fog and airborne-ragdoll revision

Matching installed engine/renderer source `1418e425`, deployed 08:49 local on
4 October 2026. Game and Tests paired build passed (82 steps). Current native
physics suite passed 64 cases / 13,864 assertions. Curated receipt:
`fog-ragdoll-owner-revision-20261004-runtime.json`. Raw logs, images and retail
pose arrays remain local in ignored build directories.

## Weather controls and default

Owner approved the new volumetric fog as the default. **Ctrl+F8 -> Weather -> Fog**
offers Legacy/Volumetric rendering, fog amount and transition time, Low ground
fog / Two layers / Elevated band presets, and advanced two-layer base/top ASL,
feather, extinction, scattering albedo and forward scattering controls. Weather
order is Wind, Fog, Rain, Snow, cloud cover/forecast, wind diagnostics.

Startup overrides seed C++ shared settings once instead of repinning the Rust
consumer every frame. Live edits then remain authoritative. Fog amount zero
adds no layered transport. Layers stay at fixed sea-level altitude; ground
presets use the ground under the camera once, not every subsequent frame.

## Tree comparison

The old receiver rain-reach gate blocked fog on a tree's covered trunk and parts
of its own cards. Genuine plant sections now consume the already integrated
air in front of them consistently; non-plant roof/interior gates remain.
Seven current fog GPU tests, production shader compositions and direct/retained
plant-callsite guards passed before root integration.

Installed `661cd6d5` original Everon campaign
`original-everon-tree-fog-owner-fix-20261004-084042` passed all four normally
exiting arms and twelve settled ground/between/above views: OFF/ON at actual
fog .45, ON/OFF at zero, fixed exposure .6, unchanged native camera/range/source
and EXE/DLL pair. Root inspected ground, between and above images. Crown/trunk
haze is more continuous. A nearby gray card-shaped band remains visible;
material coverage or canopy-volume occlusion may contribute. This is a partial
tree artefact correction, not full visual acceptance. Legacy remains available.

## Airborne corpse recovery

The old 12-metre travel guard froze the last pose while a blast was still carrying
the soldier through the air. Distance is now allowed to grow. Automatic, timed,
manual and save retirement require actual upward static-solid Box3D contact,
agreement with the current native surface within 25 mm, and low body motion.
Wall-side contact is rejected. Invalid motion/pose/bounds use authored falling
with the last finite velocity rather than retaining an unsupported frozen pose.
Saving an unsupported active body preserves ordinary falling motion without
writing a frozen snapshot. Box3D remains on the existing pinned dependency.

Installed `1418e425` campaign `owner-airborne-ground-recovery-20261004-084948`
passed the four-owner standing/frozen/persistence/budget/cleanup baseline,
ordinary frozen-body save/load, a controlled inherited 8 m/s horizontal and
10 m/s upward trajectory, and an actual live GrenadeHand death. The controlled
body crossed the old 12 m bound while airborne, refused explicit freeze,
survived an airborne save with exact active pose and unchanged solver handles,
then settled on native ground. The grenade arm used actual damage/death and
also returned to ground. Final corpse images were inspected. Active airborne
save **load** is not exercised; this receipt checks saving and continued live
falling plus the separate frozen save/load regression.

Automatic ragdolls remain an experimental opt-in with existing stock-model,
dry-ground, single-player and capacity limits. Crouch/prone, broad maps/actors,
water and excavation contact are not certified by these tests.

## Corrected interactive handoff

The previous demo removed all weapons and added a loaded M16 without selecting
it. Actual SDL click proof showed [Fired count, selected ammo, M16 ammo] of
[0,-1,30] before selection and [1,29,29] after selecting M16. The new
`scripts/Show-WeatherRagdollSmoke.ps1` preserves stock equipment, selects M16,
uses ordinary infantry/internal-camera controls and requires a real left-click
Fired event plus exactly one consumed round before leaving six live targets.

Two default preflights refused launch because current PowerShell retained null
environment assignments as empty strings. An empty fog/profile override disables
the candidate. The runner now removes those environment keys entirely and records
the exact weather observation; it does not force the fog enable switch. Failed
preflight games were root-owned and cleaned up. Final handoff receipt is retained
under `build/owner-ragdoll-fog/`. Corrected final handoff
`20261004-085633` PASS: CPU settings report volumetric true with absent overrides,
actual fog .25, ordinary Infantry controls, camera effect zero, and actual SDL
left click produced one Fired event and M16 ammunition 30 -> 29. Six live targets
remain; PID 22024 is left open for owner testing. This establishes default/config
and input readiness, not owner judgement or per-frame GPU-setting identity.

This is the finite owner-requested correction, not a restart of the full roadmap.
