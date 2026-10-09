# OFP Fusion - Four Islands

A separate flat 102.4 x 102.4 km world, not a globe and not a replacement for the
original maps. Everon, Malden, Kolgujev and Nogova are neighbours. (Desert Island, the
menu intro island, was dropped on 2026-10-09: it was only a test island and is in no campaign.)
Original height samples, terrain semantics, ground textures and object transforms
are preserved, translated without scaling. Object IDs are reassigned uniquely.

Build PoseidonTools, then run `scripts/Install-Fusion.ps1` through the game lock.
It uses the locally installed CWA Worlds and AddOns/Noe.pbo. No retail terrain
or models are checked into this repository. Do not redistribute the generated
PBO without checking the source game data's licence. Original maps are untouched.

PowerShell launch after installation:

```powershell
Set-Location 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
.\OpenPoseidon.exe --render wgpu --window --test-mission 'Missions\FusionTour.FusionOFP'
```

Also available in Single Missions as **OFP Fusion - Four Islands**, and as a
separate editor world. The tour starts in a helicopter; its Visit actions jump
between islands within the same loaded world. Ordinary flight crosses the water
without a mission change.

## Layout (lore)

Placed as close to CWA as the engine's largest world allows (owner, 2026-10-09). North up:

```
  Nogova                          Everon
                       Kolgujev
        Malden
```

* Everon is north-east of Malden ("Everon Island, Northeast of Malden", the Flashpoint
  mission) and the easternmost island ("Everon, the easternmost island in this group",
  Blake, x01iansoffice), within helicopter range of Malden ("Choppers are waiting to ferry
  you to Everon"). It sits just north of Kolgujev, its tile 19 km from Kolgujev's like the group's
  other neighbours (19-21 km), and 68 km from Malden centre to centre. No campaign asks for open ocean round it.
* Kolgujev is east-north-east of Malden: in Planes Start the A-10s leave Malden's runway
  north-east ("heading east to Kolgujev"), and Maverick starts them east-south-east of
  Kolgujev heading west-north-west, then along the south coast. That start is due east of
  Malden's airfield here. (Maverick's caption, "5 miles North East of Kolgujev", disagrees
  with its own coordinates; Planes Start's "North East of Everon" is a slip for Malden.)
* Nogova: US forces reach it from the south / south-west and Soviet Su-25s from the
  south-east (Resistance endings), matching Malden south of it and Kolgujev south-east.
* Nogova is north-north-west of Malden (Reforger's Malden-islands map; the CWA campaigns
  only call it one of "the Malden islands"). Reforger also puts it south-south-west of
  Everon; with Everon easternmost and Kolgujev east of Malden that cannot fit at this
  scale, so Everon is east of it here.
* Kolgujev follows the 1985 campaign, not Reforger (which puts it 300 km north).
* One clue fits no arrangement: in Return to Eden the invasion helicopters reach Everon's
  north edge heading west-south-west, as if from the north-east.
* Scale: canon puts the islands 179-300 km apart, beyond the engine's 102.4 km limit. The layout is
  the first 51.2 km one scaled by 2 (bearings unchanged): 19-21 km between neighbouring island tiles
  (more from coast to coast), 34-37 km centre to centre, Malden to Everon 68 km.

## Airports and map names

`Install-Fusion.ps1` debinarises the game's `BIN\config.bin` and Nogova's config and
passes them to the builder (`--config`): every source world's airport (ILS point,
direction and taxi paths) and map names move with their island. Everon's airport is the
main one; Malden's and Nogova's are `SecondaryAirports` (worlds without SecondaryAirports keep their one airport).

Which one a plane lands at (`AirplaneAuto::Airport`, every 2 s in the air):

1. the nearest its side may use and does not know to be held by the enemy, i.e. no known enemy on the ground
   (not static, not dead) within 1.5 km of the runway's middle. This uses what the side knows, so a field taken
   unseen counts as friendly until someone reports it. Its own units need not be there (captured or empty fields
   are fine), and aircraft flying over don't count;
2. failing that, the nearest it may use; failing that, the nearest. A plane always has somewhere to come down.

