# Arma 2 missions straight from an installed Arma 2 folder, 2026-10-12

Second generation of [ARMA12-DIRECT-MISSIONS-20260910](../roadmap/ARMA12-DIRECT-MISSIONS-20260910.md),
after [the Arma 1 slice](arma1-direct-missions-20261011.md). Owner's first mission:
**SP_EyeForEye** (Chernarus). Branch `MrDudeX-arma2`.

## How to start it

```
OpenPoseidon.exe --arma2 "H:\SteamLibrary\steamapps\common\Arma 2" --arma2-mission SP_EyeForEye.Chernarus
```

`--arma2-mission` takes a mission folder name (`C0_FirstToFight.Utes`), a path inside the
mission archives (`campaign\missions\C0_FirstToFight.Utes`) or a full bank path.
`--arma2-campaign` starts a campaign, `--arma2-test-seconds N` plays N seconds and quits
(script errors are logged and the mission keeps running), `--arma2oa <folder>` names an
Operation Arrowhead folder explicitly.

## What is read, and from where

- **Nothing is copied.** `<Arma 2>\AddOns` is mounted read-only under the archives' own
  prefixes (`ca\chernarus`, `ca\missions`, ...). Folders named `Arma 2 Operation
  Arrowhead*` next to the Arma 2 folder (the Steam layout) are found on their own and
  add `Expansion\AddOns` and `Expansion\Dta`. `OP_A2_ONLY=1` keeps plain Arma 2.
- **BAF, PMC and ACR are not read.** Steam ships their data under
  `DLCsetup\<DLC>\<DLC>_data` as copy-protected archives (`hprotect` header) that only the
  original game unlocks. Any archive carrying that header is skipped and logged
  (`... is copy-protected; not read`), whatever folder it is in.
- **Config.** Every `config.bin` (raP) of the mounted archives plus `Dta\bin.pbo` is read
  privately, merged in `requiredAddons` order and parsed once into a private config
  (`Arma1Compat::ConfigRoot`). CWA's own `Pars` is not changed; scripts' `configFile`
  resolves in the Arma 2 config first.
- **Worlds** come from the Arma 2 `CfgWorlds` (Chernarus, Utes, and with OA Takistan,
  Zargabad, Desert_E, ...). Their named places (`CfgWorlds >> world >> Names`) back the
  script locations (`nearestLocation`, ...).
- **Models / animations.** ODOL v48/49 and OPRW 24/25 use the existing readers.
  Binarised `.rtm` (`BMTR`) is decoded: per phase, LZO-compressed (arrays >= 1024 bytes)
  14-byte bone transforms (int16 quaternion / 16384 + three half-float translations);
  the quaternion is conjugated relative to CWA's convention (checked by a pivot
  least-squares fit, rms 5e-5).
- **Text.** `stringtable.xml` is read (converted to CWA's CSV form); `<localized id=../>`
  in briefings is resolved.

## Script surface added (SQF)

**Scheduling.** Every `spawn`/`execVM`/`addAction` script runs as a stackful coroutine
(Windows fiber, POSIX ucontext): `sleep` and `waitUntil` suspend anywhere -- inside
called functions, loops and conditions -- and a while loop holding the frame longer than
~3 ms gives it back, as Arma 2's scheduler does (`OP_A2_FIBERS=0` restores the Arma 1 line
compiler). Stringtable keys are case-insensitive, as in Arma.

Single-quoted strings; `#include`/`#define` through the real preprocessor for
`execVM`/`preprocessFile(LineNumbers)` (also `\ca\...` absolute paths); `exitWith` in
scheduled code; growable parser stacks (literal arrays of thousands of values); a
trailing `;` keeps a code block's value; spawned code keeps a reassigned `_this`.

Commands: `setVariable`/`getVariable` (objects, groups, `missionNamespace`,
`uiNamespace`), simple tasks and the diary (shown in the map notebook as *Plan* and
*Main*), the conversation system (`kbAddTopic` reads the `.bikb`, `kbTell` shows the line
and plays the recorded speech, `kbWasSaid`), scripted FSMs (`execFSM`, one transition
per simulation step; FSM-editor `"..." \n "..."` strings joined), locations
(`createLocation`, `nearestLocation(s)`, `locationPosition`, `text`, `setText`, ...),
`createVehicle [..]`, `group createUnit [..]`, `isKindOf` on class names,
`setVehicleInit`/`processInitCommands`, `attachTo`/`detach`, `addAction` with arguments
and code, `triggerActivated`/`triggerArea`, `currentWaypoint`/`setCurrentWaypoint`,
`emptyPositions`, `WFSideText`, `mapGridPosition`, layered `cutText`/`cutRsc`,
`endMission`/`failMission`, `allUnits`, `allGroups`, `nearestObjects`, `nearEntities`,
`getPosATL`/`setPosATL`, and presentation-only commands (post-process effects, loading
screens, UI list boxes, high command) accepted without effect.

