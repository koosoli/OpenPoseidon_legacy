# Earth manual flight ground clearance

Source diagnosis: `CameraVehicle::Simulate` in `CameraHold.cpp` enforced the
geographic Go minimum on every manual movement, both before rebasing and before
`Move`. Its Earth floor was still a -12000 placeholder. Rebase validation also
required 500m. Speed used radial datum altitude, so flight just above a mountain
could be much faster than flight at the same clearance over lowlands. Wheel
zoom had a 500m floor and a 1000m offset, producing large steps near terrain.

The new Earth-only manual path uses one immutable resident ground publication
and its geographic revision. It clamps the candidate to that patch's exact
triangle height +0.30m. Speed and wheel zoom use actual ground clearance when
covered; speed retains the existing high-altitude curve and wheel zoom retains
its exponential factor. Raise/lower input uses physical radial up. Camera
motion remains independent of finite-map units and navigation.

There is no fabricated ground where coverage is absent. Orbital/uncovered
flight retains the 500m **radial** fallback rather than 500m local Y. At a
resident patch boundary near the surface, horizontal movement stays on the
covered side while ascent remains available. A low camera whose support was
retired during rebase waits for fresh support without being snapped upward to
500m; downward/horizontal motion is held and ascent remains available.
Negative source elevations and curved local datum heights are supported. The
existing ocean render proxy does not become physical bathymetry.

Internal rebase accepts bounded physical altitudes [-12000,30000000]m and still
rejects stale revisions/polar origins. Explicit geographic Go/initial-location
validation remains 500..30000000m. The Earth panel labels this as **Go starting
altitude** and explains the separate manual ground-clearance limit. Rebasing
below 500m does not change the physical altitude to the navigation minimum.

CPU verification passes:

- `scripts/Test-EarthFreeFlight.ps1`: actual rendered triangle diagonals,
  lowland regression below 500m, high-mountain speed, negative/sea elevations,
  curvature/radial fallback, patch-edge/stale support, small wheel steps,
  numeric guards and low/negative physical rebases.
- Existing `Test-EarthGround.ps1` and `Test-EarthCoordinates.ps1`, including
  revision invalidation/concurrency and 900-rebase continuity.
- Production `CameraHold.cpp` and `DebugOverlay.cpp` syntax checks using the
  installed build's compile arguments; installed-test PowerShell parser and
  geodesic self-test. No game/GPU launch by this agent.

Parent owns matched build/deploy and installed acceptance. The existing owned
process harness now has a short regression arm:

```powershell
./scripts/Test-EarthGeodesicInstalled.ps1 -ExpectedCommit <installed-commit> -GroundApproachOnly -Offline
```

Run through `scripts/with-game-lock.sh` with the existing Luxembourg elevation
window cached. It starts at published ground+3m below 500m, uses real held SDL
Z/29 to reach ground+0.30m, keeps Z held to verify no tunneling, captures the
actual near-ground view, and uses Q/20 to ascend. It records actual camera
positions, ground revision/generation and clearance. The original geodesic
near/orbit arms remain available without the switch. Wheel behavior has CPU
coverage; interactive wheel and final close-ground image inspection remain
required. No installed fix or visual acceptance is claimed by these CPU checks.
