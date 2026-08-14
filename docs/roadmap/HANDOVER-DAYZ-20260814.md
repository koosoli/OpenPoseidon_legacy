# Handover — DayZ compatibility: what works, what is broken, and the river task

**Date:** 2026-08-14
**Branch:** `dayz-compat-layer`
**Commits:** `67fb605`, `361949c`, `05eb98e`, `3bc0bb9`
**Read first:** `docs/roadmap/decisions/DZ-001-dayz-is-an-arma-2-descendant-20260814.md`

You are taking over DayZ asset compatibility in this engine. This document is your
prompt. Everything numbered below was measured on the owner's install, not inferred —
if you contradict a number here, re-measure and say so rather than quietly diverging.

---

## Ground rules

1. **Push only to the private mirror.** Remote `safety` =
   `https://github.com/koosoli/PoseidonWGPU-private-safety-mirror`. Never push to
   `origin`/`poseidon-wgpu` (that is the public repo) or `upstream`.
2. **Another agent works in this same checkout, concurrently.** Files appear modified
   that you did not touch, and `git status` goes stale within minutes. `git add` only
   the explicit paths you edited — never `-A` or `.`. Check for a pre-staged index
   first: a non-blank first column in `git status --porcelain` means someone else
   staged it. Two of my commits swept in another agent's staged files before I caught
   it. Never `git stash` to force a branch switch.
3. **No DayZ data in the repo, ever.** DayZ is proprietary — it is not APL-SA like the
   Arma corpora. Extracted worlds live in gitignored `packages/`; everything else goes
   in a scratchpad and gets deleted. No DayZ fixture may be committed.
4. **Measure before you claim.** This codebase's convention is that a comment states a
   count and the corpus it came from. I got fooled three times in this session by
   plausible-looking reasoning; each time the fix was to measure. Two of my own
   hypotheses were wrong and I reverted the code rather than carry it.
5. **Always run the control.** "DayZ is broken" only became a finding once Arma 3's
   Stratis rendered through the *identical* path. Before blaming DayZ, run Stratis.

---

## What already works

* **Every DayZ model loads: 8,203 / 8,203, zero failures.** P3D is ODOL revision **54**,
  which belongs to the `Odol49.hpp` **Arma 2** family (48/49/50/52), not Arma 3's 73.
  Revision 54 adds six bytes across three fixed runs; embedded material **version 20**
  adds 104 bytes (eight colour quads where Arma writes six, plus eighteen words before
  the shader ids).
* **Both worlds parse to the byte.** OPRW revision **29**. Its app-id slot is the FourCC
  `ENF0`; static entities and road links each carry one extra word. ChernarusPlus
  15360 m, Enoch (Livonia) 12800 m.
* **It renders.** Terrain with correct textures, roads, grass, sky, clouds, and trees
  with correct alpha cutouts. Verified in **WGPU** and GL33.
* **Shader families:** DayZ is 99%+ served by families the schema knew (TerrainX 16,623,
  Super 5,447, Multi 1,628, TreeAdv 664). The ~144-material tail is now mapped.

### Repro command (this works today)

One-time extract (lands in gitignored `packages/`):

```
.\build\win-x64-clang-rwdi\apps\tools\Tools\PoseidonTools.exe pbo extract "D:\SteamLibrary\steamapps\common\DayZ\Addons\worlds_chernarusplus.pbo" packages\dayz-compat\world\chernarusplus -f ".wrp"
```

