# Arma 3 missions straight from an installed Arma 3 folder, 2026-10-14

Third generation of [ARMA12-DIRECT-MISSIONS-20260910](../roadmap/ARMA12-DIRECT-MISSIONS-20260910.md),
after [Arma 1](arma1-direct-missions-20261011.md) and [Arma 2](arma2-direct-missions-20261012.md).
Branch `MrDudeX-arma3` (on `MrDudeX-arma1`, which carries `MrDudeX-arma2`); coordination in
[ARMA-COMPAT-COORDINATION](../roadmap/ARMA-COMPAT-COORDINATION.md).

## How to start it

```
OpenPoseidon.exe --arma3 "F:\SteamLibrary\steamapps\common\Arma 3" --arma3-mission Showcase_Infantry.Stratis
```

`--arma3-mission` takes a mission folder name, a path inside the mission archives
(`showcases\Showcase_Infantry.Stratis`) or a full bank path (`a3\missions_f\showcases\...`).
`--arma3-campaign` starts a campaign, `--arma3-test-seconds N` plays N seconds and quits.
The `--arma1-*` options keep working; the newest generation given wins.

Test harness for the owner's PC: [scripts/arma3-pc](../../scripts/arma3-pc) (copied to
`<CWA>\_OpenPoseidon-Arma3`; `go.cmd` plays the missions of `job.txt`'s list with the build
in `_OpenPoseidon-Arma3\bin`, the game folder as working directory, so the shared
`Remastered` binaries are not touched by test runs).

## What is read, and from where

- **Nothing is copied.** `<Arma 3>\Addons` and the `Addons` folder of every DLC / platform
  folder (`Heli`, `Mark`, `Expansion`, `Jets`, `Orange`, `Argo`, `Tank`, `Enoch`, `Kart`,
  `Curator`, `AoW`, `Contact`, ...; not `@mods`, `!Workshop`, `Keys`, `BattlEye`, ...) are
  mounted read-only under the archives' own prefixes, `Missions` / `MPMissions` as
  `arma3\missions` / `arma3\mpmissions`. 552 archives on the owner's install.
