# Showcase Lab

A small, playable single-player test campus on Desert (`Intro`), not a new
terrain/WRP. Inspired by the requested first-person physics test-room format:
https://www.youtube.com/watch?v=cqaBAfGu7-k . This is original geometry and
procedural texture artwork, not extracted video assets or a copied level.

## Start

Open **OpenPoseidon.exe**, then **Single Mission > Showcase Lab - Physics and Light**.
Use the `LAB:` entries in the player's action menu. The Help entry describes the
stations. Dev Tools' Physics and Weather tabs remain available for deeper tuning.

- Ahead: 24 stacked boxes and 18 dominoes. Shoot them, push them, or throw a body.
  These are solid diagnostic meshes; the gallery provides textured comparisons.
- Right: sheltered material gallery, labelled FLAT / NORMAL / GLOSS. Same planar
  geometry and grid albedo below the labels; different normal/specular inputs.
- Behind: a driveable Jeep, Ural and UH60 for existing vehicle/weather features.
- Light mode clears the bodies, enables warm moving point lights, and limits the
  mission actions to four bodies. Drop places a light two metres ahead of the
  camera; ordinary body mode uses the existing terrain-crosshair placement.
- Cool colour and day/night actions offer quick comparisons. Reset restores the
  initial 42-body field and exits light mode. Clear removes the probes.

The map uses real existing lighting, normal maps, shadows and Box3D; it does not
add a parallel demo renderer. Particle, cloud and experimental snow controls are
the engine's existing Weather/Physics controls, not new simulation systems.

## Build And Install

Build/deploy the matched game pair first. From this worktree, in Git Bash:

```sh
LOCK_OWNER=showcase-install scripts/with-game-lock.sh powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Install-Showcase.ps1
LOCK_OWNER=showcase-test scripts/with-game-lock.sh powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Test-Showcase.ps1 -Menu
```

The installer produces `build/showcase/showcase_lab.pbo` with Python's standard
library, installs only `Addons/showcase_lab.pbo` and `Missions/ShowcaseLab.Intro`,
and leaves user profiles and stock missions alone. The smoke test uses its own
profile under `build/showcase/smoke/user`; screenshots/logs stay in that directory.
`python scripts/showcase/test_showcase.py` checks deterministic packing and assets.

## Limits

This is an experimental **single-player sandbox**. Probe bodies are diagnostic
objects, not mission entities: no multiplayer replication or save-game restore
of the stacks. Restart/reset instead of relying on a saved probe arrangement.
The stationary gallery has authored Geometry, View Geometry and Fire Geometry
LODs. Vehicles still use the normal gameplay simulation; this scene does not
silently transfer them to Box3D authority.

The four-light/128-body limit applies to mission actions. The general-purpose
Physics tab is deliberately still a developer tool and can exceed that budget.
There is no continuous spawner, custom ocean, destructive terrain or imported
Esoterica combat system. The existing snow system keeps its documented limits.
