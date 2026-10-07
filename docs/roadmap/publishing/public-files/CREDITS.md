# Credits

*Open Poseidon Engine* (community engine continuation, codename **Poseidon**) exists thanks
to the people and projects below.

## Bohemia Interactive

Developed and published by **[Bohemia Interactive](https://www.bohemia.net/)**.

The engine and game began life as **Operation Flashpoint: Cold War Crisis** (2001),
later re-released as **Arma: Cold War Assault**. This source release, and the
decision to make it available to the community under the GNU GPL, are the work of
Bohemia Interactive and the studio's original and ongoing development teams.

Our thanks to the original Operation Flashpoint / Cold War Crisis team, whose work
on the **Poseidon** engine — the foundation of Bohemia's later *Real Virtuality*
and *Enfusion* technology — is what you are reading here.

## Community engine lineage

Open Poseidon Engine is based on **[paavohuhtala/CWR-CE](https://github.com/paavohuhtala/CWR-CE)**,
Paavo Huhtala's fork of **[ofpisnotdead-com/CWR-CE](https://github.com/ofpisnotdead-com/CWR-CE)**,
the community continuation maintained by **Retro** and its contributors. That
project builds on Bohemia Interactive's original Poseidon/CWR source release.

We thank Paavo Huhtala, Retro and all upstream contributors for the engine work
on which this fork builds. This lineage credit complements the individual
ported-code and dependency credits below.

## The community

Thanks to the *Operation Flashpoint* / *Arma* modding and content-creation
community, who have kept this game alive for more than two decades and for whom this
release is intended.

Special thanks to Josef Šimánek, "simi", for development archaeology, codebase work, and preparation of this source release.

Thanks to the community testers:

- Petr "Killer14" Švarc
- Vojtěch "Hammer" Zatloukal
- Tony "TonyHawk" Bako
- Marek "Topal12" Lehečka
- Dr. Alex Mercer
- Inlesco
- t
- Foie
- Vojtěch "snw" Chaloupka
- Spez

The community continuation welcomes pull requests, ideas, fixes, ports, tests,
and documentation work in this repository.

## Third-party software

This project builds on open-source work. Per-component licenses are listed
in [`thirdparty/README.md`](thirdparty/README.md), and dependencies are declared in
[`vcpkg.json`](vcpkg.json). With thanks to the authors and maintainers of:

**Vendored in `thirdparty/`**

- **glad** — OpenGL function loader (David Herberth and contributors).
- **RenderDoc** in-application API header (Baldur Karlsson).
- **Khronos** `khrplatform.h` (The Khronos Group).

**Resolved via vcpkg**

- **SDL3** — windowing, input, and platform abstraction (Sam Lantinga and the SDL team).
- **OpenAL Soft** — audio backend (Chris "kcat" Robinson and contributors).
- **Opus**, **libogg**, **libvorbis** — audio codecs (the Xiph.Org Foundation).
- **FreeType** — font rasterisation (David Turner, Robert Wilhelm, Werner Lemberg, and the FreeType Project).
- **curl** — networking/transfers (Daniel Stenberg and contributors).
- **cJSON** — JSON parsing (Dave Gamble and contributors).
- **CLI11** — command-line parsing (University of Cincinnati and contributors).
- **stb** — single-file utility headers (Sean Barrett).
- **mimalloc** — allocator (Microsoft / Daan Leijen).
- **enkiTS** — task scheduler (Doug Binks).
- **Dear ImGui** — developer/debug UI (Omar Cornut and contributors).
- **spdlog** — logging (Gabi Melman and contributors).

## Embedded algorithms

- **ISAAC** pseudo-random number generator by **Bob Jenkins** (public domain),
  with the `QTIsaac` C++ template implementation by **Quinn Tyler Jackson**.
- **RANMAR** random number generator C++ port by **Phil Linttell**, kept for
  legacy network compatibility.
- **CRC-32** table generation follows the implementation described in the
  `comp.compression` FAQ.
- Various numerical and fast-math routines in `engine/Poseidon/Foundation/Math`
  adapted from publicly published references.

---

*"ARMA" is a registered trademark of BOHEMIA INTERACTIVE a.s. "OPERATION FLASHPOINT" is a registered trademark of Electronic Arts Inc.
See [`LICENSE`](LICENSE) for information concerning trademarks. This credits file is
informational and does not constitute any grant and/or waiver of rights.*

## Ported code

### Malprave (Dec's Poseidon fork)

The Sinkhole workstream (the development decision records) ports engine code from
*Malprave*, Dec's GPL fork of the same Poseidon source, with no shared git history. Each
port is listed here and in its decision record, with what was rewritten for this tree.

| Open Poseidon Engine change | Ported from (Malprave patch) |
| --- | --- |
| DDS masked uncompressed layouts, DXT2/DXT4, DXT1 punch-through (`DDSConverter.cpp`, `DdsImport.cpp`) | `ddsTextures.patch` |
| Object-cut terrain holes: `TerrainHoles.cpp` and the collision, LOS, explosion, AI, camera, sound, decal and GL33 hooks (wgpu drawing rewritten) | `terrainHole1`..`terrainHole8.patch` |
| Swimming engine, sea routes, boat boarding/exits and swimmer movement (wave following and drawn-water bridge adapted for this renderer; animation addon not included) | `swimming`, `swimBoatsAI`, `swimMoves`, `swimTurn`, `swimRoutes`, `swimAvoid`, `boatExits`; see `SINKHOLE-03-swimming-20261004.md` |
| Infantry hearing/report/order/suppression fixes, convoy/tank/aircraft/parachutist fixes, and optional spotting/hunting/exposure mechanisms | Malprave, a GPL-3.0-or-later fork of the same Poseidon upstream, as confirmed by the project owner. Selected behavior ideas were inspired by OpenXRay; no STALKER assets are included. |
| Diagnostics layer: `--diag` event log, `diag_*` harness commands, overlay (`engine/Poseidon/Dev/Diag/OpDiag*`, hooks in World / Shots / Collisions / Dammage / VehicleAICombat / SoldierOldMove / ExpressExt / SoundScene / VehicleTypes / OptionsUI / DebugOverlay / CrashHandler); Cry of Fear parts stripped, pause / step / camera / overlay rewritten for this tree (the development decision records) | `engine/Poseidon/Dev/Diag/MwDiag.cpp`, `MwDiag2.cpp`, `MwDiag7.cpp`, `MwDiag9.cpp`, `MwDiagHarness.cpp`, `MwDiagHarness2.cpp`, `MwDiagOverlay.cpp` (mwDiag) |

### Tidewater (dgreenheck/tidewater)

The *Tidewater Native* water backend (`engine/WgpuRenderer/rust/src/water_tw/`) is a
source-faithful port of parts of [Tidewater](https://github.com/dgreenheck/tidewater),
commit `4811ba48d795197de5621985f404e765c0b7c0ef`, MIT-licensed. Only code was ported. The
foam and sea-detail textures are generated procedurally, and no Tidewater asset (audio,
models, textures, cloud noise files) is used.

| Open Poseidon Engine file | Ported from (Tidewater @ 4811ba48) |
| --- | --- |
| `water_tw/fft.rs` | `src/ocean/OceanFFT.js` |
| `water_tw/cdlod.rs` | `src/core/CDLOD.js` |
| `water_tw/textures.rs` | `src/ocean/SeaDetail.js` (`makeNoiseTexture`), `src/ocean/FoamTexture.js`, `src/util/Noise.js` (`mulberry32`) |
| `water_tw/tw_water.wgsl` | `src/ocean/WaterSurface.js`, `src/ocean/SeaDetail.js`, `src/ocean/WaterMaterial.js`, `src/core/CDLOD.js` |
| `water_tw/shore_field.rs` | `src/world/ShoreField.js` |
| `water_tw/tw_shore.wgsl` | `src/ocean/ShoreWaves.js`, `src/world/TerrainGPU.js` (`terrainShoreSample`), `src/engine/render/wgsl/common.js` (`perlin2`) |
| `water_tw/shore_sim.rs`, `water_tw/tw_sim.wgsl`, `water_tw/tw_sim_kernel.wgsl` | `src/ocean/ShoreSim.js` |
| `water_tw/lace.rs` | `src/ocean/SurfFoam.js` (`makeLaceTexture`, `laceData`) |
| `water_tw/tw_surf_foam.wgsl` | `src/ocean/SurfFoam.js` (the `SurfFoam` WGSL module) |
| `water_tw/breakers.rs`, `water_tw/tw_breakers_kernel.wgsl`, `water_tw/tw_lip.wgsl` | `src/ocean/Breakers.js` (crest finder, lip mesh and material, `buildStations`) |
| `terrain/shore_wet.rs`, the TW-WATER block of `terrain/terrain.wgsl` | `src/App.js` (`terrainWetness`), `src/world/Terrain.js` (wet sand, foam residue), `src/ocean/ShoreSim.js` (`shoreSimSandFoam`) |
| `water_tw/spray.rs`, `water_tw/tw_spray_common.wgsl`, `water_tw/tw_spray_emit.wgsl`, `water_tw/tw_spray_update.wgsl`, `water_tw/tw_spray.wgsl` | `src/fx/Spray.js` (particle ring, emitter API, update, sprite material), `src/ocean/Breakers.js` (`_emitCode`, `breakersSprayShadow`) |
| `water_tw/underwater.rs`, `water_tw/tw_underwater.wgsl` | `src/post/Underwater.js` (`underwaterMedium`, `underwaterComposite`) |
| `water_tw/caustics.rs`, `water_tw/tw_caustics_splat.wgsl`, `water_tw/tw_caustics.wgsl` | `src/ocean/Caustics.js` (`CausticLayer`, `_causticsSample`) |
| `water_tw/wake.rs`, `water_tw/tw_wake_kernel.wgsl`, `water_tw/tw_wake.wgsl` | `src/ocean/WakeSim.js` |
| `water_tw/boat_spray.rs` (with the boat bodies and CPU-emitted tail in `spray.rs` / `tw_spray_update.wgsl`) | `src/player/BoatSpray.js` — its contact model is noted in the source as being "after threejs-water-pro", a product by the same author; it is used here under Tidewater's MIT grant on the repository owner's decision |

Written for Open Poseidon Engine, with no Tidewater counterpart (not ported; listed so the table above
reads as complete): `water_tw/impacts.rs` and `water_tw/tw_impact.wgsl` (shells and bullets on
the water, TW-WATER W8b), `water_tw/probe.rs` and `water_tw/tw_probe.wgsl` (the drawn surface
around the camera for the underwater split, W9a/W12d), `water_tw/tw_common.wgsl` and the
backend plumbing in `water_tw/mod.rs` / `water_backend.rs`.

```
MIT License

Copyright (c) 2026 DRG Software Solutions LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## CWR-Physical Inventory

The graphical single-player inventory, grid model, item footprints and carrying-mass
helpers are adapted from the author-supplied **CWR-Physical Inventory** GPL source
donation received by the project owner in October 2026. The supplied licence is
GPL-3.0-or-later with this project's same Section 7 terms. Changes include current
engine integration, safe transfers, user-directory settings and built-in UI/icon
fallbacks. No donated binaries or separate game/icon assets are bundled.

The 17 default inventory icons are original AI-generated equipment illustrations,
created from written descriptions without donor or retail-game image inputs.
Source PNGs and runtime PAAs are versioned under assets/inventory; prompts and
hashes are recorded in content/inventory-icons. They use the project licence.