The Arma 2 module objects (`FunctionsManager`, MP framework, ...) start from their class's
init event handler, as in Arma 2.

## Engine fixes found on the way (not Arma 2 specific)

- `EntityAI::NeedsAmbulance` spun forever for a unit that is its own person (virtual
  tail call compiled to a jump) -- found by the stall report below.
- `ArcadeTemplate` counts every loaded unit id before `Compact` (a short read no longer
  indexes past the table).
- An unknown vehicle class is skipped with a warning in compat mode instead of failing
  the mission start.

## Diagnostics

In test mode (`--arma2-test-seconds`), a stalled simulation is reported every 10 s with
the expression the evaluator last started, and at 10 s / 30 s with the main thread's
symbolised call stack. `OP_A2_WATCH="var,var"` and `OP_A2_EVAL="expr"` log values every
10 s of game time.

## Results so far

Every mission is played by `.a2build\run-a2-tests.ps1` (one log and one screenshot
each): 40 s from run 7 on. Until run 6 the screenshot, taken 20 s after the mission
started loading, also ended the run, so missions played only about 10-20 s. The `Templates\*` and `MPTemplates\*` folders are editor templates without a world
and are not counted.

| Run | Build | Missions | Enter the mission | Without a script error | Hangs / crashes |
|-----|-------|----------|-------------------|------------------------|-----------------|
| 3 | `a142bf4` | 34 (Arma 2, partial) | 34 | 19 | 6 hangs (Warfare: `NeedsAmbulance`) |
| 4 | `6651f0d` | 63 (Arma 2, incl. templates) | 50 | 23 | 0 |
| 5 | `f4a567d` | 98 (Arma 2 + Operation Arrowhead) | 91 | 27 | 7 crashes (flag proxy, fixed `47c5b1c`), 5 OA campaign time-outs |
| 6 | `516bc8b` | 98 (Arma 2 + Operation Arrowhead) | 98 | 63 | 1 stall (CE5A: pathfinding, guarded `534f5f3`) |
| 7 | `4799c30` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 96 | 0 |
| 8 | `f02e242` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 96 | 0 |
| 9 | `06789da` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 97 | 0 |
| 10 | `9e32ad8` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 96 | 0 |
| 11 | `c44ba31` | 98, laptop (Intel Iris Xe, `--render gl33`), 40 s each | 98 | 97 | 1 hang (SP_WarWelcome: modal `hintC`, fixed `7bf71f5`) |
| 13 | `9fa39fe` | 63 of 98 (stopped for a newer build), laptop gl33, Arma 2 soldiers by default | 63 | 62 | 1 crash (TE06_Parachute: water without a section), 1 stall (C6B_DogsOfWar: `AILocker::LockPosition`), both fixed `3dfb515`; MP_Detector exited silently once, passed on rerun |

