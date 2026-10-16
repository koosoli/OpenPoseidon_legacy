# Arma 1 missions straight from an installed Arma 1 folder, 2026-10-11

First vertical slice of [ARMA12-DIRECT-MISSIONS-20260910](../roadmap/ARMA12-DIRECT-MISSIONS-20260910.md),
stages 2-5 for one mission, chosen by the owner: **M09 Convoy Attack**
(`Campaigns\ca.pbo`, `missions\m09convoyattack.sara`, Sahrani).

## How to start it

```
OpenPoseidon.exe --arma1 "H:\SteamLibrary\steamapps\common\ARMA Gold" --arma1-mission m09convoyattack.sara
```

`--arma1-mission` also accepts a full virtual path
(`campaigns\ca\missions\m09convoyattack.sara`) or any mission folder inside the mounted
campaign/mission archives (`Campaigns\*.pbo`, `DBE1\Campaigns\*.pbo`, `AddOns\missions.pbo`).

## What happens

- **Nothing is copied.** `<root>\AddOns`, `<root>\DBE1\AddOns`, `<root>\Campaigns` and
  `<root>\DBE1\Campaigns` are mounted read-only under their declared PBO prefixes
  (`ca\sara`, `campaigns\ca`, ...). Arma 1 configs are not parsed (no config callback);
  `Dta\bin.pbo`/`core.pbo` are not mounted, so CWA's own `bin\` is untouched.
- The mission is started from its virtual bank path. A campaign mission keeps its
  campaign base directory (`campaigns/ca/`), so the campaign stringtable and
  `description.ext` (CfgSounds) apply as they did in Arma 1.
