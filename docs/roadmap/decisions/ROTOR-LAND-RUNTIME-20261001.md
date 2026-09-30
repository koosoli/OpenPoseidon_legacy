# Installed helicopter land-ring falsifier

`scripts/Test-RotorLandRuntime.ps1` is a candidate serial installed-game runner,
prepared from `19c5161c`. Root owns its first game run. Agent has not launched,
built, deployed or used the GPU for this work.

The runner uses the stock `perf_sand.noe` mission, actual `UH60` and an independent
`SoldierWB` pilot group. It requests AI flight with `flyInHeight` and `doMove`.
One explicit position change places each stage; subsequent observations use
real flight simulation. There is no repeated position pinning, forced velocity,
fake rotor speed, synthetic cloudlet or terrain stamp. A real hover must produce
two telemetry rows at RPM at least 0.65, height 8–28 metres and near the requested
site. Eight-second observations must then remain within eight metres and yield
at least three actual rows. Unavailable or drifting AI flight fails the fixture.

The default Noe sand point is the previously used `2675,5125`. Its actual
`dev_sand` source admission is required again at runtime. A stock ruin lies a few
metres north of the original manual walking lane; it can affect initial aircraft
seating or some airflow rays. This is a fixture limitation, not an assumed new
flight or emission defect. `SandX`/`SandZ` allow root to choose a verified clear
patch; actual source and flight gates are retained.

Controls execute in one isolated mission, in this order:

- Engine off and no fuel: no new land-emission rows; actual engine state false.
- Dry sand: actual emitted non-snow puffs, low liquid rain, actual RPM/height.
- Paused pair: unchanged simulation time and aircraft pose, no new emissions.
- Deposited snow: actual snow field depth and at least one snow puff.
- Roof canopy: 25 real audited `CampEastC` objects in a 6-metre grid, lifted
  three metres. The model is `stan_eastc.p3d`, not an invented shelter. A single
  tent is smaller than the bounded spoke ring (up to 11.4 metres). The tiled
  arrangement is still a candidate: at least one actual blocked-ray row and
  zero emissions are required, followed by renewed dust after all roofs leave.
- Above 35 metres: active engine, all recorded real poses above the admission
  window, no land rows. It relocates once, then uses actual AI flight.
- Real rain: no wetness override, liquid rain at least 0.95 and soil wetness
  at least 0.3, followed by no dry-dust emissions. Water state is read-only.
- Sea: bounded read-only bathymetry search, or caller-supplied `SeaX`/`SeaZ`.
  All 25 samples in the 64-metre square must lie below stock Noe's zero sea
  datum, and the centre must be shallow enough for a real low hover. No land
  emissions are allowed. Images retain the existing water downwash for review.

Every parsed row checks finite RPM, position, height and rain, at most eight
emissions/rays per pulse, snow count no greater than emission count and at most
128 live land puffs. The script records actual object/crew identity, source
sample, poses, weather, cloudlet rows and SHA-256 screenshots. It requires the
game lock, a private profile, before/after installed provenance and binary hashes,
normal exit zero and shutdown proof. Cleanup can stop only its owned process;
forced cleanup or changed provenance fails acceptance. Environment changes are
restored. No ocean material, backend, downwash, ripple or water-probe setter is
used.

## Verification

PowerShell parsing and focused extracted telemetry helper checks passed in the
agent worktree. Those checks establish parsing and rejection of malformed or
out-of-budget rows only. Actual flight, source coverage, canopy ray coverage,
normal shutdown, particle appearance and preservation of existing water visuals
remain pending the serial root run. Stills or emission counters alone cannot
establish that the ring looks convincing or that ocean downwash is unchanged.

Example from Git Bash in the integration checkout:

```bash
LOCK_OWNER="land rotor acceptance" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-RotorLandRuntime.ps1 -Label stock-noe
```

Retain `build/rotor-land/<label>-<timestamp>-<id>/result.json`, provenance,
TCP transcript, installed log and all captures. A failed candidate must be
diagnosed from those actual flight/source/ray values before changing production.
