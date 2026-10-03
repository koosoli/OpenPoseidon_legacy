# Land rotor fixture: preserve both audited sea coordinates

The installed `e017e028` campaign
`combat-audit/build/rotor-land/actual-land-dust-snow-strength-20261003-141948-c55a65`
was invoked with actual `-SandX 2475 -SandZ 5075 -SeaX 1750 -SeaZ 3100
-ManualPilot`. It failed at its first shallow-sea query with
`{"ok":false,"error":"finite x/z required"}`. The actual harness request
is `water_bathymetry` with `x:1750.0` and `z:null`. There is no accepted sea
footprint or water-downwash observation from this campaign.

The source already casts the bound Nullable parameters to doubles. The failure
is the singleton array expression `@(@([double]$SeaX,[double]$SeaZ))`:
PowerShell flattens it into two scalar items, so the first foreach candidate is
1750 and its second indexed coordinate is null. A typed `.Value` repair would
not address this actual request.

The runner now creates finite named `{x,z}` records through `Sea-Candidates`.
An explicit audited pair produces one record; the ordinary no-pair path retains
the same ten read-only candidates at SandX minus 128 m increments and SandZ.
The exact current script parameters are passed through this helper in both the
runtime path and SelfTest. Partial bindings and nonfinite coordinates refuse
before game access; finite zero coordinates remain valid. Runtime bathymetry
reads each named field, preserving the existing 25-sample shallow-sea proof,
physical hover, land-particle suppression and unchanged water acceptance scope.
No harness, physics, particle, roof, wetness or numeric emission guard changes.

The failed campaign nevertheless reached and retained these land observations:

| Stage | Actual native trace evidence |
| --- | --- |
| Dry sand | Four sampled rows, each emitted 6; final live 48 |
| Deposited snow | Four sampled rows, each emitted 6 snow puffs; final live 42 |
| Roof covered | Four sampled rows, emitted 0 and sheltered 8 each; final live 0 |
| Roof removed | Four sampled rows, each emitted 6; final live 48 |
| Actual rain wet | Four sampled rows, emitted 0; final live 0; liquid rain 0.95 |
| Engine off | No emission rows; actual engine-off gate passed |
| High altitude | No emission rows; actual height gate passed |

Trace rows are sampled receipts, not a complete event total. Nine PNGs remain:
dry-sand, deposited-snow, paused-a/b, engine-off, roof-covered/removed,
high-altitude and real-rain-wet. They can support visual review even though the
whole campaign status is failed. The owned game exited normally with code zero;
both installed file hashes, sizes/timestamps and deployed stamp match before
and after. There was no forced cleanup or binary replacement.

Focused source checks pass with actual PowerShell script binding:

```
pwsh -NoProfile -File scripts/Test-RotorLandRuntime.ps1 -SelfTest
pwsh -NoProfile -File scripts/Test-RotorLandRuntime.ps1 -SelfTest -SandX 2475 -SandZ 5075 -SeaX 1750 -SeaZ 3100 -ManualPilot
pwsh -NoProfile -File scripts/Test-RotorLandRuntime.ps1 -SelfTest -SeaX 0 -SeaZ 0
```

An actual invocation containing only `-SeaX 1750` refuses with the pair-required
error before game/device work. Pure checks also refuse NaN/infinite pairs and
preserve default candidate count/order. Existing rotor schema, actual neutral
hover, canopy clearance/source geometry and wet-ground suppression selftests
continue to pass. `git diff --check` passes. Installed final sea-stage rerun is
still required; this fixture repair does not itself accept water appearance or
all-map source coverage.
