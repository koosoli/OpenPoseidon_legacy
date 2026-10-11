# Island Editor for Open Poseidon

The standalone island (`.wrp`) editor from Open Malprave, packed to drop into Open Poseidon.
It is a separate program: it does not need the game running and does not change the engine.

It opens 4WVR, 3WVR and binarized OPRW v2 / v3 islands and always saves 4WVR. It has land
shaping tools, texture painting and auto texturing, height map import, road / forest / bridge
builders, region copy between islands, reskins written as addons, and it shows the objects as
their real models. The user guide is `apps/tools/IslandEditor/README.md`.

## What is in the zip

| Path | What it is |
| --- | --- |
| `apps/tools/IslandEditor/*.cpp, *.hpp` | The editor (about 10,400 lines). Unchanged from Open Malprave. |
| `apps/tools/IslandEditor/shaders/` | Its five HLSL shaders (terrain, models, lines). |
| `apps/tools/IslandEditor/dx12/PoseidonD3D12/` | The Direct3D 12 device layer it draws with (`Dx12::Device`). Three files, Windows SDK only. |
| `apps/tools/IslandEditor/CMakeLists.txt` | Written for Open Poseidon's tree (see below). |
| `apps/tools/IslandEditor/CMakeLists.open-malprave.txt` | The original, for reference. Not used. |
| `apps/tools/IslandEditor/README.md` | User guide. |
| `patches/optional-terrain-cache-check.patch` | Optional engine fix, see the end. |
| `compat/report.txt` | What was compared and compiled against your tree. |
| `compat/history.txt` | The editor's commits in Open Malprave. |

## Putting it in

1. Copy `apps/tools/IslandEditor` into your tree at the same path.
2. Root `CMakeLists.txt`, next to the other tools:

   ```cmake
   if(WIN32)
       add_subdirectory(apps/tools/IslandEditor)
   endif()
   ```

3. `vcpkg.json`: the `imgui` port needs the feature `dx12-binding` as well (yours has
   `sdl3-binding`, `sdl3-renderer-binding`, `opengl3-binding`, `freetype`, `test-engine`).
   The editor uses `imgui_impl_sdl3` and `imgui_impl_dx12`.
4. Build the target `PoseidonIslandEditor`. `fxc` from the Windows SDK compiles the shaders
   at build time (the CMake file looks for it the same way our renderer does).

The program is Windows only as it stands, because it draws with Direct3D 12.

## What it needs from the engine

Only the `Poseidon` library, for reading the game's files:

- archives: `IO/PackFiles.hpp`, `IO/Streams/QStream.hpp`, `QBStream.hpp`
- islands: `World/Terrain/WrpReader.hpp` (reading; the editor has its own 4WVR writer)
- textures: `Graphics/Textures/PAADecoder.hpp`, `PAAEncoder.hpp`, `DDSConverter.hpp`,
  `Image.hpp`, `Graphics/Shared/PNGWriter.hpp`
- models: `Asset/Formats/P3D/ODOLLoader.hpp`, `MLODLoader.hpp`, `Asset/Probes/AssetInfo.hpp`
- `Foundation`: `RString`, `Array`, `Math3D`, `PlatformPaths`

It links as your `PoseidonStudio` does: `Poseidon`, `PoseidonOpenAL`, SDL3, freetype, CLI11,
imgui. It does not use your GL33 or wgpu renderers.

## What was checked against your tree, and what was not

Your folder was only read (commit `31b73f95`, 2026-10-03). Nothing was built in it.

- **Checked:** every source file of the editor compiles (syntax only, clang-cl, C++20) against
  your engine headers with no errors. Nine of the seventeen engine headers it includes differ
  between the two trees and one is missing in yours (next point); the editor compiles against
  all of yours as they are. The list is in `compat/report.txt`.
- **Checked:** `Dx12Device.cpp` compiles on its own, with no engine header and no precompiled
  header.
- **Changed for you:** our `main()` called a processor check from a header only our tree has
  (`Foundation/Platform/CpuCheck.hpp`). It is now behind `__has_include`, so the same source
  builds in both trees.
- **Not checked:** linking and running in your tree. The compile used our vcpkg headers for
  SDL3, imgui, CLI11 and stb, at the versions both trees pin (imgui 1.92.8).
- **Not checked:** your model and texture loaders behaving the same as ours on real data.
  `ODOLLoader.hpp` and `MLODLoader.hpp` differ the most between the trees.

In Open Malprave the editor passes its own tests on Everon:
`"Island Editor.exe" --selftest <wrp>` (round trip, every tool, undo) and
`"Island Editor.exe" <wrp> --game <dir> --cam 4950 190 6880 20 -18 --check-models` (22 scripted
checks of the model view, with the window hidden). Run both after your first build; they are
the quickest way to see whether your loaders give it what it expects.

## If you want it on your own renderer

Everything graphical is in two places:

- `Renderer.cpp / Renderer.hpp` (about 1,750 lines): the island view. It uses `Dx12::Device` like a
  small Direct3D 9: create texture, vertex and index buffers, set shader constants, draw.
  Terrain in chunks with an atlas of the ground textures, the sea, object models baked per
  200 m area, lines and markers.
- `main.cpp`: window and device creation, the ImGui backend calls (`ImGui_ImplDX12_*`), the
  frame loop, and reading the frame back for `--shot`.

`Island.*`, `Builders.*`, `HeightMap.*`, `Reskin.*` and most of `GameData.*` have no graphics
at all. A port replaces the device calls in `Renderer.cpp` and the backend calls in `main.cpp`;
the tools and the file handling stay as they are.

## Things you may want to rename

- Text in the window and the command line help says "Cold War Assault / Operation Flashpoint
  and Open Malprave".
- Settings are kept in `%APPDATA%\OpenMalpraveIslandEditor\settings.ini`
  (`getUserConfigDir("OpenMalpraveIslandEditor")` in `main.cpp`).
- The interface font is Segoe UI from `C:\Windows\Fonts`, with ImGui's own as the fallback.
- In Open Malprave the built program is copied to the game folder as `Island Editor.exe`;
  here it goes wherever your `dist_copy` puts it, as `PoseidonIslandEditor.exe`.

## Not there yet

Road crossings and levelling the ground under roads, a texture transition maker, and writing
OPRW. The user guide lists these under "Not there yet".

## Optional engine patch

`patches/optional-terrain-cache-check.patch` (one file, `World/Terrain/LandSave.cpp`). The
game's terrain subdivision cache is found by the island's name alone, so an edited island
keeps loading the heights of the old one until the cache is deleted. With the patch a cache
that disagrees with the island is thrown away and made again. Your tree has the same cache.
The patch was made against our `LandSave.cpp`; it was not test-applied to yours.

## Licence

The same as the tree it comes from (GPL, as Open Poseidon). No third-party code or assets are
added by this export; the Direct3D 12 layer is our own.