Run from the **repo root** (`Start.ps1` guarantees a matched exe/DLL pair out of `dist\`;
the game folder's `ColdWarAssault.exe` is a stale deploy and will not have DayZ support):

```
.\scripts\Start.ps1 -LogFile "$PWD\cwr-dayz.log" --render wgpu --width 1600 --height 900 --dev --test-mission "D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\dev-missions\devtest-dawn.abel" --test-world "$PWD\packages\dayz-compat\world\chernarusplus\chernarusplus.wrp" --addon-root "D:\SteamLibrary\steamapps\common\DayZ\Addons" --test-world-max-objects 100000 --test-world-freefly 1037 2727 60 45 -6 --test-world-hour 10 --log-level info
```

Harness notes that cost me real time: `--test-mission` needs an **absolute** path;
`--auto-screenshot` takes `frame:path` and accepts only **one**; `Start.ps1` returns
while the game keeps running, so poll for the PNG *and* the process; `LoadOprw25`'s
object-count line is `LOG_DEBUG`, so `--log-level info` hides it; and **measure captures
numerically** (`min/max/mean/nonblack`) — "looks black" and "every pixel is exactly 0"
are different diagnoses, and a capture taken too early photographs the loading state.

---

## Issue 1 — WGPU goes black at full object scale (the blocker)

`--test-world-max-objects 100000` renders with **0 uncaptured wgpu errors**. Remove the
cap and all **2,971,218** placements load and the frame is **every pixel exactly 0**.

What is established:

* Not the substituted-world path and not the other agent's in-flight renderer work:
  **Stratis renders completely through the identical path** (921,600/921,600 non-black,
  mean 140, zero errors).
* Not a validation failure at ≤100k. At 200k the 3 errors present were all emitted
  **during shutdown, after** the screenshot.
* At full scale the run streams cleanly for ~105 s, then invalid `TextureView`s appear in
  **three unrelated subsystems within the same moment** — `wgr_sky_bake_layer`,
  `wgr_bindless_texture_bind`, `wgr_grass_terrain_bind` — and the process dies. Host
  memory is 4–5 GB in that window. Three independent subsystems do not develop
  lifetime bugs simultaneously; what they share is the device.
* GL33 draws the same full world fine — it has no bindless array and bakes no sky.

Leading hypothesis: **device resource exhaustion**. Once `create_texture` starts failing,
every view derived from a failed texture is invalid.

**Tried and rejected, do not repeat:** making `SharedTextures::bindless_slots` hold
texture handles resolved against the registry at build time instead of caching cloned
`TextureView`s. It removes a real class of dangling-view bug but changed nothing
measurable (52 bindless errors before, 52 after) because those failures are collateral —
the sky-bake view goes invalid first. I reverted it.

**Do next:** instrument `create_texture`/`create_buffer` failure and GPU allocation
directly, so exhaustion is confirmed rather than inferred. Then decide between a texture
budget and not making 2.97 M objects resident at all (see Issue 2 — they are the same
problem).

---

## Issue 2 — Load time: 100 s, and it is the same problem as Issue 1

Measured on the full world: **placement loop 99,725 ms for 2,971,218 objects = 33.6 µs
each.** This is what the owner sees as the game sitting in "Get ready…" for minutes.

**Tried and rejected:** batching the per-cell `StaticChanged()` recalculation (it looked
O(N²) per cell — `ObjectListFull::Add` walks the whole cell to recompute its bounding
sphere on every insert). Measured **no improvement**: 105 s vs 100 s. Reverted.

Ruled out cheaply: the GPU-driven `SceneObjectCreated` hook is off by default
(`_gpuDriven = false`, needs `WGR_GPU_DRIVEN`); `AutoArray::Resize` already grows
geometrically, so it is not array growth.

**A phase split attributed 85% to `Landscape::AddObject`, but do not trust it.** Those
were wall-clock timers on a thread competing with active rendering (the world streams at
~2 fps while loading), and three equal 500k batches came out 40 s / 11 s / 8 s — that
shape is contention, not a cost curve. **Get a real profiler on it, or load with
rendering suspended,** before optimising anything.

The structural answer is almost certainly that the engine should not create 2.97 M
resident objects up front. `Landscape` already has per-cell object lists, so the spatial
structure for streaming exists. That is an architecture change; scope it with the owner
before starting.

---

## Issue 3 — Rivers and ponds render as flat colour, and this is the owner's priority

DayZ's water draws as flat neon green. **This is now well understood and the fix is not
guesswork.**

Each DayZ water surface has **two** material files side by side:

* The `.rvmat` is a **79-byte stub** carrying nothing but
  `PixelShaderID="CalmWater"; VertexShaderID="CalmWater"`. No stages, no colours. The
  engine reads this one, finds nothing to shade with, and falls back — that is the green.
  `CalmWater` is now a known family with an explicitly empty stage list (accurate, but it
  does not make water look like water).
* The **`.emat`** sibling is the real material — Enfusion format, plain text, trivially
  parseable. Example, `DZ\water_bliss\ponds\data\enoch_pond.emat`:

```
MatWaterPool {
 Color 0.0392 0.0549 0.0667 0.3725
 ZWrite 0
 Sort translucent
 NormalMap "{126A7892740C80E0}DZ/water_bliss/ponds/data/enoch_pond_nohq.edds"
 NormalPower 1.55        NormalMapping 0.1
 SunPower 525.19         SunInfluence 5        SunNormalMul 0.108
 WaterExtinction 3       DepthScattering 1     GeometryBlend 0.678
 RefractScale 0.5        RefractOffset 0.051   ChromAberation 0.168
 WindInfl 0.2
 CausticMap "{EA764AD7A6112284}Enfusion/Graphics/Textures/Water/caustic.edds"
 Caustic 0.05            WetFloor 0.75
 EnvironmentMap "{46C92AE52447906C}Enfusion/Graphics/Textures/Water/env_lake_co.edds"
 ReflectionColor 0.2 ...
}
```

Texture references are `{GUID}path.edds` — strip the brace-delimited GUID and resolve
the path. `.edds` is a **plain DDS container** (magic `DDS `, e.g. 512×512, 10 mips,
`pfFlags=0x41` = uncompressed RGB+alpha), and the engine already has a DDS reader:
`Graphics/Textures/DDSConverter.hpp` (`ReadDDSBuffer`). **`.edds`/`.dds` are not
registered in the texture-type selector** — see the extension list in
`Graphics/Rendering/Font/Pactext.cpp` (~line 2062), which accepts only
`.paa/.pac/.jpg/.jpeg/.png/.tga/.bmp` and logs "Unrecognized texture type" otherwise.
Registering them needs a `TextureSourceDDS` + factory; model it on
`Graphics/Textures/JpgImport.{hpp,cpp}`, which is a small, self-contained example of the
same `ITextureSource` interface.

### The geometry, and flow direction

| Where | Count | What |
|---|---|---|
| `DZ\water\ponds` | 187 | pond meshes |
| `DZ\water\streambed` | 159 | stream beds |
| `DZ\water\streams` | 24 | stream surfaces |
| `DZ\water_bliss\river` | **804** | river segments, `enoch_river_0..N.p3d` |
| `DZ\water_bliss\ponds` | 195 | pond meshes |

**Rivers are per-segment meshes**, placed by the world file like any other object.

**Flow direction is not authored anywhere I could find.** The `.emat` has `WindInfl` but
no flow vector, and the `.rvmat` has nothing. So the owner's request — rivers that flow,
with direction — means **deriving** direction rather than reading it. The obvious source
is the placement itself: each river segment is a `Oprw25Object` with a full
`Matrix4x3` basis (rows are aside/up/dir/pos), so **the segment's `dir` row is its
downstream axis**, already in world space, already read by `Oprw25.hpp`. Chaining
consecutive `enoch_river_N` segments by proximity gives a polyline with per-segment
direction and therefore a flow field.

Before implementing, verify that claim: sample the `dir` rows of the river placements out
of the world file and check they run consistently along the watercourse rather than
arbitrarily. If they do not, fall back to deriving direction from the segment centre
chain. **Do not assume it — this is exactly the kind of plausible inference that cost me
time three times over.**

The engine has a substantial existing water system (FFT ocean, cascades, foam,
interaction) under `engine/WgpuRenderer/rust/src/water/` and
`engine/Poseidon/World/Terrain/WaterSurfaceQuery.*`. Rivers should extend it, not fork
it. Be aware another agent actively edits the renderer (sky, god rays, water); coordinate
via the owner before restructuring shared water code.

---

## Issue 4 — Smaller, genuinely open

* **The material-version gate is untested at its boundary.** `Odol73.hpp` widens the
  embedded material at `material.version >= 12`, but every Arma material observed is
  ≤ 11 and every DayZ one is exactly **20**. Nothing between has been seen, so where the
  change actually landed is unknown. If an Arma 2/3 asset ever reports 12–19, re-measure.
* **Revisions 51, 53, 55, 56 stay refused** and a test asserts it. The DayZ widening keys
  on `revision >= 54`; admitting 53 would silently give it DayZ's field widths.
* **Sakhal** (the DLC world) was not present as a `.wrp` in the scanned install — only
  ChernarusPlus and Enoch. Worth re-checking if the owner installs it.
* **Trailing map-info is retained undecoded** (17,810,372 bytes on ChernarusPlus),
  exactly as the Arma 3 worlds do.
* **1,035 materials log `no albedo`** and fall back to their face texture. For `ironPlate`
  and similar this is *correct* — those rvmats genuinely author no `_co` stage. Do not
  "fix" it without first checking whether the material has a colour stage at all.
* **DayZ trees are gold/yellow.** The leaf atlas genuinely decodes that way (verified
  against the PAA directly, correct alpha). DayZ likely applies a seasonal tint we do
  not; confirm before treating it as a bug.
* **Out of scope, and a different engine:** 2,816 Enforce Script `.c` files, 8,483 `.anm`
  animations in a `FORM`/`ANIMSET5` container (RTM is gone entirely), `.ptc` particles,
  `.layout` GUI, precompiled DXBC shaders.

---

## Where the tooling is

* `PoseidonTools terrain census <wrp>` — the world compatibility harness. Prints
  byte-accounting that catches a wrong field width anywhere earlier. Use it after any
  ODOL/WRP/material change and compare counts; a drop is a regression.
* `PoseidonTools model inspect <p3d> [-t --sections --classify --texroot <dir>]` — per-LOD
  textures, section→texture assignment, and alpha classification (opaque/cutout/blend).
* `PoseidonTools pbo extract` / `image convert` / `config debin` (note: `config debin`
  does **not** work on RVMATs — use a rapified reader).
* `--test-world-max-objects N` — the triage lever that separated Issues 1 and 2 from each
  other. Without it, "DayZ is black on WGPU" reads as a compatibility bug.
