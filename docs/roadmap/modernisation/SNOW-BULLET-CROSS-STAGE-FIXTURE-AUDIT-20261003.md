# Snow bullet cross-stage fixture audit, 2026-10-03

Source/script candidate only. No game, device test, deployment, or production
snow geometry change was performed for this candidate.

## Recorded failure

Root campaign `build/snow-bullet/directional-original-rifle-20261003-114847-4a6327`
ran the installed b59f455a pair. It exited normally but failed the actual cell
gate `Actual changed cell escaped directional impact capsule`.

The actual before inventory contains 625 cells, all deficit zero, sampled at
paused simulation time 12.274 seconds. The engine records three actual terrain
terminal events:

| Event | Simulation time | X/Z | Entry length | Centre before/after |
| --- | --- | --- | --- | --- |
| Commanded dry calibration | 10.166 | 4975.396 / 4676.702 | 0 | 0 / 0 |
| Unrequested shot while resuming aim | 12.291 | 4975.402 / 4676.711 | 0.63721m | 0 / 0.0819m |
| Commanded snow-positive shot | 16.643 | 4975.398 / 4676.711 | 0.34754m | 0.0819 / 0.1629m |

The second event occurs immediately after `triClearView` and `setAccTime 1`,
before the positive aiming measurements. The positive stage sampled its
starting counters *after* aiming: Fired 2, ammunition 28. Its local one-click
proof then accepted Fired 2 to 3 and ammunition 28 to 27. This did not account
for the actual second shot between the pristine before inventory and that
starting counter.

There are nine after-inventory cell centres outside the commanded positive
capsule. All nine lie inside the earlier, longer capsule. For example,
cell centre (4975.4375, 4676.0625) has actual deficit 0.08127330988645554m;
its distance to the measured capsule is 0.3035288937546718m, versus radius
0.227m plus the unchanged 0.002m trace tolerance. Its distance to the earlier
capsule is 0.03784780417048348m. The failure correctly rejects interpreting
the cumulative two-shot inventory as one shorter shot.

## Kernel comparison

`SnowField::ModifyDisc` centres the capsule at impact minus direction times
half length, clamps the signed projection to plus/minus half length, and uses
the perpendicular projection for capsule radius. The fixture measures from
the impact endpoint and clamps the signed projection to minus length through
zero. These expressions are equivalent for the normalized source direction.
The actual radius 0.227m is above CellSize 0.125m and below the 0.35m bullet
clamp. Actual samples use `(index + 0.5) * 0.125` as production does.
Millimetre-rounded traces and five-decimal directions cannot explain the
observed centimetre-scale escapes.

The pure PowerShell self-test compares both expressions over 625 native cell
centres for each recorded snow event, checks the recorded counterexample,
and preserves the existing outside, forward-direction, round-only,
bounded-source, shelter/roadway, malformed-trace and settling falsifiers.
This establishes no production snow-kernel defect in this campaign.

## Fixture candidate

- Send an ordinary SDL left-button release before releasing the photographic
  view and again before unpausing. Allow 100ms of normal SDL pumping; harness
  acknowledgement alone means the event was queued, not consumed.
- Retain the completed shot ledger across stages. Assert unchanged Fired,
  loaded ammunition, terrain and terminal counters before aim, after camera
  release, throughout ordinary aim and after the paused UI image. An actual
  extra shot now fails before the commanded click.
- Keep the actual one-shot Fired/ammunition/terrain checks and add the exact
  one-terminal-event check. Capsule extent, source admission and outside-cell
  tests are unchanged; no union of multiple capsules is accepted.
- Record `afterPatch` before shape assertions so a shape failure retains the
  actual inventory in result.json.
- Always release the ordinary SDL button before cleanup unpauses the owned
  game for normal exit.

The source of the unrequested resumed trigger is not established. Explicit
ordinary release may prevent it; if it persists, the new cross-stage gate
diagnoses it without hiding the shot or widening the capsule. A production
input change would require a separate source diagnosis.

## Validation and remaining runtime work

`powershell -NoProfile -ExecutionPolicy Bypass -File
scripts/Test-SnowBulletRuntime.ps1 -SelfTest` passes, without game/GPU/profile.
`git diff --check` passes.

Root must run the actual installed-game campaign, retain all shot and capsule
guards, inspect the snow images, and run covered-ground/refill controls after
the positive passes. This candidate does not accept the appearance of the
directional groove or claim that the resumed-trigger issue is fixed.
