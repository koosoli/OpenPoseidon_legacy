# Spherical addresses and bounded Earth free-flight rebasing

This slice replaces the equirectangular terrain lookup with the exact inverse
of the existing spherical east/up/north projection. `FromLocal` and `ToLocal`
now identify the same point as `GlobeNormal` and the curved terrain datum.
Overview imagery unwraps longitude relative to the geographic origin; prepared
surface overlap uses a conservative spherical bound. Source heights, collision
triangles and native OFP coordinates are unchanged.

Only the manual Earth camera with altitude speed scaling rebases. Its lateral
deadband is 32 km near the ground, grows to altitude / 4, and caps at 400 km,
below the existing coordinate safety limit. On crossing it, the camera point
`(x - 6400, EarthRadius + y, z - 6400)` supplies a new spherical footpoint and
radial altitude. The local position returns to `(6400, altitude, 6400)` and its
view/up/velocity rotate between the old and new ENU bases. Radius **plus camera
height** is used here; terrain address inversion instead uses the radius of the
surface datum. The camera altitude limits remain 500 m to 30,000 km.

A compare-and-set under the location mutex rejects a rebase if an explicit
geographic Go has intervened. Each successful rebase advances the existing
geographic revision and immediately invalidates resident ground. Immutable
reader snapshots remain safe, and old worker results cannot publish into the
new revision. The existing renderer relocation path rebuilds its fixed meshes
and sources; their vertex, atlas, tile-cache and worker limits do not grow.
Overview rendering already skips DEM requests above 500 km altitude.

Prepared roads/vegetation/building consumers must use
`Earth::ToLocal(originLatitude, originLongitude, latitude, longitude)` and reject
`!visible`; its `x,z` replace their former equirectangular formulas. Those two
consumer files belong to the parent provider slice and are intentionally not
edited by this commit.

## Verification

All checks ran in the isolated worktree, without game or GPU access:

- `scripts/Test-EarthCoordinates.ps1`: spherical projection/inverse at origins
  up to +/-84 degrees, dateline wrapping, curved-datum agreement, independent
  global ECEF position/view/velocity continuity, altitude deadbands, 900
  synthetic constant-altitude equatorial waypoints spanning a full circuit,
  stale rebase/ground rejection, immutable snapshots and conservative atlas
  bounds passed.
- `scripts/Test-EarthGround.ps1`: existing resident triangles, coverage,
  generation ordering and concurrent publication/query tests passed.
- `scripts/Test-EarthSurface.ps1` with the existing parent vcpkg directory:
  actual CPU atlas composition, source alignment before/after an origin shift,
  cancellation, imagery seams and latest publication passed. No source was
  downloaded or modified.
- Production `clang-cl /Zs` using configured build flags, own source/include
  paths and existing generated dependencies passed for `CameraHold.cpp`,
  `EarthTerrainStream.cpp` and `EarthSurfaceStream.cpp`.

Installed verification belongs to the parent: cross a near-altitude rebase,
check an orbital rebase preserves view and speed, cross the antimeridian, wait
for paired current-revision terrain/features, and repeat the resident-ground
test. These checks are pending here, not represented by the CPU circuit test.

## Limits

This is geographic free-flight exploration, not a global simulation rebase.
Finite mission entities, AI and navigation remain local and are not moved to
follow geographic origin changes. Ground is explicitly unavailable while the
new revision streams; a visual globe is not proof of collision support.
Earth flight follows the view and preserves its physical direction, so a
tangent flight naturally gains radial altitude. It does not auto-level or
auto-orbit. Origin latitude remains bounded to +/-84 degrees, with Mercator
source coverage limited to +/-85.051 degrees. Unprepared feature/ortho cells
still use the existing fallback. Near terrain retains its existing vertical
height extrusion above the spherical datum rather than becoming a global
radial-elevation physics model.
