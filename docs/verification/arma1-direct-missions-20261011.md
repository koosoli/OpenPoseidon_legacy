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

## Known gaps (honest list)

- `armory1.intro` needs the config script commands (`configFile >> ..`, `getText`, ...),
  which are not implemented.
- Missions needing third-party addons (`un-impossible_mission.cain`: `BBB006`) cannot run.
- No fading for `ctrlSetFade` (shown/hidden on commit), no team switch, no aperture.
