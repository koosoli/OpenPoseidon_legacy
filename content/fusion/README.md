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
.\OpenPoseidon.exe --render wgpu --window --test-mission '.\Missions\FusionTour.FusionOFP'
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
