# Persistent snow cuts from real terrain shots

The user requested visible holes from actual shots into snow. This bounded first
slice hooks the existing `ShotShell::Simulate` terminal terrain-hit branch, used
by ordinary rifle bullets. It executes after scenery/cargo/object hits and fused
bounces have already selected their original routes, and after the terrain/sea
ballistic terminus is reported. Existing damage, projectile deletion, ricochet,
penetration, water interaction, impact FX and network calls are unchanged.

Only a local, nonexplosive projectile with finite incoming speed/damage/position
can edit snow. The actual terrain hit must be within the map and above sea,
within 8 cm of current ground support, facing an upward normal and entering the
surface rather than grazing away. Existing local remaining snowfall or snowline
depth must exceed 8 mm. Object/wall/roof hits never call this branch. Five physical
footprint samples additionally reject actual roads, raised ground support and
overhead View geometry. The latter ignores only the current projectile and its
shooter, and casts 30 m upwards from just above ground; it does not depend on
camera maps or artistic AO. Very tall cover above that bounded ray remains a
limitation.

A global simulation-clock budget admits at most eight footprint checks per
quarter second (at most 40 support/overhead queries). Invalid position, absent
snow and unsuitable shot direction fail before those queries. Budget refusal
never alters the projectile outcome. Cosmetic marks can be omitted under a
large local barrage; no unbounded physics-query loop is added.

`SnowField.BulletImpact` adds a smooth radial bowl to actual stored deficits,
bounded to 35 cm radius and 16 cm removal per call, always capped by existing
snow base depth. Typical rifle damage produces approximately 22 cm radius and
8 cm removal, with a shallower edge. Uncut outer powder supplies the rim;
this does not heap new snow above the original surface or invent displaced
mass. Repeated shots can deepen a hole only to the existing base depth. The
same sparse chunks, snapshot, geometric displacement and normal path as tracks
carry the result. Camera movement never clears it; toggles preserve it; actual
new precipitation refills it. Existing tracks/rotor kernels retain their
original edge profile.

The collision shell remains the original terrain. This change does not stop a
round at the raised snow top or redirect ballistic impact position along the
incoming line. It creates the cut at the actual ground terminus; oblique shots
into deep snow may need a later physically integrated snow collision slice.
Snow lying on bare rocky terrain can be cut because the snow itself is soft;
hard object, road and snow-absent hits are excluded. This is not a mud-surface
whitelist.

## Evidence and required runtime

`scripts/Test-SnowBulletImpact.ps1` compiles the actual production policy and
complete `SnowField` under clang++ warnings-as-errors. Its only headless seam
is a minimal Landscape declaration; tests disable altitude cover and cannot
use that stub to license a terrain impact. Actual field tests pass for radial
centre/edge/outside shape, depth caps, repeated shots, settings toggle retention,
camera snapshot return, precipitation refill and reset. Policy tests falsify
missing snow, slow/extreme/nonfinite input, glancing/steep direction, support
mismatch, remote/explosive/sea/road/shelter conditions and the global budget.
Source contracts verify the edit stays after fused terrain resolution and
before existing impact FX. `git diff --check` passes.

The complete actual header exposed an existing UCRT deprecated `getenv` call.
Snowline override parsing now uses Windows `_dupenv_s`/free, with portable
`getenv` elsewhere, preserving override interpretation and warnings-as-errors.

Agent did not compile the full game, run GPU tests, launch or deploy. Root must
build the matched pair and fire actual ordinary rifle rounds onto exposed
deposited snow; capture the same fixed view before/after, paused and after camera
return. It must verify centre deficit increases in the existing snow sample,
no outside changes and refill after precipitation. Bare terrain, actual sea,
stock road, hard object/wall/roof and covered-ground firing are negative controls.

`POSEIDON_SNOW_BULLET_TRACE=1` logs `SNOW_BULLET terrain=1 changed=...` with
actual hit coordinates/speed, base/remaining depth, radius/depth, before/after
stored deficit, roadway/sheltered/land/budget booleans, normal-up/incidence and
support mismatch. Booleans use ordinary fmt `true`/`false`. No texture, shader
uniform, ABI or muzzle sprite is changed here. Visible muzzle work belongs to
the separate grass agent. Neither hole appearance nor multiplayer replication
is accepted by these CPU/source checks; surface edits remain mission-local.