SP_EyeForEye (the owner's first pick), all eight Arma 2 Bootcamp missions and the
Arma 2 campaign through I3 enter and run with no script error. Operation Arrowhead's
Takistan and Zargabad render; its bootcamp and scenarios start.

Run 6 is the first with scripts as coroutines: the Operation Arrowhead campaign no
longer times out (CE0-CE8 all enter and run), and every one of the 98 missions enters.
What it found, fixed afterwards:

- **Config strings were cut at 2 KB.** FSM states keep whole scripts in one string
  (C1_IntoTheStorm's opening radio conversation is 5966 characters); the config parser's
  word buffer ended them mid-block. Values are now read up to 256 KB.
- **`exitWith` inside a `call`ed function** broke out of the caller as well, so
  `_side call GetSideLetter` gave nil and the Warfare unit tables were built from
  `<null>SOLDIER`. `call` now takes the exitWith value as its result.
- **`typeOf`** returned the import's `A1x_` class name; scripts look it up in their own
  class lists (C7B's convoy). It returns the mission's name now.
- **Locations are their own type** (`typeName` = `LOCATION`, `locationNull`, `isNull`),
  which `BIS_fnc_locations` relies on.
- **Pathfinding:** the operative-path field map only walks fields on the map; a 60 s
  stall in CE5A_Sandstorm was inside that loop.
- Commands: `addSwitchableUnit`/`removeSwitchableUnit`, `ctrlShown`, `inputAction`,
  `showHUD`, binary `hideObject`, `moveOut`, `forceWalk`, backpack commands.

Run 7 (all 98 enter, 96 without a script error, none hangs) followed one more find:

- **`rank="LIEUTENANT"`.** CWA spells the rank `LIEUTNANT`; the mission loader stops
  reading a class at its first unknown enum value, so 236 Arma 2 units lost their name
  and init line. In C1_IntoTheStorm that was the helicopter pilot: never put in the
  seat, the Venom carrying the player fell into the sea. Arma 2's other spellings in the
  shipped `mission.sqm` files (`AMBIENT LIFE`, `DIAMOND`, `FILE`, `GETIN NEAREST`,
  `DISMISS`, `... SEIZED`) are read as the nearest CWA value, and an unknown value is
  logged. C1 now flies its route with no script error.
- `format` prints nil as `any`, `call` accepts a nothing value on its left.

A rerun of the missions that ended early or had errors (build `877165b`, then `f02e242`)
left no script error in any of them. Missions now say why they end on their own:

- **SPE1_Jackal** starts with a HALO jump from an archive script given without its
  leading backslash; `exec` did not find it, so the player started on the ground among
  the enemy. Fixed: the player now falls from 2000 m (`bis_fnc_halo`), and in the
  unattended test hits the ground at 36 s because nobody opens the parachute.
- **MPE1_Dogfighters**: the player's A-10 starts in the air and, with nobody at the
  controls, crashes after 9 s -- expected without input.
- **MP_Deathmatch / MP_TeamDeathmatch / C7A_WarThatNeverWas** end by their own end
  conditions in single player (end modes 3 / 2).

Run 8 showed the HALO free fall itself spinning (SPE1_Jackal, 132780 errors): the SQS
runner wrote its own clock into `_time` around every step of a coroutine script, and
fn_halo keeps its frame time in `_time`, so `time - _time` stayed 0 and the division by
it failed before the loop's `sleep`. Coroutine scripts no longer get `_time`
(`22b7a49`), a scheduled script resumes in a later frame at the earliest, and `0 atan2 0` is 0.
Rerun: no script error in any of the six missions checked.

The two missions left with an error: MP_Paradrop uses a variable it never sets
(`_targetmarkerdir`; a mission bug, Arma 2 reports it too) and MP_WhenDiplomacyFails
(Warfare `<null>SOLDIER`, fixed after the run).

## Second machine (laptop, Intel Iris Xe), 2026-10-13

Build `2e82aea` from the `Arma 2 / Windows game build` workflow, installed by hand into
`ARMA Cold War Assault\Remastered` (Steam in `C:\Program Files (x86)`; Operation Arrowhead
next to Arma 2 is found on its own).

- **WGPU on Vulkan:** SP_EyeForEye loads and shows the briefing; on the first 3D frames
  after "Continue" the driver reports `wgpu device lost (Unknown)` and the game exits
  (code 1). Same with `OP_A2_FIBERS=0`, so not the scheduler. Renderer issue on this
  integrated GPU, not Arma 2 specific.
- **`--render gl33`:** SP_EyeForEye plays, no script error in the log, diary and tasks
  created.
- `WGPU_BACKEND=dx12` not yet tried.

## Open findings from the run 8 screenshots

Every mission now leaves a screenshot about 30 s into play (`.a2build\tests`). Most show
the expected scene (bootcamps, the Chernarus campaign, Operation Arrowhead's campaign and
scenarios). Not yet right:

- **Utes, SP_TrialByFire:** the LHD that is part of the Utes map is drawn in pieces --
  frames and catwalks without the flight deck, parts floating above it. The parts are
  map objects, so this is about placing composite Arma 2 models (their origin vs. their
  bounding centre), not about the mission.
- ~~**MPE_MountainWarfare (+3Sided):** the Warfare players stayed outside the map.~~
  Fixed for run 9: `scopeName`/`breakTo`/`breakOut` were ignored (Warfare's list of
  starting points filled up with nil), `worldName` did not exist (no towns found) and
  `synchronizedObjects` was always empty (no teams). All three work now, and the player
  starts at the base. A scan of every SQF/FSM in the Arma 2 and Operation Arrowhead
  archives for command names the engine does not know added about 30 more
  (`overcast`, `fog`, `rain`, `wind`, `cursorTarget`, `actionKeys`, `nearRoads`, ...).
- **MPE1_Dogfighters:** the player's A-10 starts in the air with the engine reported off
  and glides into the ground in about 9 s without input.

Run 9 leaves one mission with a script error, MP_Paradrop, whose script uses a variable
it never sets (Arma 2 reports it as well). After it: `exitWith` in the body of a
`while` or `for ... from ... to` loop ends the loop (it was lost with the body's own
context, so such loops never ended).

Run 10 (with the loop `exitWith` fix) shows no regression. With Warfare now starting
properly, MPE_MountainWarfare gets further and reports `createUnit` with an undefined
soldier class twice (`Format ["%1SOLDIER", side letter]`) -- open.

Run 11 is the first on the laptop and the first with Arma 2 soldiers carrying their own
weapons (`A1x_` weapon classes with the Arma 2 models) and Arma 2 vehicles crewed by their
own crew classes; see [the asset plan](../roadmap/ARMA2-ASSETS-20261014.md). No mission lost
anything to that. SP_WarWelcome stopped at a modal `hintC` 25 s in, which an unattended
test cannot dismiss; test mode now shows it as an ordinary hint.
