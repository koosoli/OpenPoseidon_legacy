# DZ-001 — DayZ is an Arma 2 descendant, not an Arma 3 one, and its assets and both its worlds read

**Date:** 2026-08-14
**Status:** Measured, and implemented. Models: whole-corpus closure. Worlds: both shipped worlds parse to the byte.
**Corpus:** the owner's retail DayZ install, `D:\SteamLibrary\steamapps\common\DayZ` — 133 PBOs, 109,853 entries.
**Licence note:** DayZ data is proprietary. Nothing from it is vendored here, no fixture is committed,
and every byte read during this work lived in a session scratchpad and was deleted. The harness points
at the install by path, the way `.trident.env` points at game data.

## The claim

DayZ's *asset containers* did not follow Arma 3. They followed Arma 2 and then diverged on their own
line. Concretely:

* **P3D is ODOL revision 54**, and revision 54 belongs to the `Odol49.hpp` A2 family (48/49/50/52),
  not to the revision-73 family. Its prologue carries no `appId` and no muzzle-flash string — the two
  fields Arma's `version >= 44` / `>= 51` gates put there — and it has none of revision 73's three
  per-LOD index arrays.
* **The world is OPRW revision 29**, and it reads through the same later-generation profile as
  revisions 18/20/24/25 with four small additions.
* What DayZ *did* replace is the runtime, and none of that is Arma-shaped. See "Out of scope" below.

## What was measured

### Format census (whole install, 0 read errors)

| Format | Finding |
|---|---|
| PBO | classic `Vers`/`Prod` header; **100% of 109,853 entries stored uncompressed** (packing method 0) |
| P3D | **ODOL v54, 8,203 models, no other magic or revision anywhere** |
| PAA | `FF01` DXT1 ×18,249, `FF05` DXT5 ×9,947, plus 6 legacy — no new type |
| RVMAT | rapified, 31,045 files, all material version 20 |
| WRP | **OPRW v29 ×2** (ChernarusPlus, enoch) |
| RTM | **none** — DayZ has no RTM at all |

### ODOL 54: the model delta

Two deltas against the A2-family reader, and nothing else.

**1. The directory widens by six bytes** — five bools where A2 writes four, seven floats where it
writes six, and thirteen filler bytes where it writes twelve.

This was fitted, not guessed. The LOD byte-offset table can be located independently of any layout
knowledge, because almost nothing else in a P3D satisfies its constraint: two arrays of `lodCount`
u32 whose intervals tile the tail of the file exactly once and end at EOF. Locating it first gives
ground truth; enumerating field-count variants against it gives the layout. The winning variant hit
the table exactly on **199 of 200** models; every rival was below 120.

