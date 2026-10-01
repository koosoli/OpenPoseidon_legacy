# Ordinary player snow-shot runtime falsifier

`scripts/Test-SnowBulletRuntime.ps1` is prepared from integration `f40d397d`
(`82465a96` plus the real terrain-impact snow edit). Agent owns only this new
runner and document. Root runs the installed game serially after the matched
build; no game/GPU/deploy was performed while authoring it.

The fixture uses stock `perf_sand.noe`, an actual `SoldierWB`, an actual M16 and
ordinary SDL mouse movement/left-click. The player starts at `4975,4672.5`, facing
north, near the previously tested open soil walking lane. `AimDy` controls a
relative mouse movement, not a weapon transform or fake projectile. A dry
calibration shot must terminate on actual upward terrain 1.4–8 m from the
shooter, preventing overlap with ordinary boot marks. If the input direction,
collision or source cannot establish that fixture, the run fails. It does not
guess a hole coordinate from camera projection.

The actual Fired event records count, time, weapon, muzzle, firing mode and ammo
class only. It never pauses the game. The runner releases the mouse after one
Fired event and waits for an actual `SNOW_BULLET terrain=1` event and at least
200 ms of simulation after firing before pausing. Exactly one fired round,
one ammunition decrement and one new terminal terrain event are required.
There is no `triFirePlayerWeapon`, synthetic Shot, direct field stamp, forced
impact or AI firing substitute.

Snow starts enabled but at zero depth, with falling and snowline cover disabled.
The calibration hit must make no cut. The standard `dev_snow deposit` fixture
then adds 0.18 m of real stored snow. A paused 13×13-cell before grid surrounds
the measured dry impact. A second ordinary shot must hit within 25 cm of it,
report an actual changed deficit and remain within source depth/radius bounds.
The same grid after firing checks:

- a sampled centre corresponds to the actual before/after telemetry;
- changed cells stay within the actual radial cut, including half-cell margin;
- a shallower radial edge exists;
- outside powder is unchanged and no sampled deficit exceeds the snow depth.

Trace coordinates are rounded to a millimetre, while cells are 12.5 cm. Near a
cell boundary, the reported coordinate can fall on the other side. Centre
correspondence therefore checks candidate cell centres within half a diagonal
and matches the actual before/after trace values. This does not relax the
outside-kernel gate or select a random distant depression.

Fixed low and top pictures use the existing render-only `triSetView` camera
in raw engine XYZ, released with `triClearView` before ordinary firing. They
are captured before/after, after a five-second pause and after camera movement
away/return. Clock and cell comparisons require persistence; identical image
bytes are not required or treated as a visual snow mask.

The covered-ground negative spawns the audited real `CampEastC` /
`stan_eastc.p3d` centred on the measured hit, lifted 6 m to keep its Geometry hull
away from the shooter. Another actual terrain event must report `sheltered=true`,
an admitted query budget, `changed=false` and equal deficits. It must remain in
the central roof patch and leave the entire sampled grid unchanged. The roof is
removed afterwards. This proves the bounded physical roof policy only after a
real run, not by the class name or an absent mark.

Optional `-Refill` enables the existing snowfall storm preset and advances 20
actual simulation seconds. Increased global depth and a reduced original centre
deficit establish actual precipitation refill. The test does not manually clear
tracks to simulate refill.

## Safety, evidence and open controls

The script requires the shared game lock and preserves foreign processes. It
uses a private profile, disabled infinite ammunition, before/after deployment
stamp and exe/DLL hashes, recorded script/mission/source provenance, the TCP
transcript, exact event rows, PNG hashes, normal exit zero and shutdown proof.
Failed or forced cleanup is recorded and fails the run; only its own PID can be
stopped. Process environment changes are restored.

`-SelfTest` passed without launching the game: it exercises the actual strict
trace parser against malformed/nonfinite/unsupported cut rows, camera arithmetic,
SQF value decoding and the outside-kernel comparison falsifier. PowerShell
parsing and `git diff --check` passed. Flight/collision, live player input,
actual snow geometry and installed acceptance remain pending root's run.

Hard object/wall/roof-object and sea projectile routes intentionally never call
the terrain snow hook. The existing read-only ballistic snapshot provides ammo,
speed and direction, but not an actual terminal type and endpoint for those
routes. Absence of a snow row after firing cannot prove the intended negative
target was hit. Those controls remain explicitly unexercised here rather than
being reported as passed. A later source-bounded read-only terminal query can
make them concrete without changing the firing controller.

The corrected `50db754c` normal-grade settled attempt
`settled-normal-grade-fixed-20261001-022738-92cdb0` again establishes dry and
snowy real terrain hits, a7.71cm retained cut and142 unchanged outside cells.
The lifted6m tent still prevents the third shot from producing a terrain row;
its actual terminal target is unknown. The run remains failed and exits normally.
Settled pictures remove the impact plume, but the snow remains very bright and
the depression has weak visible contrast. Appearance is not accepted by these
field counters.

An opt-in terminal-site trace now observes ground, water, object and expired
routes directly, including rounds too short for the per-frame ballistic recorder
to sample. Under the existing `POSEIDON_SNOW_BULLET_TRACE=1` it logs at most16
local nonexplosive terminal events per quarter simulation second, with actual
position, velocity and hit-object identity. It does not alter collision or snow
policy. Default-off cost is an early flag check. Compilation, deployment and a
new covered-shot target diagnosis are required before accepting this telemetry.

Pictures still require visual judgment of the hole and retained powder rim.

The first installed attempt on `d99bf434`, retained at
`build/snow-bullet/first-actual-20261001-020422-cac09d`, established a real dry
terrain hit and then a real snowy hit with a 7.62 cm stored cut, radial edge,
unchanged outside powder and persistence after pause and camera return. Low
and top captures show a soft circular depression. The complete campaign failed:
the third, covered shot produced no terminal terrain event with the tent lifted
3 m. That absence is not a passing roof exclusion. The fixture now lifts the
same real tent 6 m to keep its walls clear of the projectile and still requires
an actual sheltered terrain event. Normal exit and pair hashes passed for the
failed attempt; the corrected covered control and precipitation refill await
another installed run.
This runner does not claim raised displaced mass, projectile collision with
the snow top, arbitrary-map coverage, multiplayer replication or visual quality
from deficit counters.

```bash
pwsh -NoProfile -File scripts/Test-SnowBulletRuntime.ps1 -SelfTest
LOCK_OWNER="ordinary snow shot" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-SnowBulletRuntime.ps1 -Label stock-noe -Refill
```

Evidence is retained under `build/snow-bullet/<label>-<timestamp>-<id>/`.
