# Actual standard-US muzzle-light runtime fixture

`scripts/Test-MuzzleLightRuntime.ps1` prepares a reproducible installed-game
capture. It is not installed acceptance by itself. The source candidate uses
the existing successful infantry bullet-effects path and one actor-owned light;
this fixture never calls a synthetic effects/light setter or an ammunition
firing cheat. The source decision is
[MUZZLE-FLASH-LOCAL-LIGHT-20260930.md](MUZZLE-FLASH-LOCAL-LIGHT-20260930.md).

## Run and evidence

CPU-only parser/admission checks require neither the game lock nor a game:

```powershell
pwsh -NoProfile -File scripts/Test-MuzzleLightRuntime.ps1 -SelfTest
```

After root has built/deployed the candidate and the owner's game is closed,
run from the repository root in Git Bash:

```bash
LOCK_OWNER="standard-US muzzle illumination" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-MuzzleLightRuntime.ps1
```

The default is two serial process arms, `On` and the exact previous source
ablation `POSEIDON_MUZZLE_LIGHT=0`, each with Night and Roof scenarios.
`-Scenario Night|Roof` and `-Arm On|Off` support focused reruns; single-arm runs
cannot establish matched ablation controls. Each process has a private profile,
TCP port, transcript and engine log. `result.json` stores the original deployment
stamp, executable/DLL hashes, actor/model debug identity, exact actual camera
and target coordinates, stock roof placement, actual world hour/weather,
ammunition/event history, pulse trace, image hashes and captured simulation
milliseconds. Every required command failure/unknown response is a failure.
The runner refuses an already-running game, restores process environment,
normally exits its owned process and verifies unchanged installed binaries.
Exceptional forced cleanup of only its own process is recorded as a failed
campaign, never as normal-exit evidence.

The player is the actual `SoldierWB` in the stock Noe `perf_sand.noe` mission,
with an ordinary M16 and ammunition. The controlled scene is land near
`[4975,4675]`; height comes from actual `water_bathymetry` centre data, not a
guessed altitude. Night is 00:00, Roof is 16:00 on 21 June 1985. Rain/fog are
queried and required dry/clear. Snow and weather particles are disabled;
cloud/wind/grass/temporal overrides are fixed, exposure and game brightness
are fixed at 1. The actual simulation clock starts each comparison at 100s.

The roof arm creates the existing verified `CampEastC` / `stan_eastC.p3d`
fixture 1.5m in front of the shooter, lifted 1.7m above ordinary placement.
Its actual world/model identity and placement are retained. The lift avoids
the stock solid Geometry hull pushing the actor; displacement above 30cm
is a failure. This is a **stock tent shelter, not an enclosed dark building**.
Its physical presence does not automatically prove interior darkness or
surface illumination. The surface camera includes more of the overhead
material than the body camera; actual retained pixels must be reviewed.

## Real shot and peak timing

Before firing, the capture camera is removed and the real player is returned
to its ordinary external view. SDL left-mouse input fires the rifle at
`accTime=0.02`; the existing `Fired` handler records the real event/time and
pauses simulation. In production, `Soldier::FireWeaponEffects` emits the
muzzle light before calling `EntityAI::FireWeaponEffects`, where the `Fired`
event runs. The enclosing successful `FireWeapon` operation then completes
its ordinary ammunition decrement. Therefore the pause preserves an actual
emitted pulse; it does not manufacture a light or bypass shot readiness.

One event and exactly one ammunition decrement are required. The selected
real player's magazine is checked against the rifle count. The actual
`query/play_state.time_ms` must agree with the recorded `time` at the event.
On-arm telemetry must contain exactly one new active `MUZZLE_LIGHT` bullet
row with 110ms lifetime, 0.70m core, 8.40m reach and immediate/daylight flags.
The trace world position is engine XYZ, explicitly converted against SQF
XZY actor position; a muzzle further than 3m from the shooter fails. The
actual captured age must lie in the 0–20ms peak. Off-arm intentionally emits
no new-pulse trace: its earlier source behavior is restored, and real
mouse/event/ammunition evidence still remains mandatory.

Two fixed camera views capture Before, Peak and Expired while simulation
is paused. PNG validity and unchanged actual simulation time are required.
Real simulation advances more than 200ms before Expired, beyond the 110ms
new pulse and previous 150ms housekeeping; a second shot is a failure.
Default two-arm runs additionally require matching actor/camera positions
within 5cm, matching ammunition history and exact initial 100000ms clocks.

## Review criteria and limits

Functional status ends with `surface-illumination-review-required`; it never
equates the existence of the point-light event or white flash pixels with
surface lighting. Review the same named Peak camera between On and Off,
with the Before and Expired frames as context. Require a warm transient
response on the shooter's uniform and weapon plus actual adjacent ground
and overhead canvas where visible. Exclude the muzzle sprite, particles,
sky, HUD and changed recoil silhouettes from material regions. The stored
world camera/target coordinates establish what was requested, but do not
invent a valid pixel ROI where the installed image has no actual surface.
No global-white count or unexplained image threshold is a passing gate.

Actual source tests, full build and installed run remain root-owned and
pending for this runner. The helper self-test exercises real production
trace grammar, wrong/inactive/unknown telemetry, nonfinite/out-of-world
positions, peak boundaries, wrong-owner distance, literal model paths and
SQF doubled-quote decoding. PowerShell source parses and diff whitespace
checks are separate source checks. They do not establish lighting.

An actual enclosed stock building, AI successful shots, prone/stance muzzle
placement, burst retrigger/no accumulation, slow AI cadence, actor deletion
and unload remain additional installed acceptance cases. Recoil/motion may
change material pixels between Before and Peak even during slow simulation;
matched ablation Peak ages are the principal surface comparison. The owner
must be able to see the shooter body through the independently fixed player
visibility path before its illumination can be judged.
