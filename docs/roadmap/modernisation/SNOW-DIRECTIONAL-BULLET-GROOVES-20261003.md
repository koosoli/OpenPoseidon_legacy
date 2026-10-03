# Directional snow bullet entry grooves

The actual terminal terrain-impact path previously passed only a radius and depth
to `SnowField::BulletImpact`. Every angle of fire therefore produced a round bowl.
The candidate now uses the actual incoming projectile direction and local terrain
normal to estimate the path through the remaining snow above that terrain hit.
The terrain hit is the downstream endpoint; the snow entry lies upstream, along
the firing direction. A vertical shot retains its round bowl.

The horizontal entry length is `remainingSnow * horizontalSpeedFraction *
normalUp / -incidence`, capped at 0.8m to bound both local-plane extrapolation and
cell work. The existing 20–35cm radius, maximum16cm cut depth, and actual remaining
snow bounds remain. The production field carves a capsule with rounded powder
edges and keeps deficits nonnegative. Repeated hits remain bounded by existing
snow; precipitation fills the same stored deficits. The maximum continuous
capsule volume before edge taper is below0.152m³, with the representative rifle
CPU fixture removing below0.03m³. This is a deliberately resolvable125mm field
effect, not a calibrated millimetre-scale ballistic snow model.

The existing eight-impact-per-quarter-simulation-second budget is unchanged.
Each admitted candidate still performs five bounded terrain/support/roof probes:
the capsule centre and four corners of its enclosing oriented rectangle. This
includes the upstream extension. These are finite sampling guards, not proof
that an arbitrarily small obstacle between probe positions cannot be missed.
Actual object, fuse, water, roadway, sheltered and invalid terrain admissions are
unchanged. The change does not create trails along a bullet's free-air flight.

## Verification

On2026-10-03, `Test-SnowBulletImpact.ps1` passed with LLVM clang++ using the actual
production `SnowField` and policy headers. It checks vertical and oblique shapes,
directional rotation/reflection, untouched forward/side/outside cells, bounded
removed volume, invalid values, persistence, camera-window return and snowfall
refill. The actual projectile source-contract checks also passed.

`Test-SnowBulletRuntime.ps1 -SelfTest` passed. The installed-runner now requires
actual terminal direction and groove length, compares sampled changed cells
against the upstream capsule, rejects an oblique round-only footprint, and retains
the original actual-shot/ammunition/Fired, roof, pause, camera-return and refill
requirements. Its before/after inventory is bounded at625cells, enough for the
maximum capsule plus permitted calibration drift.

Full combined build, matching installed binaries, actual ordinary-player shots
and visual review remain pending with the root deployment owner. No GPU or game
run was performed by this isolated source agent.
