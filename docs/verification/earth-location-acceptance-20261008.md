# Installed Earth location acceptance

`scripts/Test-EarthLocations.ps1` tests live relocation in one installed game
process. Run it through the game-folder lock after deploying the combined
commit, for example from Git Bash:

```sh
LOCK_OWNER="Earth location acceptance" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-EarthLocations.ps1 -ExpectedCommit <installed-commit>
```

The script refuses an existing game and checks `DEPLOYED-FROM.txt` before and
after. It stops only the process it starts, restores its environment variables,
uses an isolated user settings directory, and writes screenshots, performance
samples, patch receipts and `result.json` under `build/earth-locations/`.

It reuses the existing external NASA overview cache; it does not download that
image. Elevation uses the existing bounded worker/provider cache. Each region
gets one request and a bounded wait (default 150 seconds, configurable 60--240).
Source retries remain the worker's existing timed backoff. Overall execution
has a deadline, and service unavailability fails acceptance without claiming
the rendering passed. Rapid requests revisit already loaded cells rather than
initiating a bulk dataset download.

Coverage:

- Alps, Grand Canyon, and equatorial longitude 179.995 (fine patch crosses the
  dateline), then return to Alps, all through `triEarthLocation`.
- Query/request coordinates and revisions, actual camera anchor and altitude,
  and uploaded patch geographic-origin/revision receipts agree.
- Queries leave the revision unchanged; successful Gos increment once.
- Latitude 85, longitude 181 and altitude 499 reject without changing either
  camera or origin state.
- Rapid Canyon -> dateline -> Alps requests may not upload a superseded origin
  after the final renderer rebase receipt.
- Screenshots exist, cached elevation files remain at most 256 and 2 MiB each,
  and runtime logs contain no device/validation/crash errors.

Script syntax and synthetic helper checks for query parsing, actual-camera
comparison, patch receipt matching and rejection of stale publication passed.
The installed game test has not been run by this subagent; runtime evidence
must be recorded after the parent runs it on the combined build. Screenshots
still require visual inspection for terrain/image seams.

This remains an opt-in presentation preview with explicit geographic teleport.
It is not seamless globe circumnavigation, worldwide collision, or a populated
world editor. The NASA overview is coarse; high-resolution global ground
imagery is absent. Any subsequent optional building layer needs separate
acceptance and attribution, and is not established by this elevation test.