- **Encrypted Creator DLCs are not read.** Global Mobilization, S.O.G. Prairie Fire, CSLA
  Iron Curtain, Spearhead 1944, Western Sahara, Expeditionary Forces and Reaction Forces ship
  their data as `.ebo`, which only the original game decrypts. Each folder's `.ebo` count is
  logged (152 on the owner's install) and nothing of them is opened.
- **Config.** Every `config.bin` (raP, including the 64-bit integer value type Arma 3 adds)
  and `Dta\bin.pbo` / `languagecore(_f).pbo` are merged in `requiredAddons` order into the
  private Arma config (52.7 MB of text on the owner's install; worlds Stratis, Altis, VR,
  Tanoa, Malden, Enoch). CWA's own `Pars` only gains the generated `A1x_<class>` /
  `A3_<world>` classes, as for Arma 1 and 2.
- **Missions.** `mission.sqm` in either Arma 3 layout:
  - Eden (`version=52..54`, binarised or text) is rebuilt into the Arma 2 layout the
    engine reads (`Arma3Eden.cpp`): groups, units, vehicles with their crews (`CrewLinks`),
    waypoints, markers, triggers, logics, layers, synchronisations (pairwise ->
    groups), waypoint activations, trigger owners, random-start markers, inventories,
    stances and the custom attributes that are plain script.
  - the Arma 2 layout (`version=12`, pre-Eden missions) is read as it is; only the
    pairwise synchronisations are rewritten.
  Offline over all 387 mission files of the install: 282 Eden missions and 69 with
  rewritten synchronisations convert and re-parse; 35 are pre-Eden without
  synchronisations and stay as they are; one (`Sites\test_guide.Enoch`) has no `Mission`
  class. 20,272 units, 68,486 vehicles/objects, 6,441 waypoints, 2,932 triggers, 8,222
  links; 242 entities without a counterpart (Eden comments, polylines).
- **Units and vehicles.** An Arma 3 class is carried by a CWA class of its kind, read from
  its ancestry in the Arma 3 config (`Tank`, `Wheeled_APC_F`, `Truck_F`, `Helicopter`,
  `Plane`, `StaticWeapon`, `ParachuteBase`, ...), and drawn with its Arma 3 model through the
  generated `A1x_` class. Animals are left out (no animal models yet). Simulation names
  with Arma 3's PhysX suffix (`carx`, `tankx`, `helicopterrtd`, ...) are read by their kind.
- **Weapons.** Arma 3 weapons by name and calibre onto the CWA weapon of their role
  (`arifle_MX_*` -> M16, `arifle_Katiba_*` -> AK74, `srifle_*` -> M21 / SVD, `launch_Titan_*`
  -> AALauncher / 9K32Launcher, `launch_NLAW_F` -> LAW, `launch_RPG*` -> RPG, `hgun_*` ->
  Beretta / Revolver / HK, ...): every scope-2 handheld weapon of the install maps.
- **Models / animations.** ODOL 73 through the existing reader; Arma 3 men are grounded and
  centred like Arma 2 men and face forward (`reversed=0`). `BMTR` version 5 (Arma 3's
  binarised moves: two extra header words, a per-array compression flag) is decoded; checked
  on a real move, 131 phases x 103 bones.

## Script surface added (SQF)

Only under `--arma3` (OFP, Arma 1 and Arma 2 missions see none of it):

- **Code values.** `{...}` is `CODE` (`typeName`, `isEqualType`, `str`), `compileFinal`.
- **The function library.** `CfgFunctions` of the Arma 3 config, the campaign's and the
  mission's `description.ext`; a function is compiled the first time a script names it
  (`BIS_fnc_*`, `BIN_fnc_*`, the mission's own tags), with Arma 3's `_fnc_scriptName`
  header; `.fsm` functions run their FSM. `uiNamespace getVariable` finds them too.
- **Mission start in Arma 3's order:** preInit functions, the objects' init lines,
  `BIS_fnc_initModules`, `initServer.sqf`, `initPlayerLocal.sqf`, `initPlayerServer.sqf`,
  postInit functions, `bis_fnc_init = true`, `init.sqf` -- each scheduled step after the
  one before.
- **Language core** (about 200 commands): `params`/`param`, `pushBack(Unique)`, `append`,
  `apply`, `findIf`, `select` with ranges, filters and on strings, `deleteAt`,
  `deleteRange`, `insert`, `reverse`, `sort`, `arrayIntersect`, `selectRandom(Weighted)`,
  `selectMax/Min`, `isEqualTo/isNotEqualTo/isEqualType*`, `joinString`, `splitString`,
  `trim`, `toFixed`, `linearConversion`, vector math, `distance2D`, `getRelDir/Pos`,
  `inArea(Array)`, `inPolygon`, hashmaps (`createHashMap`, `get`, `set`, `getOrDefault`,
  `keys`, `values`, `merge`, `forEach` with `_x`/`_y`), lazy `&&`/`||` with code,
  `_forEachIndex`, mission event handlers (`EachFrame` and the rest),
  `configClasses`/`configProperties`/`configHierarchy`/`getMissionConfigValue`, ...
- **Arma 3 forms of older commands** written as SQF over the engine's own:
  `setPosWorld`, `getPosWorld`, `AGLToASL` and the other position conversions,
  `allPlayers`, `fullCrew`, `createVehicleCrew`, `moveInAny`, `createSimpleObject`,
  `getUnitLoadout`/`setUnitLoadout`, `addMPEventHandler`, `getMissionLayerEntities`, ...
- **740 presentation-only commands** (UI controls, Zeus, Eden, cosmetics) are accepted with
  a neutral result, their arity read from the scripts of the install.

## Runtime status

The engine build of `5989082` compiles (Windows CI run 38616228687, 1 h 7 min cold).
Not yet played in the engine: see the open items in the coordination doc.