- **Worlds.** `CfgWorlds` gets generated classes `Sara`, `SaraLite`, `Sara_dbe1`, `Porto`
  (only when the wrp is present), inheriting `Eden` with their own `worldName` and an
  empty `Names` class -- the same construction as `RegisterLocalMapWorld`. An existing CWA
  class of the same name is never edited. `Landscape::LoadData` routes OPRW revisions > 3
  named by a CfgWorlds class to `LoadOprwModern` (OFP's OPRW v2/v3 keep the classic path).
- **Units.** Arma 1 classes that CWA defines (`SoldierWB`, `T72`, `BMP2`, `Ural`, `UAZ`,
  `HMMWV`, `M113Ambul`, ...) are spawned as CWA classes. Classes CWA lacks map to a
  counterpart chosen at run time from an ordered candidate list (first one present in
  `CfgVehicles`): `AH1W -> Cobra`, `Stryker_* -> M2A2/M113`, ... Each substitution is logged
  once (`Arma 1: CfgVehicles 'AH1W' is represented by CWA 'Cobra'`); a class without a
  counterpart is logged as such and left unchanged. `addWeapon`/`addMagazine` use the same
  mechanism for common Arma 1 weapon/magazine names. Physics, weapons and AI are CWA's.
- Arma 1 `CA*`/`DBE1*`/`Warfare` entries in a mission's `addOns[]` are not reported as
  missing addons when `--arma1` is active (they are the mounted archives).
- **Text.** Arma 1 ships stringtables and briefing/overview HTML as UTF-16 with a BOM;
  both are now decoded to UTF-8 (`DecodeUtf16BomToUtf8`).
- **Scripts.** New commands: `spawn`, `execVM` (binary and unary), `terminate`,
  `scriptDone`, `waitUntil`, `sleep`, `preloadCamera`, `preloadObject`, `isKeyActive`,
  `activateKey`. Spawned code is split into its top-level statements and run by the
  existing resumable `Script` machinery; a top-level `waitUntil {..}` / `sleep n`
  suspends exactly like SQS `@` / `~` and survives save/load. Suspension nested inside
  `if`/`while`/`call` is **not** available: `waitUntil` there evaluates once and `sleep`
  is ignored, each reported once in the log. `execVM` strips preprocessor lines and logs
  that it did. Script handles are numbers (no SCRIPT type yet). Campaign keys persist in
  `<UserDir>\Arma1CampaignKeys.txt`.

## Build note (not Arma 1 specific)

Building with VS 18 (MSVC STL 14.51) + clang 22 produced an exe that crashed in
`AppConfig::ParseCommandLine` before logging started: CLI11's header-inline
`valid_alias_name_string` / `is_separator` keep a function-local `static const std::string`.
The C++20 engine TU constant-initializes it into `.rdata`; the precompiled `CLI11.lib`
initializes the same COMDAT at run time -> write to read-only memory. AppConfig.cpp now
renames its copies of those two functions before including CLI11 (vcpkg's precompiled
package ships no impl headers, so header-only is not an option).

## Runtime result, 2026-10-11 02:24

Installed binary built from this branch on the owner's PC. Log: 49 mission archives
mounted, `Sara` world class registered, `AH1W` represented by `Cobra`, mission parsed with
7 groups / 21 markers, Sahrani loaded in 1.7 s, `InitVehicles ok=1`. The owner played it for
about seven minutes; normal shutdown (exit code 0), no script or mission errors in the log.

## All-missions run, 2026-10-11 22:32 (binary 2241051, stamped in the game folder)

131 missions (campaigns ca/ra/Merc, armory, `Missions\*`, `MPMissions\*`), each started with
`--arma1-mission <m> --arma1-test-seconds 25`, one screenshot at 20 s. "Entered" = the
mission booted and the runtime ran; "clean" = no script error until the test ended.

| binary | entered | clean (exit 0) | not booted |
|---|---|---|---|
| 550a359 (baseline) | 17 | – | 114 |
| 2241051 | 91 | 74 | 40 |
| f498083 | 129 | 117 | 2 |
| f11c945 (2026-10-12 00:43, stamped) | 130 | 122 | 1 |
| 9cfe1ef (2026-10-12 07:09, stamped) | 130 | 127 | 1 |
| fcd1a51 (2026-10-12 08:26, stamped) | 130 | 128 | 1 |
| 8edc850 (2026-10-12 09:30, stamped) | 130 | 129 | 1 |
| 212decb (2026-10-12 11:00, stamped) | 131 | 131 | 0 |

Fixed between the two: crashes in `Building`/`AILocker`/`OperField` (generated `A1x_`
classes inherited scope 0 from abstract CWA bases, so the type was no `BuildingType`);
groups above 12 crew seats (now split); unknown addons refusing the mission; binarised
`mission.sqm`; ~40 missing A1 script commands; Arma 1 `CfgSounds`; the strategic map
dialogs (`findDisplay`/`displayCtrl`/binary `ctrl*`).

Fixed after that run (f498083, not yet measured): Arma 1 weapon/magazine names in cargo,
pool and `addWeapon`/`addMagazine` (the remaining `Size: missing entry 'muzzles'/'modes'`
refusals), `init.sqf` scheduled like Arma 1 (comments, sleep), `switch/case/default`,
`isKindOf`, `compile`, `preprocessFileLineNumbers`, `distance` with positions,
`moveInTurret`, `createUnit` on a group, `str`, `date`, multiplayer missions started alone
play their first playable unit.

Remaining on f11c945 and the commit that addresses each (built, not yet measured):
s03dolores / fos_lament crash in `FileCache::Find` from a sound-preload worker (d8f2e36);
RA_M03 `switch` with sleeping cases (1c66833); m13/m14 `while` with sleep/`exitWith` in
scheduled code (26268f0, e104692); mp23 `parseText` (e104692); 03CAMP `enableAI` (1c66833).
Strategic-map labels now render (c991c58); its mission icons draw as pictures (328230a).

On 9cfe1ef the four left are: armory1 (config script commands, not implemented),
un-impossible_mission (third-party addon BBB006), m13 (`animationState`) and mp23
(`composeText`/`lineBreak`) -- the last two added in d9990e5. Since then also d512c1b:
constant arithmetic in config values (`style = ST_LEFT + ST_MULTI`), which makes the
strategic map's overview wrap over several lines.

On fcd1a51 three are left: armory1 (config script commands), un-impossible_mission
(third-party addon) and m13, whose `while` sleeps inside a `forEach` body -- compiled
since 157bd71 (not yet measured).

On 8edc850 only two remain: armory1.intro (config script commands such as
`configFile >> ..` / `getText`, not implemented) and un-impossible_mission (needs the
third-party addon class BBB006).

On 212decb every one of the 131 missions boots and runs its 25 s without a script
error. f68e88c (installed afterwards, armory-only re-run) adds the remaining commands the
Armory scripts call.

## Closing status (2026-10-12)

What the automated run proves: each mission loads (world, units, objects, scripts,
briefing path) and runs 25 s with no script error, crash or hang. What it does not prove:

- **Mission completion.** No mission was played to its end; outros, debriefings and the
  step from one campaign mission to the next were checked by hand only for the start of
  campaign `ca` (intro, briefing) and M09.
- **Multiplayer.** MP missions ran alone (first playable unit as player). No network game.
- **Armory (armory1.intro).** Loads and runs without errors, but the Armory library dialog
  does not appear within 65 s (camera at sea, cinema borders still on): its init does not
  reach `createDialog "RscLibrary_Main"` in that time, and the dialog uses Arma 1 control
  types (structured text, controls groups) the CWA UI does not have. Not functional.
- **Stand-ins / no-ops.** Classes from addons that are not installed (BBB006, BBB007,
  AAA739 in un-impossible_mission) are replaced by a plain unit of the same side or a game
  logic. Commands accepted without effect: particle sources, lights, setRank, enableAI,
  setAperture, enableTeamSwitch, camSetFocus, setCameraInterest, ctrlSetEventHandler,
  ctrlSetFocus, doFSM/execFSM (FSMs do not run), scopeName/breakOut/breakTo.
  ctrlSetFade shows/hides on commit (no fading). Structured text is shown as plain text.
- **UI.** Arma 1 font families are drawn with tahomaB24. The strategic map's overview
  box stays empty (its text goes to a single-line control).
- **Weapons.** See [Weapons, 2026-10-14](#weapons-2026-101415-binary-9d545a7-stamped-in-the-game-folder):
  Arma 1 weapons in hand, in crates and in the briefing pool, with Arma 1 magazine sizes.
- **Vehicles and animations.** Driving (HMMWV), flying (UH-60, OH-58 incl. AI), boarding
  and the Arma 1 soldier animations were checked with dedicated test missions; not every
  vehicle type and crew position was checked individually. Which CWA class carries which
  Arma 1 vehicle, and what is still wrong about it: see
  [Vehicles, 2026-10-15](#vehicles-2026-10-15-f58cb9b9-tested-from-dista1-vs18-the-game-folder-keeps-ef440e9).

## Weapons, 2026-10-14/15 (binary 9d545a7, stamped in the game folder)

Arma 1 soldiers carry their own Arma 1 weapons (178de3d): each one is a generated
`A1x_<weapon>` class with the Arma 1 model on a CWA weapon, which keeps the magazines,
ballistics and sounds. Fixed since, in order:

| commit | what was wrong | now |
|---|---|---|
| ab0d541 | weapons both games name alike (AK74, PK, M16A2) kept the CWA model | Arma 1 model on the CWA class |
| b1cab8b | carriers with `magazines[]={this}` (M60, LAWLauncher, RPGLauncher): the generated class took itself as magazine -- M240 with 0 rounds, launchers empty | the generated class names the CWA magazines |
| d044160 | scripts saw `A1x_M4AIM`; `hasWeapon "M4AIM"`, `removeWeapon`, `selectWeapon`, `fire`, `ammo` found nothing | `primaryWeapon`/`secondaryWeapon`/`weapons` report Arma names, the others accept them |
| 204f9ec | Arma 1 crates and vehicles handed out CWA rifles (CWA cargo inherited) | their own `TransportWeapons`/`TransportMagazines`; `getWeaponCargo`, `getMagazineCargo` |
| eabbeb3 | `Stinger`/`Strela` mapped to classes CWA does not have (AA soldiers unarmed) | `AALauncher` / `9K32Launcher`; the M136 is CWA's `AT4Launcher` |
| b36be5d | the briefing weapon pool read Arma 1 names (`AK74GL`, `SVD`, `30rnd_545x39_AK`) straight from CWA's CfgWeapons (18 `ca` missions + `m02minerconvoy`) | resolved like `addWeapon` |
| 13c0586 | `magazines` listed CWA names (`M16`) | Arma names (`30Rnd_556x45_Stanag`) |
| 1dd7199, 6483a1a | `fire ["HandGrenadeMuzzle", ...]`, `selectWeapon "HandGrenadeMuzzle"`, mode names CWA lacks (`FullAuto`) | mapped to the CWA muzzle that takes the magazine |
| 583f221 | gear/briefing pictures were the CWA carrier's | Arma 1 equipment picture |
| 99b4cde | `reload` did nothing | loads empty weapons |
| 1ab101c | HUD said `M16` over an M4 Aimpoint, actions offered to drop `AT4` | Arma weapon and magazine names |
| d0cdd15 | `5Rnd_127x108_KSVK` (crate/vehicle cargo in 79 missions) and `8Rnd_9x18_MakarovSD` had no counterpart | SVDDragunov / Tokarev magazines; a muzzle name that maps to nothing is logged once with the unit's muzzles |
| 87feff9 | magazines took the size of their CWA carrier: M24 20 rounds (Arma 1: 5), M249 100 (200), M107 20 (10), M9 10 (15), KSVK 10 (5) | a magazine of another size is a generated `A1x_<magazine>` with the Arma 1 count and name; every muzzle that takes the carrier takes it, but an Arma 1 weapon only the magazines its Arma 1 class lists. `OP_A1_MAGAZINES=0` turns it off |
| 79badd0 | -- | diagnostic: `OP_FORCEFIRE_LOG=1` logs why a scripted `fire` does not go off |
| 9d545a7 | `unit fire "LAWLauncher"` never fired -- CWA's own AT soldiers too: the soldier selected the launcher but stayed in his rifle move, where no missile may leave; `player fire` waited for an AI aim the player's input never let settle | a pending scripted launcher shot raises the launcher; the player's unit fires where the player aims |

Test missions (`tests\missions\a1weap*.Sara` on the build machine), on 1ab101c, d0cdd15 and
9d545a7 unless noted:

| check | result |
|---|---|
| weapons in hand, 3rd person, West + East (a1weapW/E, 0994802) | correct Arma 1 models, M136 on the back |
| first person, rifle and launcher (a1weap1st, a1weapP) | Arma 1 M4 Aimpoint / M136 models in hand |
| AI `fire` with every primary weapon, 8 soldiers | ammo 30→29 / 100→99 / 20→19 for all |
| player fires rifle and M136 by key (a1weapP) | 30→29, 1→0, rocket hits 80 m out |
| player throws a grenade by key (a1weapP3) | 10→9 magazines |
| AI throws a grenade by script, Arma names (a1weap, a1weapT2) | 10→9 (`fire ["HandGrenadeMuzzle", "HandGrenadeMuzzle", "HandGrenade_East"]` reaches CWA's `Throw` muzzle) |
| AI anti-tank in combat (a1weapAT) | SoldierWAT kills a T72 with the M136 at 90 m; SoldierEAT hits an M1 with the RPG7V (damage 0.22) |
| crates (a1weap) | WeaponBoxWest/SpecialBoxWest/AmmoBoxEast hold M4, M16A4, M249, G36, M107, Stinger, ... |
| briefing pool (m02minerconvoy, m01sniper) | 4 / 5 weapons, 45 magazines, no config error |
| script names (a1weap) | `hasWeapon (primaryWeapon x)` true for all; remove/add round trip |
| HUD and action menu (a1weapP) | `M136 Granatwerfer`, `Waffe M4 Aimpoint`, `Nachladen M136`, `M136 ablegen` |
| magazine sizes (a1weapMag, 79badd0 and 9d545a7) | M249 200, M107 10, M24 5, M9 15; the M24 shot empty 5→0 and reloaded to 5 with one magazine less; the M107 refuses M24 rounds (ammo 0); a CWA M21 magazine still fits the M24 (20) |
| scripted launcher fire, out of combat (a1weapL2, 9d545a7) | LAW (CWA soldier), M136, RPG7V, Stinger all fire: magazines 7→6 (one round reloaded), Stinger 1→0 |
| `player fire` (a1weap, a1weapL, 9d545a7) | Arma 1 player M4 30→29, CWA player M16 30→29 (before: no shot) |

Regression: all 131 missions entered and ran 25 s with no script error, crash or hang on
0994802 (2026-10-14 20:31-21:29), 1ab101c (22:46-23:44) and 79badd0 (2026-10-15 09:30-10:30,
with the generated magazines: M249, KSVK, M24, M9, M107 and 20-round Stanag generated in
44-84 missions each); the 20-mission subset (`tests\missions_weap.txt`) ran clean on
eabbeb3, 99b4cde, d0cdd15 and 9d545a7.

Known limits, by design or not Arma 1 specific:

- **Magazines of the same size as their carrier stay CWA's** (Stanag on M16, M240 belt on
  M60, AK on AK74); their Arma 1 names are shown. A weapon takes every magazine of its CWA
  carrier, so an M249 also loads M240 belts. Ballistics and sounds are the carrier's.
- **A scripted `fire` aims as before**: without a target it shoots into the air (a launcher
  steeply up). CWA's grenade muzzle is `Throw`: `fire ["Throw", "HandGrenade", "HandGrenade"]`;
  Arma 1's `HandGrenadeMuzzle` is mapped to it.
- A campaign saved with generated `A1x_` weapons or magazines in its pool needs them
  generated again before the pool is read (they are, when the mission's units load).
- `weapons` leaves out Throw/Put, as OFP did (`hasWeapon "Throw"` is true).
- Weapons without a CWA counterpart are left out of loadouts and crates (logged once).

## Vehicles, 2026-10-15 (f58cb9b9, tested from `dist\a1-vs18`; the game folder keeps ef440e9)

Every Arma 1 vehicle draws its Arma 1 model, but on a CWA class whose simulation, weapons,
armour, speed and crew it keeps (`Arma1ModelClass`; unlike Arma 2, Arma 1 vehicles do not
take their own speed/armour/fuel yet). The 131-mission logs of 79badd0 (`*_g.log`, lines
`'X' uses the Arma 1 model ... on CWA 'Y'`) list which CWA class carries which Arma 1 class.
Two kinds were wrong and are fixed (64e75fc7, f58cb9b9):

| Arma 1 class (missions in the run) | carried by before | now |
|---|---|---|
| `AGS` (2), `SearchLight` (3) -- static weapons, simulation "tank" | `T72`, `M1Abrams` (engine, tank gun) | `M2StaticMGE` / `M2StaticMG` by side; the searchlight without the M2 |
| `HMMWV50` (35), `UAZMG` (30), `DATSUN_PK1` (7), `HILUX_PK1` (5), `HILUX_DSHKM2` (4), `MAA_DATSUN_PK2` (3), `HILUX_PK2` (3), `HILUX_DSHKM1` (2), `DATSUN_DSHKM1` (1) -- cars whose turret gun fires bullets | the unarmed `HMMWV` / `UAZ`: a driver, no gunner, no gun | `JeepMG` (M2), with the Arma 1 seat count and the Arma 1 crew class |

The rule is data-driven: the first real weapon on a turret, its first magazine's ammo, and
that ammo's `simulation` (`shotBullet`) in the Arma 1 config; only CWA's unarmed light cars
are swapped. `OP_A1_ARMED_CARS=0` restores the old carriers. Two trunk commits the fix builds
on were taken over unchanged (38d3181a, ecbca155; Arma 2 only).

**Deviation, by design:** the gun is CWA's M2 (Browning) also where Arma 1 mounts a DShKM or
a PK, and the AGS-30 fires M2 rounds (as the D-30 and M119 already did on `M2StaticMG`).

Test mission `tests\missions\a1vehArm.Sara` (build machine), f58cb9b9 against the same build
with `OP_A1_ARMED_CARS=0`:

| check | without the fix | with it |
|---|---|---|
| `HMMWV50` / `UAZMG` / `DATSUN_PK1` weapons, gunner | `CarHorn`, no gunner | `Browning`, gunner in place to the end |
| crew of `UAZMG` / `DATSUN_PK1` | CWA `SoldierEB` driver | Arma 1 `SoldierEB` / `soldierPB` (EAST) -- the first build, 64e75fc7, had JeepMG's West crew in them; fixed in f58cb9b9 |
| 3 x `moveInCargo` into `HMMWV50` | 4 aboard (driver + 3) | 5 aboard (driver, gunner, 3) |
| scripted `fire` with v1..v4 | -- | ammo 500 -> 499 on each |
| `AGS`, `SearchLight` | -- | gunner only, speed 0, did not move |
| `HMMWVMK`, `UAZ_AGS30`, `Truck5tMG` | unchanged | unchanged, logged |

Regression: the 20-mission subset (`tests\missions_weap.txt`) ran clean on 64e75fc7 and on
f58cb9b9 (20/20, no script error, crash or hang).

Not shown by the test: the AI gunners did not open fire on their own within 20 s, with or
without `reveal` (CWA's own `JeepMG` was not run as a control). Passengers in the Arma 1
HMMWV are drawn sitting on the ground beside it -- with and without the fix, so it is the
cargo-proxy placement of Arma 1 vehicles, not this change.

Left as they are, for the owner to decide (each would change what the vehicle is):

- grenade launchers and missiles on cars: `HMMWVMK` (MK19, 9 missions), `UAZ_AGS30` (15),
  `HMMWVTOW` (8), `BRDM2_ATGM` (8) -- unarmed;
- `Truck5tMG` (M2, 13) on the unarmed truck; `BRDM2` (KPVT/PKT, 20) on CWA's unarmed BRDM;
- `Stryker_*` on `M113`, `RHIB2Turret` (MK19) on `BoatW` (M2), `KA50` on `Mi24`, `AV8B` on
  `A10`, `MH6`/`AH6` on `OH58`;
- Arma 1 vehicles keep CWA's speed, armour and fuel (Arma 2 vehicles take their own).

## Vehicle values, global scripts, .wss names (2026-10-15 evening; a317aa05 from `dist`)

| commit | what was wrong | now | checked |
|---|---|---|---|
| 7af6acf0 | Arma 1 vehicles kept the seats, top speed and fuel of their CWA carrier | `transportSoldier`, `maxSpeed`, `fuelCapacity` from the Arma 1 config, as Arma 2 vehicles already do; armour stays CWA's on purpose (CWA rounds); `OP_A1_VEHICLE_VALUES=0` keeps the CWA values | `a1vehSeats`: 12 x `moveInCargo` into an Arma 1 M113 -> 11 aboard (8 with `OP_A1_VEHICLE_VALUES=0`); UH-60 12 either way |
| fb732033 | MP missions call `timing.sqs`, `Area.sqf`, `GlobRunning.sqs` by bare name without shipping them -- they are Arma 1's own, in `ca.pbo` under `data\scripts`; 13 missions of the 131-mission run logged them not found | `FindScript` also tries `ca\data\scripts\` with `--arma1` | mp07, mp16, m10armoredfist: "script not found" 3/3/2 -> 0 |
| a317aa05 | sounds named `.wss` in configs ship as `.ogg` (mp04cleansweep: CfgSounds `sound\m04r01.wss`, archive `Sound\M04r01.ogg`): "Load failed", no radio line | with `--arma1`/`--arma2`, a missing `.wss` loads the `.ogg` of the same name | mp04cleansweep: "Load failed" 1 -> 0 |

Regression: the 20-mission subset ran clean on 7af6acf0 (all 20 logs shut down normally,
0 script errors) and on a317aa05 (20/20 exit 0, 0 script errors); a1vehArm unchanged on 7af6acf0.

### What the missions use (static census, `tools\a1census.py` on the build machine, read-only)

All 131 mission archives, 212 mission folders, 2621 script/config files (none packed):
`exec` in 183 folders, `camCreate` 161, `titleText` 151, `enableRadio` 123, `forceEnd` 109,
`playMusic` 59, `sideRadio` 54 (1116 calls), `fadeMusic` 50, `execVM` 47, `saveGame` 44,
`say` 35 (1797 calls), `playSound` 33, `setRadioMsg` 30, `setIdentity` 13, `createDialog` 11,
`setFace`/`setMimic` 6, `saveStatus`/`loadStatus` 5/3, `endMission` 4. The archives hold
3191 `.ogg` and 790 `.wss` files.

**FSMs matter little for Arma 1:** one `.fsm` file in all of them
(`armory1.intro\...\opponentAI.fsm`) and one `doFSM` call -- the Armory only. Spoken radio
and `say` conversations, music and camera work are what most missions are made of; their
timing is not covered by any test yet.

Audio problems seen in the run logs and still open: AI radio words load as
`voice\Rob\.wss` (an empty word -- Arma 1 voices against CWA's word table, 8 missions);
`x09quickupdate` asks for CfgSounds `09v50`, which no CfgSounds defines;
`m09convoyattack_s06` asks for `ca\sounds\missions\m09r06.ogg`, which is not in the AddOns.

## Rebased on the trunk, vehicle weapons, faces (2026-10-16; d3ac4c62, stamped in the game folder)

**Rebase.** The trunk `perf/optimization-20261014` had taken over the Arma 1 work up to
92ce7531 already (cherry-picked under other hashes). `MrDudeX-arma1` now is the trunk at
ed011a90 plus the commits it lacked (vehicle values, Arma 1's script folder, .wss->.ogg,
docs, radio-word diag) and the two below; the old history is kept as
`archive/MrDudeX-arma1-pre-rebase-20261015` (7adae3e3). The rebased build (12088873) was
built on the build machine (new vcpkg ports: Steam Audio, libmysofa, pffft) and boots the
Arma 1 test missions.

| commit | what was wrong | now |
|---|---|---|
| 0abfb465 | the owner decided every vehicle gets its original weapons; MK19, AGS-30, TOW, AT-5, KPVT, the gun truck and the BRDM-2 were unarmed, DShKM/PK cars fired CWA's 500-round M2 | the gunner's Arma 1 weapons (first turret with a real one) become generated `A1v_<weapon>` classes: Arma 1 name, rounds per magazine, rate of fire, full-auto, muzzle velocity, sound; on CWA's 7.62/12.7/30 mm MG, the M203 grenade, tank shells or the AT-3 by kind; the turret's number of magazines. All armed cars ride JeepMG for the gunner's seat. Cars, boats, statics; `OP_A1_VEHICLE_WEAPONS=0` turns it off |
| d3ac4c62 | Arma 1 heads (selection `osobnost`, 593 times in characters.pbo) wore CWA's face textures, laid out for OFP's head: a smeared face | Arma 1's `CfgFaces >> Man >> FaceN` (`ca\characters\hhl\hhl_NN_CO.paa`) for heads of Arma 1 models, random faces and `setFace`/`setIdentity` alike; `OP_A1_FACES=0` turns it off |

Checked on d3ac4c62 (from `dist`, then deployed):

| check | result |
|---|---|
| a1vehArm: weapons and one scripted shot each | HMMWV M2 100 -> 99 (6 magazines), UAZ DShKM 50 -> 49, Datsun PK 100 -> 99, AGS-30 29 -> 28; HMMWV MK19, UAZ AGS-30, M2 truck armed (were unarmed) |
| 20-mission subset (`_p2`) | 20/20 exit 0, 0 script errors; rebuilt weapons logged for M2StaticMG, HMMWV50, UAZMG, Stryker_ICV_M2/MK19/TOW, Truck5tMG, RHIB, RHIB2Turret, DSHKM, D30, M119, BRDM2 (KPVT + PKT), BRDM2_ATGM, LandroverMG, HMMWVTOW, HMMWVMK, UAZ_AGS30 |
| a1face against `OP_A1_FACES=0` (screenshots) | Arma 1 face on the Arma 1 head; before, CWA's texture smeared across it |
| a1vehSeats, a1weapMag | unchanged: M113 11 aboard; M24 5 / M249 200 / M107 10 rounds |

Deviations and limits, by design for now: the ammunition is CWA's (damage, splash) --
Arma 1 ammunition and Arma 1 armour are to come together (owner, 2026-10-15); a vehicle with
two gunners (RHIB2Turret's rear MK19) gets the first one's weapon only; tanks, helicopters
and planes keep CWA's weapons; the Arma 1 sounds of the rebuilt weapons were not listened to.

## Full run after the rebase, Arma 1 ballistics (2026-10-16)

**All 131 missions on d3ac4c62** (07:56-08:50, from `dist`): 130 entered and ran 25 s clean;
`fos_lament.eden` crashed (0xC0000005) in `FileCache::Maintain`, called from
`SoundSystemSteam::PreloadWave` on a pool worker -- the trunk's new Steam Audio backend
repeats the FileCache race the OpenAL backend had (fixed there in d8f2e36). 01192861 makes
it preload on the calling thread, as OpenAL does: on 01192861 the 22-mission gate (the
20-mission subset, fos_lament, s03dolores) ran 22/22 clean, and that build was deployed
(stamped 2026-10-16 09:27).

**0927bf7a, Arma 1 ammunition and armour together** (owner's decision): generated
`A1a_<ammo>` CfgAmmo classes on the CWA ammunition of the same kind take Arma 1's hit,
indirectHit and indirectHitRange; the rebuilt vehicle weapons and every Arma 1 magazine
whose round differs from the carrier's (now generated even when the size matches) fire
them; Arma 1 vehicles take their Arma 1 armour (T-72 690 vs CWA 400, M1A1 900 = 900,
UAZ/HMMWV 40 vs 20, M113 105 vs 150, UH-60 30 vs 60). `OP_A1_BALLISTICS=0` turns both off.

| check (0927bf7a) | result |
|---|---|
| a1weapAT, with / without `OP_A1_BALLISTICS` | T-72 destroyed by the M136/LAW salvo in both; M1A1 after the RPG-7V: damage 0.80 (Arma 1) vs 0.66 (CWA) |
| magazines (log) | e.g. `100Rnd_762x51_M240` on CWA's M60 (same size) fires `A1a_B_762x51_Ball`; `15Rnd_9x19_M9` on the Beretta fires `A1a_B_9x19_Ball` |
| a1vehArm, a1weapMag | unchanged (vehicle weapons fire; M24 5 rounds, M107 refuses M24 rounds) |
| 20-mission subset (`_b1`) | 20/20 exit 0 |

Crew-seat diagnostic (4b3970af), Arma 1 HMMWV M2 on JeepMG, LOD 0, model box
(-1.70 -2.51 -2.56)..(1.70 2.51 2.56): driver (-0.93 -1.58 0.28), gunner (-0.15 -1.43 -0.15),
cargo (0.53 -1.57 0.23) (0.61 -1.59 -0.70) (-0.90 -1.54 -0.71) -- the passengers sit at the
driver's height, so the seats themselves are in the car; why the passengers are drawn on
the ground is still open (their Arma 1 cargo move is the next suspect).

## Known gaps (honest list)

- Missions needing third-party addons (`un-impossible_mission.cain`: `BBB006`) cannot run.
- No fading for `ctrlSetFade` (shown/hidden on commit), no team switch, no aperture.
