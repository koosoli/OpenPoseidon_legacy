---
name: ofp-blender
description: Edit Operation Flashpoint / Cold War Assault assets in Blender and bring them back to the game -- P3D models (import, edit, export as MLOD, textures as PAA) and RTM animations on the editable MC soldier rig (joint limits, collision check). Use when a task means making or changing a model, texture or animation for this engine, or driving Blender (the Blender MCP) for it.
---

# OFP assets in Blender

Everything lives in the repo's Blender add-on, `apps/tools/BlenderAddon/io_import_p3d` ("Poseidon P3D Tools"), on
top of the engine's own readers and writers in `PoseidonFormats.dll` (`engine/PoseidonFormats`). Don't write a second
P3D/RTM/PAA codec: extend the DLL's C API and `p3d_lib.py`.

## Set up (once per session)

1. Build the DLL: `.\scripts\Build.ps1 -Target PoseidonFormatsDLL` (it lands in `build\<preset>\engine\PoseidonFormats\`
   and is copied to `dist\<arch>\`; the copy fails while a Blender has the old one loaded -- use the build one then).
2. Blender 4.2+ (the owner runs 5.3 with the Blender MCP). Either install the add-on (`apps\tools\BlenderAddon\package.ps1`,
   then Install from Disk) or load it from the repo for the session -- through the Blender MCP:
   ```python
   import os, sys
   os.environ["POSEIDON_FORMATS_DLL"] = r"<repo>\build\win-x64-clang-rwdi\engine\PoseidonFormats\PoseidonFormats.dll"
   sys.path.insert(0, r"<repo>\apps\tools\BlenderAddon")
   import io_import_p3d; io_import_p3d.register()
   from io_import_p3d import p3d_lib; p3d_lib.load()
   p3d_lib.get().pf_vfs_mount(rb"D:\SteamLibrary\steamapps\common\ARMA Cold War Assault")  # read the game's PBOs
   ```
   In the UI: sidebar > Poseidon > Game Data does the mount.
3. Game data (models, textures, stock animations) is APL-SA: never commit it. Extract to the scratchpad if a file is
   needed (`PoseidonTools pbo extract <pbo> <dir> -f <name>`).

## Models

* Import: File > Import > Poseidon P3D. Each LOD is a mesh object; proxies are child objects; named selections are
  vertex groups; properties are `cwr_<name>` custom properties; the game's face flags are the `cwr_face_flags` face
  attribute; textures are materials with `cwr_texture`.
* Export: File > Export > Poseidon P3D (`export_p3d.py`) writes an MLOD (P3DM 28.256) through the engine's MLODWriter.
  The selected mesh objects are the LODs (resolution from `cwr_lod_resolution`, else 1, 2, ...); the object origin is
  the model origin. Optionally each material's image is written as a PAA (power-of-two sizes) under a texture prefix.
* Round trip is exact for `data3d\mc vojakw2.p3d` (all 11 LODs: geometry, UVs, winding, textures per face, every
  selection by position, proxies' paths and transforms, memory points), except its 12 zero-area faces in LOD 5.
  Check a change with the same comparison: load both files with `p3d_lib.P3DModel`, add each `bounding_center()`, and
  compare triangles (positions + UVs, rotation-normalised), textures per face, selections as position sets, proxies.

## Animations (the MC soldier)

* Sidebar > Poseidon > Soldier Rig > Build (skinned and textured from the mounted game data), or File > Import >
  Poseidon Soldier Rig (.p3d). Then File > Import > Poseidon Animation (.rtm), or Animations > "Load onto Soldier Rig".
  An RTM becomes an ordinary action (a key per phase) -- edit it, then File > Export > Poseidon Animation (.rtm):
  phases, step (travel per cycle, forward is -Z), and a check first.
* Check Motion / the export check: limbs as capsules (a hit when one limb's centre line reaches into another), the
  hands swapping sides, and joint limits from the full range of the 509 stock animations (+5 deg; knees and elbows one
  way; arms overhead and pointed toes allowed beyond the stock range). Importing a game move switches the limits off.
* Scripted moves: `scripts/swim/dive_moves.py` shows the pattern (key poses as channels, Catmull-Rom curves, hands and
  feet trailing, baked to every frame, joined exactly to the moves before and after).

## Conventions that cost hours to find

* Spaces: the MC rest skeleton and the soldier's own vertices are x forward, y up, z left; an RTM bone matrix takes
  that to game model space (x left, y up, forward -z). Blender: the soldier faces -Y, left +X, Z up. RTM D =
  Pg^-1 . (pose @ rest^-1) . Pr (rig.py). The importer's own mapping for plain models is (x, z, y).
* A binarized (ODOL) model keeps its points about its bounding centre: add `bounding_center()` back (the importer
  stores `cwr_bounding_center`; the exporter adds it).
* The loader turns each face round (0<->1, quads also 2<->3); the writer takes file order.
* Selection weights: the DLL hands out byte/255; the engine skins with byte/128 normalised to 1; an MLOD stores O2's
  byte (1 = full, else 256 - 255 w). Zero weight is still membership (hit zones).
* Double-sided faces are two faces on the same points: Blender cannot hold both, so the importer gives the back face
  copies of its vertices.
* Upper arms and thighs rotate YZX on the rig (with XYZ, an arm raised past the horizontal swings inward and the arms
  cross). The collarbones turn up and out under an overhead reach.
* Blender MCP area screenshots can come back stale: render to a file with the scene camera instead.

## Into the game

* Pack and stage: e.g. `scripts\Build-Swim.ps1` (the swim pack). Run your build against the game data without touching
  the game folder: `OpenPoseidon.exe --window -C "<game dir>" --mods-dir "<dist>\Mods" --mod @OP_Swim --test-mission
  <mission dir>` (`--test-mission`, not a bare mission path, starts it playing).
* Trident tests: build `tri` from the branch (`cargo build -p tri --profile rwdi`; an older one does not know
  `OpenPoseidon.exe`), `tri test <test.sqf> --game-dir <dist> --data-dir <game dir> --game-arg ...`; `triAssert` takes
  an array; wait on `time`, not frames; end with `triEndTest`.
* One game at a time on this machine: check `tasklist` first -- other agents run the game too -- and deploy only with
  `scripts\Deploy.ps1` after checking who owns the game folder (CLAUDE.md, rules 0 and 0d).

## Tests

* `python -m pytest tests/BlenderAddon --dll <PoseidonFormats.dll>` -- RTM round trips, the MLOD and PAA writers.
* `blender --background --factory-startup --python tests/BlenderAddon/blender_rig_check.py` (with
  `POSEIDON_FORMATS_DLL`) -- the rig, RTM round trip, the checks catching crossed arms and a backward knee, model export.
* Lint: ruff 0.15.6 (the add-on's `uv.lock`): `ruff check` and `ruff format` on `io_import_p3d` and `tests/BlenderAddon`.
