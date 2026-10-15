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

## Runtime status (2026-10-15)

Played on the owner's PC with the CI builds (rolling pre-release `arma3-ci`), installed into
`_OpenPoseidon-Arma3\bin` and run by `scripts/arma3-pc/go.ps1` against the 13 missions of
`missions_smoke.txt` (one per world and DLC: Stratis, Altis, Tanoa, VR, Malden, Livonia,
Jets, Tanks, Marksmen, Laws of War, Apex, Karts, Contact), 40 s each, first- and
third-person screenshots at 8 s and 14 s.

| build | result |
|-------|--------|
| `59b5455` | 13/13 missions load, play and quit cleanly (exit 0) |
| `e51a102` | 10/13; the other three lost the renderer at creation to `failed printing to stderr: Zugriff verweigert` (the harness's redirected stderr file) -- the harness now retries such a start |
| `fea1971` | smoke 13/13; `missions_part00` (40 missions: Bootcamp, East Wind, Apex, ...) 40/40 exit 0 |
| `61b1a32` | jets showcase + `missions_part01` (41: VR scenarios, Contact's missions and sites): 39 exit 0, Contact `Outro2` crashed (`switchLight` on a UGV, fixed in `a82fa18`), the Contact scene `Enoch_intro1` (intro only) timed out (fixed in `f40f72f`) |

What the runs found, and what changed for it:

- **Mount time.** 521 archives took 59 s to mount (one `ReadFile` per header byte); a 64 KB
  header buffer brings it to about 4 s.
- **Config.** A module class whose arguments are named like its values (`faction: faction`)
  ended the merged config in the middle of CfgVehicles; quoted array strings that look like
  `(...)` were evaluated. Both fixed in `ParamFile`.
- **Moves.** Arma 3's binarised moves (`BMTR` 5) carry the move's events (`StepSound`, ...)
  after the bone names; without reading them every move failed ("Binarised animation could
  not be read", 2,211 per run). Then the soldiers stood posed, but knotted: arms through the
  body, legs wound round each other. The moves are read like Arma 2's (each bone local to its
  parent, a rotation about its rest joint, posing the turned model), but they pose the
  **source** model, and ODOL 73's reader dropped the binariser's bounding centre, so the man
  was posed around a point 0.3 m aside, 0.4 m behind and 0.8 m below his hips. Measured
  offline with the repo's own ODOL 73 and BMTR readers on `b_soldier_01` (bounding centre
  0.305, 0.836, -0.401) and four moves, as the mean stretch of the posed mesh's edges:
  4.3-4.9 around the stored centre, 0.08-0.12 around the bounding centre (Arma 2's soldiers:
  0.04-0.09). Taking the bones as model space instead measures 0.22-0.28, so the Arma 2
  reading stays.
- **Dressing.** An Arma 3 man is dressed by his class: `linkedItems[]` (helmet: ItemInfo
  type 605, vest: 701, drawn with `uniformModel`) and `backpack`; the model only carries
  placeholder proxies at its origin, without bones. The head proxy (`heads\bysta`) is drawn
  with `CfgHeads >> Default_A3`, the gear proxies with the class's items, held on the head
  (helmet, goggles) and the upper back (vest, pack); uniforms take their
  `hiddenSelectionsTextures`.
- **Scripts.** Function headers included as `..\paramsCheck.inc`, `0 = ...` init lines,
  list-box/list-n-box control commands, `animate` with three elements, `lock="LOCKEDPLAYER"`,
  a backslash ending a line in `BIS_fnc_moduleInit`, `ammo` on a CWA stand-in vehicle
  (division by zero in the helicopter showcase).
- **Worlds.** The generated world classes keep the Arma 3 world's latitude and longitude
  (the showcases start at dawn and at night; Everon's sun is not Stratis's).
- **Ships.** The carrier and the destroyer assemble themselves from their init handler
  (multi-part bases).
- **Heights.** Eden keeps a unit's or object's height (a pilot on the carrier's deck at
  23.7 m ASL, atlOffset 0.04 because the deck is the surface); CWA stands everything on the
  ground or the sea. Every non-crew unit, vehicle and object more than 1.5 m above the terrain
  or the sea is put back at its height from its own init line (Eden's height is a soldier's
  feet and a vehicle's model origin, which is where this engine has their origins).
- **Crashes.** Symbolised from the CI's pdb (the crash handler prints module addresses; the
  image base is found by matching `WinMain`/`__scrt_common_main_seh` at the bottom of the
  stack): `dyn_cast<StreetLamp>` accepted any entity, so `this switchLight "OFF"` on a vehicle
  crashed. Six more classes had the same missing class id (see the coordination note).
- **Script surface, third round.** `try`/`catch`/`throw`; eight nular commands the stub table
  had as unary (`allCutLayers`, `isTutHintsEnabled`, ...); `#include "\a3\..."` from the
  file-system root (every Contact mission's `description.ext` failed to preprocess);
  `reveal [obj, n]`; `getGroupIconParams` (50,000 errors a run in `VR.VR`); action titles
  without their structured-text tags; a weapon's name as its own muzzle.
- **Motion and sound census (test runs).** Before `ARMA1-TEST PASS` a run now logs which AI
  soldiers on foot never moved and what their AI was doing (unit state, the group's command,
  waypoints, current move, disabled AI), and what the sound system was asked for (files,
  requests, failures, plays, what plays at the end, fadeSound/fadeMusic), plus a few script
  values (`OP_A3_PROBE` for more). The first census (`b898168`): the AI walks (Tanks: 72 of 89
  soldiers moved), but every unit a mission held back with `disableAI "MOVE"` stayed --
  `enableAI` was an ignored command; and the showcases opened engines, helicopters and
  footfalls without ever starting a wave, while Contact's `Intro1` played 134 sounds.
- **Why the showcases were silent.** Each opens with `0 fadeSound 0` and fades the sound
  back in when `BIS_fnc_titlecard` / `BIS_fnc_establishingShot` are done. Those waited for
  the mission display (`findDisplay 46` answered displayNull), for title resources to
  register themselves from onLoad (never run; Arma 3's own RscTitles did not exist for the
  engine), for a named cut layer (`"name" cutFadeOut 0` was a type error) and for
  `RscStatic_display`, which RscStatic's script sets -- a script Arma 3 compiles at preStart.
  Each now has a stand-in (`GameStateExtA3Rsc.cpp`): a cutRsc'd class runs onLoad on a
  numbered stand-in display, compiles its script on first use, and closes after its duration
  (onUnload; isNull then reads it null). With `0fb326d` the title card finishes and every
  showcase ends at fadeSound 1 with waves playing.
- **Step sounds.** Arma moves give the step times as `soundEdge[]`; only `soundEdge1/2` were
  read, so no Arma soldier ever made a step sound.
- **Vehicle sounds.** An Arma 3 vehicle with its own model takes its outside engine (a
  helicopter: its rotor) from its sound sets as `soundEngine[]` instead of the CWA carrier's.
- **Known, open.** A soldier in some Contact sites (`s_dolnik_00`) is drawn stretched, with
  long panels round him -- not yet explained; Arma 3 config noise ("Undefined base class
  'Components'" in a few CfgAmmo sensor classes); `nearestTerrainObjects [obj, ...] #0` in
  Contact's `Intro2` is wrong in Arma 3 too (`#` binds tighter than the command).
