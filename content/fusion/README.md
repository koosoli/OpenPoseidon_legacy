# OFP Fusion - Five Islands

A separate flat 51.2 x 51.2 km world, not a globe and not a replacement for the
original maps. Everon, Malden, Kolgujev, Nogova and Desert Island are neighbours.
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

Also available in Single Missions as **OFP Fusion - Five Islands**, and as a
separate editor world. The tour starts in a helicopter; its Visit actions jump
between islands within the same loaded world. Ordinary flight crosses the water
without a mission change.

## Layout (lore)

Placed as close to CWA as the 51.2 km world allows (owner, 2026-10-09). North up:

```
  Desert Island                              Everon
  Nogova
                       Kolgujev
        Malden
```

* Everon is north-east of Malden ("Everon Island, Northeast of Malden", the Flashpoint
  mission) and the easternmost island ("Everon, the easternmost island in this group",
  Blake, x01iansoffice).
* Kolgujev is east-north-east of Malden: in Planes Start the A-10s leave Malden heading
  east / north-east, and Maverick starts them off Kolgujev's east-south-east coast flying
  north-west.
* Nogova is north-north-west of Malden (Reforger's Malden-islands map; the CWA campaigns
  only call it one of "the Malden islands"). Reforger also puts it south-south-west of
  Everon; with Everon easternmost and Kolgujev east of Malden that cannot fit at this
  scale, so Everon is east-north-east of it here.
* Kolgujev follows the 1985 campaign, not Reforger (which puts it 300 km north).
* Desert Island, the menu intro island, is in no campaign: apart in the north-west.
* Distances are compressed: canon puts the islands 179-300 km apart.

## Airports and map names

`Install-Fusion.ps1` debinarises the game's `BIN\config.bin` and Nogova's config and
passes them to the builder (`--config`): every source world's airport (ILS point,
direction and taxi paths) and map names move with their island. Everon's airport is the
main one; Malden's and Nogova's are `SecondaryAirports`, and each AI plane uses the
nearest (World::NearestAirport; worlds without SecondaryAirports keep their one airport).
Kolgujev and Desert Island have no airport in the game's config. The map carries the
game's own names: 18 on Everon, 14 on Malden, 30 on Nogova (Kolgujev and Desert Island
have none in the original config), plus the five island labels.

The builder also merges every source world's `ReplaceObjects` (the forest blocks the engine swaps for their
walk-through versions): DefaultWorld's, Malden's and Nogova's (11 rules). With DefaultWorld's alone, Malden's and
Nogova's forests -- other models -- stayed solid to infantry and AI. And the map grid gets a third label character
(the islands' two run out at 100 squares of 128 m; Fusion is 400 across).

This is an experimental composite world. Loading costs exceed one original map.
Existing campaigns do not automatically migrate: absolute positions and object IDs
would need per-mission work.
Source shoreline edge samples are retained, including the tiny Nogova edge.
The generated manifest records source hashes, offsets and object counts.

## Verified size and refinement limits

Installed 4f1bd59c renders the five-island Fusion world at 6.25 m terrain grid.
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