The animation block needed no new case: revision 54 sits below both of its gates (55's hide value,
56's period/phase pair), and testing all four gate combinations gave **400/400 exact** with both off
and at most 399 otherwise.

Closure over the whole corpus, reading directory → skeleton → animations → LOD table → per-LOD
trailer and requiring the reader to land exactly on the first byte of LOD data:

```
models            8203
directory closed  8203  (100.000%)
LOD ranges tile   8203  (100.000%)
with skeleton     1405
animated           661
failures             0
```

**2. Material version 20 widens by 104 bytes** — eight colour quads where every Arma revision writes
six (+32), and eighteen further words between the specular power and the shader ids (+72).

Also fitted rather than guessed, and against an unusually good oracle: *the embedded material names
its own source `.rvmat`*, so the rvmat is independent ground truth. Across 300 cross-checked
materials the specular power always lands at +128 (eight quads, not six) and the surface path always
at +220. Reading it this way makes the shader ids resolve to the classic enum values their rvmat
names imply, with no name ever mapping to two numbers:

| rvmat name | PixelShaderID | | rvmat name | VertexShaderID |
|---|---|---|---|---|
| `Normal` | 0 | | `Basic` | 0 |
| `Super` | 102 | | `Super` | 23 |
| `NormalMapSpecularMap` | 18 | | `NormalMap` | 1 |

Everything after that is revision 73's material layout unchanged, including the four words after the
surface (`[word][renderFlags][nStages][nTexGens]`), the `[filter][asciiz][stageId][useWorldEnvMap]`
stage record, the 52-byte texGen, and the thermal stage at material version >= 10. The stage array
begins with an implicit empty stage 0, so `nStages` is the rvmat's stage count plus one.

The gate is `material.version >= 12`, not `== 20`, because the two populations are far apart: every
Arma material observed is <= 11 and every DayZ one is exactly 20. Nothing in between has been seen,
so **where the change actually landed is untested** — the gate only has to separate two known families.

### Result: every DayZ model loads

Through the real C++ reader, whole corpus, batched so the scratchpad never held more than one batch:

```
models  8203
loaded  8203  (100.000%)
failed     0
```

Before this work the same corpus loaded 0. The intermediate state is worth recording because it is
the shape these things take: once the directory was right but the material was not, a 122-model
sample went 4 → 122, and all 118 failures carried one identical cause.

### OPRW 29: the world delta

Four additions, each measured:

1. **Header.** The `appId` slot is not a number, it is the FourCC `ENF0` — the Enfusion marker —
   followed by a further word and a single byte. Reading it that way yields `landRange` 256×256 and
   `terrainRange` 2048×2048 for both worlds, with cell sizes 60 m and 50 m, i.e. **15360 m for
   ChernarusPlus and 12800 m for Enoch** — the documented sizes of both maps. Off by one byte none of
   that lands.
2. **Static entity** carries one extra word after `objectId`. Identified positionally: walking
   Chernarus's table, `objectId` increments from record to record while this word holds the same
   value throughout.
3. **Road link** carries one extra word after `objectId`, in the same relative place.
4. Nothing else. Every other gate (`lzo >= 23`, `randomization < 24`, `primaryTextureIndex >= 24`,
   `roadConnectionTypes >= 24`) already resolves correctly for 29.

The strongest single check is that the file's own byte accounting closes: for ChernarusPlus,
`roadNetStart 20,784,587 + roadNet 6,785,947 + objectTable 178,273,080 + mapInfo 17,810,372`
= **223,653,986 = the file size exactly**.

Both worlds then census clean:

| | ChernarusPlus | enoch (Livonia) |
|---|---|---|
| Extent | 15360 m square | 12800 m square |
| Elevation | −231.8 .. 550.4 m | 145.5 .. 614.1 m |
| Terrain cells below sea level | 977,567 | **0** |
| Objects | 2,971,218 | 2,932,069 |
| Model table / referenced / unused | 2352 / 2352 / 0 | 2484 / 2484 / 0 |
| Static entities | 18,452 (423 classes) | 8,518 (332 classes) |
| Road links | 51,959 | 36,263 |
| Surface materials declared / used | 7193 / 7192 | 9432 / 9431 |

Two of those are external checks rather than internal consistency: Livonia is landlocked, and it is
the world that reports zero cells below sea level; Chernarus has a coastline and reports 977,567.
And in both worlds every model in the table is actually referenced.

## Out of scope, and why

DayZ kept the RV-lineage *containers* and replaced the *runtime*. None of the following is
Arma-shaped, and none of it is addressed here:

| What | Count | Note |
|---|---|---|
| `.c` Enforce Script | 2,816 | not SQF; a different language and VM |
| `.anm` animation | 8,483 | `FORM`/`ANIMSET5` IFF container; RTM is gone entirely |
| `.emat` Enfusion material | 207 | used by particles/postprocess/water |
| `.edds` / `.dds` | 553 | plain DDS containers |
| `.ptc` particles, `.layout` GUI | 498 | Enfusion authoring formats |
| `.pso` / `.vso` / `.cso` | 1,561 | precompiled DXBC, DX11 |

So "DayZ compatibility" means *asset* compatibility: models, textures, materials, worlds. Gameplay
is a different engine.

## Engine changes

* `Asset/Formats/P3D/Odol49.hpp` — revision-gated widening; `ReadOdol54StaticModel`.
* `Asset/Formats/P3D/Odol73.hpp` — material version >= 12 widening in `ReadOdol73EmbeddedMaterials`.
* `Asset/Formats/P3D/OdolRevision.hpp` — revision 54 in the table, generation "DayZ".
* `Asset/Formats/P3D/ODOLLoader.hpp` — `loadOdol54Static` and its dispatch.
* `Asset/Formats/World/Oprw25.hpp` — revision 29 accepted; header, static-entity and road-link deltas.
* `World/Model/ModelCache.{hpp,cpp}` — see below.

## Getting it into the engine: two defects that were not about formats

Reading the files is not the same as loading the world. Substituting ChernarusPlus
into the runtime with `--test-world` + `--addon-root` found two more, both of which
had nothing to do with parsing and everything to do with assumptions:

**1. The addon product allow-list silently refused every DayZ archive.**
`CheckProductCallback` refuses any PBO whose `product` property is outside a fixed
list, and refuses it *silently*. DayZ declares `product = "dayz"` (99 archives) or
`"dayz bliss"` (24), so all 117 were dropped, and the symptom was not "no addons" —
it was a world that loaded and then reported every one of its 2,352 models as
"file not found". The comment above that list already records Arma 3 hitting this
exact trap. Adding the two names is what made models and materials resolve.

**2. Object ids are sparse handles, and two containers were indexed by them.**
`Landscape::_objectIds` was an `AutoArray` addressed by object id, as were the
`idCache`/`conflicts` pair inside the OPRW placement loop. Every world up to Arma 3
numbers objects densely from zero, so that held. DayZ does not: Chernarus's ids run
**0x01000000 .. 654,367,047** for 2,971,218 objects, so `Access(id)` tried to grow
one element per id — 5.2 GB to hold 23 MB of pointers — and took the process down
inside `Realloc`. Both are now keyed rather than indexed, which also removes a
latent O(largest id) allocation for every other generation.

The duplicate-id fixup that followed `idCache` derived its next free id from the
container's *size*; with a map that is meaningless, so it now continues past the
highest id actually seen.

## One thing that was not about DayZ

`ModelCache::loadFromFile` ended in `catch (...) { return nullptr; }`. Every load failure — an
unsupported revision, a malformed file, a missing file — reached the caller as an indistinguishable
null, and the readers' messages, which name the exact constraint a file broke, were discarded. That
is the diagnostic entry point for *all four* compatibility layers, and it was empty.

It now records the reason, `UnsupportedOdolRevision` is caught separately from a malformed file
(the two call for different follow-up), and `InspectModel` and `PoseidonTools model inspect` surface
it. This is what turned

```
Error: Failed to load ...
```

into

```
ODOL revision 54 is not supported by this build for this model
(recognised: DayZ; ... : ODOL 73 material stage count is invalid)
```

which is the line that located the material delta. Without it this work would have been a hex editor
and a guess.

## It renders

Chernarus draws in the engine. Freefly at 6500/2700, 400 m, noon, `--render gl33`: Chernogorsk's
panelak blocks, the road network with its lane markings, trees and bushes, and forested hills behind
— all DayZ models on DayZ terrain. Measured rather than eyeballed alone: 529,038 of 921,600 pixels
non-black, mean luminance 88.

Two things that frame does *not* show, and one that did not work at all:

* **The ground is white.** Terrain surface materials are read (7,192 of 7,193 satellite/mask blends
  bound, 27,855 surfaces) but are not textured on the GL33 path. GL33 is the fallback backend and
  parity with it is explicitly not chased, so this is a WGPU question, not a GL33 one.
* **Some model sections draw untextured** — 45 of them in that run, plus 1,035 materials with no
  albedo slot falling back to their face texture.
* **WGPU draws pure black.** Every pixel exactly 0, at every camera position tried. The control
  proves it is this path and not the rig: the same recipe on the stock OFP mission with no DayZ
  renders normally (mean 168, 1.43 M non-black), and produces **zero** uncaptured wgpu errors, while
  each DayZ run produces 60-70. The dominant one, 57 of 64:

  ```
  In Device::create_bind_group, label = 'wgr_bindless_texture_bind'
    TextureView with '' label is invalid
  ```

  One invalid view poisons the whole bindless array, so every object draw after it fails. A second,
  rarer one invalidates `wgr_sky_bake_layer` inside `Queue::submit`, which explains why even the sky
  is absent.

  **This is not attributed yet.** The working tree carries another session's uncommitted renderer
  changes (`EngineWgpu.cpp/.hpp`, `wgpu_renderer.hpp`, `rust/src/ffi.rs`, `rust/src/godrays.rs`), and
  those are compiled into the DLL these captures ran against. Whether DayZ's texture volume exposes a
  pre-existing hole in the bindless slot lifetime or interacts with that in-flight work cannot be
  told apart without a clean tree. Reproduce on a clean checkout before treating it as a DayZ bug.

## What is not claimed

* **A playable game.** The world draws; nothing else about DayZ works. See "Out of scope".
* **WGPU rendering.** See above -- the target backend is black, with a named and reproducible cause.
* **Object streaming performance.** Chernarus loads at roughly 2 fps for several minutes and reaches
  ~4-5 GB resident. 2,971,218 placements is 18x Stratis, and nothing here was tuned for it.
* **The material-version boundary.** Gated at >= 12, observed only at 20. See above.
* **Map info.** ChernarusPlus retains 17,810,372 bytes of trailing map-info undecoded, exactly as the
  Arma 3 worlds do.
* **Sakhal.** The DLC world was not present as a `.wrp` in the install scanned; only ChernarusPlus and
  enoch were.
