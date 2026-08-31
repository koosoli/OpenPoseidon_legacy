# MAT-054 — Everon's white blocks are colour-only `.emat`s the converter refuses to deploy

**Date:** 2026-08-31
**Status:** diagnosed; engine contract tested; converter fix specified, NOT applied (out of this
session's write scope) and the deployed corpus is stale besides.
**Reported as:** a cluster of objects rendering as untextured WHITE blocks from ~200 m altitude at
~1 km distance, on the substituted Reforger Everon world.

## Symptom, and the mechanism that makes it WHITE

The renderer already names the failure. `EngineWgpu.cpp:5597` logs, once per (model, rvmat):

```
Wgpu material: section draws UNTEXTURED (fallback=none) model=… lod=… section=… rvmat=… untexturedRetainedSections=N
```

`fallback=none` is not "no material was found". It is `UntexturedAlbedoHandle` returning 0, and the
run says why in one line of its own:

```
untextured-albedo correction is OFF -- those sections keep the 1.0 white at bindless slot 0
(WGR_UNTEXTURED_ALBEDO=<unset>)
```

That is MAT-050's deliberate opt-in default. So the colour on screen is 1.0 white, which is the
brightest value the format has, applied to sections whose authored colours are **0.016 to 0.32**.
The white is the maximum possible error, and it is why a 4 m crash barrier reads as a block.

## What is established (not re-derived here)

Not residency (identical with eviction disabled, at 5,611 MB with zero evictions, and at 900 MB
with 60); not far-tier stand-ins (`WGR_FAR_TIER=0` changes nothing); not this branch (`ba26ba05`
shows them). The `draws UNTEXTURED` count is 70 in every arm.

## The 70, enumerated

70 (model, section) pairs over **40 distinct materials** and 62 models, all Reforger Everon, from
`p7-default.log`. Every one of the 40 was read out of the source `.pak` corpus
(`C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons`, inheritance folded):

| class | count | has a texture map | has `Color` | what it is |
|---|---|---|---|---|
| `MatPBRDecal` | 25 | no | 8 of 25 | LOD-0 overlay shells: welds, graffiti, sign text, `*_Decals.emat` |
| `MatPBRBasic` | 12 | **no** | **12 of 12** | **coarse-LOD baked materials — the white blocks** |
| `MatPBRBasicGlass` | 1 | no | yes | `Ashtray_01_glass_clear` |
| `MatPBRBasic` | 1 | **yes** (`BCRMap`) | no | `Common/Materials/Default.emat` |
| `MatTerrainObject` | 1 | no | no | `Common/Materials/default_MatTerrainObj.emat` |

The 12 are the answer to the reported symptom, and they are a coherent family — Reforger's
**baked-LOD** materials, named `*_MLOD.emat`, `*_lod3.emat`, `*_Color_palette.emat`. They appear at
`lod=2..6, section=0`, i.e. as the *whole object* at distance, which is exactly the viewing
condition reported. The `MatPBRDecal` 25 appear at `lod=0, section>=1` — specks, close up, and a
different problem (below).

The same model draws textured near and untextured far. `cratewooden_01.p3d` is the clean case:
`lod=0` binds its real albedo; `lod=4 section=0` names `CrateWooden_Plain_MLOD.emat` and binds
nothing.

Verbatim, from `data005.pak`:

```
Assets/Structures/Walls/Crashbarrier/data/CrashBarrier_MLOD.emat
MatPBRBasic {
 Color 0.168 0.185 0.188 1
 Cull none
 SurfaceProperties "{CE9253778DD8FBDE}Common/Materials/Game/metal.gamemat"
 RoughnessScale 0.48
 MetalnessScale 0.838
}
```

No `BCRMap`, no `BCR_N`, no map of any kind. Its LOD-0 sibling `CrashBarrier.emat` is a full
`MatPBR2Layers` with five maps and deploys correctly. The other eleven are the same shape:
`HaystackSmall_01_MLOD` 0.095 0.083 0.035, `CrateWooden_Plain_MLOD` 0.105 0.078 0.053,
`HouseRuin_01_BrickPile_MLOD` 0.051 0.042 0.032, `Wheele_tire_lod3` 0.016 0.016 0.016,
`Tire_BTR70_Color_palette` 0.032 0.026 0.022, `SnowBreaker_E_01_Stack_MLOD` 0.084 0.076 0.063,
`FootballNetFence_01_Net` 0.177 0.195 0.13, and so on.

**This is not an asset gap.** The source is complete and unambiguous: the material *is* a flat
colour, which is how Enfusion draws a baked LOD.

## It is (b): the converter drops them, and the engine was never given the chance

Two facts settle it.

**One — all 40 are absent from the deployed tree.** Checked file by file against
`…\ARMA Cold War Assault\reforger\everon\textures\`: `present=0 missing=40`. Meanwhile the same log
resolves 2,329 materials under `reforger\everon\textures\…`. The failing sections name the raw pak
path (`Assets/Structures/Walls/Crashbarrier/data/CrashBarrier_MLOD.emat`) because
`BakedMaterial::MaterialField()` fell through to the source path when both `ematPath` and
`rvmatPath` were empty. That path cannot be opened at runtime — there is no `.pak` beside the game.

**Two — `TextureBaker::Bake` refuses on "no image", not on "nothing bindable".**
`XobCommand.cpp:1220` takes the material's absence of `BCRMap`/`BCR_N` as terminal:

```cpp
Unbound("material names no BCRMap and no BCR_N layer");
```

and returns an empty `BakedMaterial`. The deploy is then gated on the image, not the material
(`XobCommand.cpp:1304`):

```cpp
if (_options.emats && !baked.colourPaa.empty())
    baked.ematPath = WriteEmat(...);
```

with the reasoning in the comment above it — "a material with nothing bindable in it would only be
an empty file the engine opens, translates, and reports as binding nothing." **That premise is
false for this family.** `EmatMaterialAdapter.hpp:495` already synthesises a base colour from
`Color` when no albedo slot exists:

```cpp
if (hasColour && !out.Find(MaterialSlot::BaseColour))
    // "#(argb,8,8,3)color(r,g,b,a,CO)"
```

which is Real Virtuality's own notation for a flat stage, and `ResolveMaterialBaseColour` binds it
(the `baseFromConstant` path in `EngineWgpu`). So the engine has handled colour-only materials
since DZ-002. It never sees these because the file is not on disk.

## The `MatPBRDecal` 25 are stale data, not a second bug

`hideFaces` — drop the faces of an unresolvable decal shell rather than draw them opaque over the
surface they decorate — landed in `1dee99c7`, **2026-08-26**. The deployed models are dated
**2026-08-23** (`crashbarrier_01_4m_v1.p3d` 18:06, `containermetal_01.p3d` 18:15). A re-bake on
today's converter removes all 25 with no new code. They are also cosmetically minor: `lod=0`,
`maxFace` 0.02–2.2 m, and not what is visible at 1 km.

`Common/Materials/Default.emat` is the odd one out — it *does* name
`Common/Textures/default_BCR.edds` and still bound nothing. Separate, single-material, unchased.

## What was done

Nothing that moves a pixel. The converter is `apps/tools/Tools/commands/XobCommand.cpp`, outside
this session's write scope, and the change needs a ~45-minute re-export and a ~6 GB redeploy that
is not this session's to run.

What was added is the lock the converter fix depends on: a regression test in
`tests/unit/engine/Poseidon/Asset/Formats/Material/test_emat_material_adapter.cpp` that feeds the
verbatim `CrashBarrier_MLOD.emat` to `TranslateEmat` and requires a procedural
`color(0.168,0.185,0.188,1)` base colour and `!BindsNothing(...)`. The existing coverage for this
synthesis was written for DayZ water (`MatWaterPool`, `[dz-002]`); the synthesis is keyed on
`Color` and not on a class, but the converter is about to depend on it holding for `MatPBRBasic`,
and an untested cross-family dependency is how it stops holding. 39 `[emat]` cases, 307 assertions,
green.

## The converter fix, specified

In `TextureBaker::Bake`, the `bcr.empty()` branch must distinguish "nothing bindable" from "no
image". A material that carries a `Color` is bindable. Concretely: on the unresolved path, when the
family is neither `MatPBRDecal` nor water-erase **and** `emat.Vec4Of("Color", …)` succeeds, still
call `WriteEmat(...)` (with an empty `colourPaa`, so `WriteEmatText` emits no map key and the
adapter's `Color` synthesis fires) and set `baked.ematPath`, so `MaterialField()` names the
deployed file instead of the unreachable pak path. Equivalently, change the deploy gate at
`XobCommand.cpp:1304` from `!baked.colourPaa.empty()` to "an albedo was baked **or** the material
names a `Color`".

Expected effect: 12 of the 40 materials resolve; combined with a re-bake dropping the 25 decals,
the `draws UNTEXTURED` count should fall from 70 to roughly 3 (`Default.emat`,
`default_MatTerrainObj.emat`, `Ashtray_01_glass_clear.emat`).

## Deliberately not done

- **No `WGR_UNTEXTURED_ALBEDO` default flip.** Painting these grey would hide the white while
  leaving `CrashBarrier_MLOD` at 128,128,128 instead of its authored 43,47,48 — a wrong colour that
  is silent instead of loud, which is precisely what MAT-050 argued against. The knob remains the
  right *diagnostic*: `WGR_UNTEXTURED_ALBEDO=255,0,255` makes exactly these sections magenta.
- **No runtime path-rewriting** in `ShapeAdapter` to redirect a pak path under the deployed prefix.
  The file is not there under any spelling; that would only move the failure.
- **No engine change at all.** The adapter is already correct, and the evidence for that is now a
  test rather than a reading.

## How to confirm

Fly the Everon freefly to a crash-barrier or haystack cluster at ~200 m altitude, ~1 km out, and
run with `WGR_UNTEXTURED_ALBEDO=255,0,255`. Every reported white block must turn magenta and
nothing else may — that is the proof the blocks are these sections and not a second population.
Then `grep "draws UNTEXTURED"` for `lod=[2-6] section=0` and confirm the 12 `MatPBRBasic` names.
