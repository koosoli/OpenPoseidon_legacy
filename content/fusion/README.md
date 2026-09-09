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
without a mission change. The three southern islands run west to east: Everon,
Malden, Kolgujev. Nogova and Desert Island are to the north.

This is an experimental composite world. Loading costs exceed one original map.
Existing campaigns do not automatically migrate: absolute positions, object IDs,
location metadata and airport/ILS definitions would need per-mission work.
The map currently labels the five islands, not all original towns/airports.
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
