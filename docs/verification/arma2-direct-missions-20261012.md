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

Every Arma 2 mission is played 25 s by `.a2build\run-a2-tests.ps1` (one log and one
screenshot each). Run 3 (commit `a142bf4`-era build, before the `NeedsAmbulance` and
trailing-`;` fixes), first 34 of 63 missions: **34/34 start and enter the mission**, 19
without a single script error. The time-outs (C5 Badlands, C6 Dogs of War, MP CivilWar,
Superpowers, UrbanWarfare, WarWelcome -- the Warfare missions) were the `NeedsAmbulance`
spin, fixed in `472fed2`. Run 4 (`6651f0d`, with Operation Arrowhead mounted next to
Arma 2): first 7 missions all clean; the full table follows when the run completes.