In the pattern it diverts only away from a field that has turned hostile or been closed, not to one that has
become nearer. On short final it goes around first, then diverts. On the ground it uses the field it is on.
Missions decide with two commands. The airport is given by number (0 = Everon's, then Malden, Nogova) or by name
(`"Everon"`, `"Malden"`, `"Nogova"`):

    setAirportClosed ["Malden", true]      // closed to everyone; false opens it again
    setAirportSide ["Nogova", east]        // only East and its friends land there; sideLogic = anyone again

These are local, like `setFriend`. In multiplayer, run them where the AI planes are simulated (the server).
Kolgujev has no airport in the game's config. The map carries the
game's own names: 18 on Everon, 14 on Malden, 30 on Nogova (Kolgujev has none in the
original config), plus the four island labels.

The builder also merges every source world's `ReplaceObjects` (the forest blocks the engine swaps for their
walk-through versions): DefaultWorld's, Malden's and Nogova's (11 rules). With DefaultWorld's alone, Malden's and
Nogova's forests -- other models -- stayed solid to infantry and AI. And the map grid gets a third label character
(the islands' two run out at 100 squares of 128 m; Fusion is 800 across).

Ambient sounds are per island: `class SoundRegions` gives each island's tile its source world's `EnvSounds`
(engine: `Poseidon/World/EnvSoundRegions.hpp`). Each island plays its own loops; over the sea between two they
crossfade within 1 km of the halfway line. In the stock data only Malden has its own set (a quieter sea,
crickets in the trees, lighter wind and rain); Everon keeps `CfgEnvSounds`. Kolgujev and Nogova,
which use `CfgEnvSounds` in OFP, get Fusion's own sets (`ISLAND_AMBIENCE` in `build_fusion.py`, made only from CWA's
own loops that loop cleanly):

| Island | Character |
| --- | --- |
| Kolgujev | cold and windswept: rougher sea, `wind_thick` by day and a louder whistle at night, fewer birds and crickets |
| Nogova | green and sheltered: more birdsong, a calmer sea, softer wind, frogs in the meadows at night |

A mod that gives any world class its own `EnvSounds` takes precedence on the next build. Which loop plays
where (sea, trees, meadows, hills) comes from each island's own sound map, as before.

This is an experimental composite world. Loading costs exceed one original map.
Existing campaigns do not automatically migrate: absolute positions and object IDs
would need per-mission work.
Source shoreline edge samples are retained, including the tiny Nogova edge.

## Sea floor

The source maps end 20-100 m deep at the edge of their squares. Fusion used to fill the open sea at a flat 100 m,
which left a 50-80 m underwater cliff round every island, drawn as a pale outline from the air. `sea_floor` in
`build_fusion.py` now models it (owner 2026-10-09, "make the sea floors realistic"):

* Each island's own sea bed carries on outward. Its edge samples are smoothed along the edge within the first
  200 m, then it slopes to a shelf edge 110-135 m deep 3 km out.
* A continental slope runs down to a basin 200-400 m deep, 8.5 km out, with gentle relief on 2 km and 8 km
  scales. Neighbouring islands are at least 19 km apart, so their slopes never meet.
* Nogova's few edge samples above water (+3.6 m) run down like a beach instead of standing on a wall.
* The last 3 km before the world's edge rise back to 100 m deep, the engine's ground beyond the map
  (`YOutsideMap`).
* The islands' own 12.8 km squares are not touched.
The generated manifest records source hashes, offsets and object counts.

## Verified size and refinement limits

Installed 4f1bd59c rendered the earlier 51.2 km five-island Fusion world at 6.25 m terrain grid.
The acceptance run visited all islands with the player helicopter and ended cleanly;
sampled process working set was 2.2-2.3 GB. This is not a large-battle benchmark.

Two large-heightfield failures were fixed: the redundant C++ 256 MiB upload copy,
and the terrain shadow mask's invalid clamp when source dimensions exceed 4096.
The shadow mask retains its 4096 cap; source heights are not downsampled by that fix.
Automatic subdivision now respects an 8192-sample side budget (256 MiB float array).
For 102.4 km this means 12.5 m effective grid, rather than crashing while trying to
allocate a 16384-square 1 GiB array. The saved quality preference stays unchanged;
original OFP worlds keep their existing resolution. This is a refinement cap,
not an absolute map-area guarantee or support for arbitrary huge authored grids.

`scripts/Install-SizeProbe.ps1` optionally installs a separate, synthetic 102.4 km
world with three simple islands and no terrain objects. Installed 4f1bd59c passed
mission startup, height checks and vehicle/camera visits at [10000,10000],
[51200,51200], and [92000,92000], with roughly 1.7 GB sampled working set.
This does not establish dense object/AI, collision-at-every-point, campaign,
save/load or multiplayer compatibility at that size. Larger sizes are unmeasured.
View distance is separate from world size.

