# Installed Earth geographic rebase smoke test

`scripts/Test-EarthGeodesicInstalled.ps1` is a separate bounded owner test. It
requires an installed matching EXE/DLL deployment, existing NASA overview
imagery and the game lock. It refuses an existing game, uses an isolated user
directory, records the deployment stamp and stops only the PID it launches.
It does not build or deploy anything.

Owner invocation from Git Bash after integration/deployment:

```sh
LOCK_OWNER="Earth geodesic acceptance" scripts/with-game-lock.sh powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Test-EarthGeodesicInstalled.ps1 -ExpectedCommit DEPLOYED_COMMIT
```

Replace `DEPLOYED_COMMIT` with the installed source hash, at least eight hex
digits. `-Offline` blocks network transfers and requires both tested near
elevation windows to be cached already. Missing offline coverage fails the
fresh-publication check explicitly; it is not reported as passing ground.
Optional imagery/features/buildings and the configured content provider are
disabled, so this test adds no feature downloads. Overview imagery is never
downloaded by the test. It uses only the initial region, its nearby rebase
window and an orbital dateline view, retaining the existing 256-tile/2-MiB-per-
tile elevation cache bounds.

The test starts at Luxembourg 49.61,6.13, waits for initial resident support and
requires terrain below 950 m so its 1000 m camera is above actual land. It positions the actual Earth manual
camera 10 m inside its 32 km threshold at local Y=1000 m, and holds ordinary
SDL D until a simulation tick advances the geographic revision. It then checks:

- Actual camera motion follows the original physical east line. Its global
  ECEF position has no transverse jump over 10 m, and its physical direction
  and up vector agree within 0.0001 across the origin change. The displacement
  is bounded to 5 km, with at most eight origin increments during the short
  input arm; ordinary runs should need one.
- Old-frame camera coordinates lose resident support. A query can report
  generation zero while loading, or a newer generation if publication already
  won the race; it cannot retain the old generation.
- Fresh coverage at the new anchor matches the new revision and a real terrain
  upload log generation. A superseded patch cannot publish after relocation.
- An orbital setup at latitude 0, longitude 179.5, altitude 18,000,000 m crosses
  the 400 km threshold with actual SDL input. Its new origin wraps to negative
  longitude, preserves the same ECEF pose invariants, and exposes no near
  ground or near-DEM publication. Orbital displacement is bounded to 2000 km.

`result.json`, engine/stdout/stderr logs, the actual camera/ground samples,
performance and two screenshots are saved under
`build/earth-geodesic-installed/<timestamp>-<id>/`. Partial arm samples remain
in a failing receipt. Screenshot existence is checked; appearance still needs
owner inspection. The receipt tests free-flight coordinates and resident
publication, not globally moved mission entities, global navigation, player
walking, a complete circumnavigation, or measured velocity continuity. The
independent CPU tests cover vector/velocity transforms; this runtime test
checks actual motion and camera pose without adding engine diagnostics.

## Source-only verification

The PowerShell parser and the following CPU-only command passed in the isolated
worktree; neither starts the game nor touches its files:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Test-EarthGeodesicInstalled.ps1 -SelfTest
```

Self-tests cover the location/SQF parsers, an orthonormal ECEF basis, a correct
spherical rebase with real east displacement, and negative controls for
transverse position jumps and a rotated physical view. Installed execution is
delegated to the parent and remains pending at this checkpoint.

## Following-tick up-vector regression

The parent installed run `20261008-134056-4b8f31` crossed the near threshold
and preserved physical direction (error 2.85e-8), but failed physical up
continuity (error 0.0053388). `CameraVehicle::CamEffectFOV` subsequently rebuilt
the camera with local `VUp`, discarding the roll transported by the rebase.
Manual altitude-scaled Earth cameras now retain their current orientation's
up vector when aiming at the virtual target. Native/scripted cameras retain
their prior world-up behavior. The runtime assertion is unchanged, and the
CPU self-test additionally rejects direction-preserving roll loss. A parent
installed rerun is required; this source fix does not establish a runtime pass.
The failed receipt's up vector matches the normalized projection of local
world-up onto the plane perpendicular to its direction within 7.26e-8,
confirming this source path. The PowerShell parser/self-test, coordinate
regressions and production `CameraHold.cpp` syntax check passed after the fix.