Run the checked-in `scripts/Test-Fusion.ps1` through the game lock for repeatable
installed tests, or pass `-Large` for the sparse size probe. Tests use a private
profile under `build/fusion/acceptance/user`, not the player's settings. Inspect
the resulting screenshots as well as the exit status and terrain-ready log check.

## Tested in game (2026-10-09, branch build on the CWA-OLI data)

Results below are from the 51.2 km five-island build unless marked.

* **Acceptance** (`scripts/Test-Fusion.ps1 -GameDir G:\CWA-OLI -Exe <branch build>`):
  * The world loads in about 3 s.
  * The terrain upload succeeds.
  * All five islands are land where expected.
  * The working set is about 2.4 GB.
  * The log reports 3 airports and 5 ambient sound regions.
* **Config fix:** the first run found that the add-on's config did not parse past `SoundRegions`
  (`Undefined base class 'CfgEnvSounds'`). That also lost the map names. The builder now declares
  `class CfgEnvSounds;` first, and the second run has no errors.
* **Airports** (`tests/AirportTest.FusionOFP`: an AI A-10 at 300 m east of Nogova, ordered to land at 3 s):
  * It lands at Nogova, the nearest field, touching down at the threshold at ~118 s and taxiing.
  * With `setAirportClosed ["Nogova", true]`, it flies 21 km to Malden and is on short final over Malden's
    runway at 235 s.
* **102.4 km, four islands** (same test, 2026-10-09, 53e379cd + these changes):
  * It loads with no errors and the working set is ~2.5 GB.
  * The terrain is 12.5 m (refinement cap) and the satmap is 4096 (25 m a texel, so the roads hold up from the air).
  * The view-distance cap is now 150 km. The test plays at 50 km and switches to 150 km (`setViewDistance`) only for
    the two overview shots, which then show all four islands. Playing at 150 km coarsens the near terrain.
  * Sea floor (below): the pale lines that traced each island's 12.8 km square from the air are gone, with no
    line at the world's edge either.
* **Enemy-held airfield** (`tests/AirportEnemyTest.FusionOFP`, 102.4 km): two East soldiers by Nogova's runway, a West
  observer who reports them, everyone holding fire. At 1 s: `obs reveal e1; a10 reveal e1`, combat mode BLUE; at
  3 s: `a10 land "LAND"`.
  * The A-10 never turns for Nogova, though it is the nearest field. It flies south-south-east for Malden from the
    start (23.1 km, 39.3 km at 169 s, still closing).
  * Without the enemy (AirportTest) the same A-10 lands at Nogova at ~118 s.
* **Map grid** (Test-Fusion, map at Kolgujev): squares past 100 carry the third character, e.g. columns `Fg0`-`Fj9`
  and rows `283`-`308`. The far zoom shows the islands with their own place names.
* **Save/load** (Test-Fusion): `triSaveGame` over Kolgujev, a teleport to Malden, then `triLoadGame`. The player is
  back 62 m from the save (the helicopter flies on) and the terrain is intact.
* **Ambient sounds** (Test-Fusion): each crossing into an island's area logs `Ambient sound region: <island>` with
  its sea and trees loops. The test requires all four. Malden plays its own crickets; Kolgujev and Nogova play
  Fusion's sets (same files as Everon's for sea and trees, different volumes and other loops). Not yet judged by ear.
* **Multiplayer** (`tests/integration/multiplayer/fusion_mp.test`, a dedicated server and two clients, run with
  `tri test --game-dir dist/x64-win-rwdi --data-dir <game dir> tests/integration/multiplayer/fusion_mp.test`):
  * Both clients join in progress and see land on all four islands.
  * Each sees the other's unit where it is, 65 km apart.
  * An AI UH-60 flies.
  * After a teleport from Malden to Nogova, the other client must see the new position.
  * **Bug found and fixed:** that last step failed. The server weights a unit's updates by `20^2 / distance^2`
    (`NetworkObject::CalculateErrorCoef`), ~10^-7 at 61 km. A far unit was never re-sent, even after an 11 km
    jump, so other players saw it frozen. Beyond 2.5 km units are now weighted as at 2.5 km, and all three roles
    pass.
  * Seen in passing, not Fusion's: both clients log `Assertion failed 'ValidateOutVehicle(vehicle)'`
    (WorldSetup.cpp) as the helicopter crew is moved out of the landscape on joining.
* **Not yet tested:** the ambient sounds by ear; a full multiplayer session (respawn, vehicles, many players).

