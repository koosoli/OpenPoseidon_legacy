# Debugging vegetation that does not shade

Companion to [DEBUGGING-INVISIBLE-GEOMETRY.md](DEBUGGING-INVISIBLE-GEOMETRY.md). That one is
for geometry you cannot see at all; this one is for geometry you *can* see and that looks
**flat** — a tree canopy lit evenly from every angle, a bush with no lit side and no dark
side, a forest that throws nothing on the ground.

Written 2026-08-29 from the Chernarus (DayZ) investigation. **The headline finding is that
"the trees do not shade" was four different complaints wearing one sentence**, and three of
the four turned out not to be the renderer's fault at all. Work the list in order: it is
sorted so that the cheap measurements that *eliminate* whole explanations come first.

---

## Rule: never judge vegetation shading from a single screenshot

Three separate things make a correct forest look flat in a still, and none of them is a bug:

1. **The sun behind the camera.** Every cast shadow then falls *away* from the viewer and is
   hidden by the object casting it. In the Chernarus session a 10:00 capture looked completely
   shadowless; the same camera at 07:30 showed the treeline throwing a broad shadow across the
   slope. Nothing had changed but the sun angle.
2. **Cloud cover.** `cloudDim` in `EngineWgpu::NextFrame` ramps to 1 by ~0.8 coverage and
   deliberately reduces the directional sun to 5%, boosts ambient, and fades cascade darkness
   to nothing. An overcast sky *should* have no sharp shadows. Force `0 setOvercast 0` in the
   test mission before drawing any conclusion.
3. **The camera far above the canopy.** Cascade 0 covers the near field. From 60 m up it may
   legitimately contain nothing at all, and a shadow-map dump then looks empty and innocent.

So: **low sun, clear sky, camera near the vegetation.** Three flags, one run, and the
question becomes answerable.

---

## The measurement ladder

Each rung is one command and eliminates a whole class of cause. Do not skip upward.

### 1. Is the shadow pass even running?

```
WGR_SHADOW_GATE_TRACE=1
```

Logs on *transition*, from two places that answer two different questions:

- `Scene shadow pass gate: mapsEnabled=… shadowFactor=… nDraw=… ran=…`
  (`Scene::DrawObjectsAndShadowsPass2`) — did the depth pass run at all?
- `Wgpu shadow gate: active=… cpuCasters=… gpuInstances=… cascadesValid=…`
  (`EngineWgpu::NextFrame`) — did the frame end up with a usable cascade set?

**Trap that cost an hour:** `cascadesValid=false` in the *last* logged transition is not
proof of anything. `_smCascadesValid` is cleared in `InitDraw` and set again by the shadow
pass, so any frame that draws no world — a UI frame, the screenshot frame — legitimately ends
with it false. Read the scene-side line, which says whether the pass ran, before believing the
backend line.

### 2. Is anything actually in the shadow map?

From an `init.sqs` in a staged test mission:

```
0 setOvercast 0
~28
_r = triShadowSceneDump "sm"
logInfo format ["SMDUMP %1", _r]
```

Set `TRI_OUTPUT_DIR` to somewhere you can read; the PNG lands there. Canopy-shaped speckle in
the dump means the trees *are* being rasterised into cascade 0 and the fault is downstream, on
the receiver side. A uniformly flat image means either nothing is casting **or** cascade 0
covers empty air (see the camera-height trap above) — re-shoot from the ground before
concluding.

### 3. Is this asset even on the path you are debugging?

```
GpuModel foliage bail: model=… lod=… section=… tex=… retained=0
```

Vegetation whose texture classifies `Blend` and whose material reads as cutout is
**deliberately** kept off the retained/GPU-driven path (`EngineWgpu.cpp`, the
`WGR_TREEADV_RETAINED` block — the retained path loses the pine-trunk discard, the finest LOD,
and shadow casting). Chernarus bails 90 models including every `dz\plants\tree\*`.

This matters because `Objects shadow counts: c0=… c1=…` counts **only the retained cascade
cull**. A forest of bailed trees reports zeros there and still casts perfectly well through
`Scene::RenderShadowMapDepthPassGpu` → `EngineWgpu::AddShadowCaster`. Do not read that line as
"the trees are not casting".

### 4. Does the model *say* it casts?

`Scene::AdjustShadowComplexity` and the caster loop both honour `NoShadow`,
`obj->CastShadow()`, the cascade reach, and the `lodnoshadow` LOD property.

`lodnoshadow` is a genuine trap and worth knowing about even though it was **not** the
Chernarus cause: it is a 2001 authoring hint meaning "do not build a stencil shadow VOLUME
from this LOD", which was correct when the volume was extruded per object on the CPU and a
canopy of alpha cards would have produced garbage. It does not mean "this object occludes no
light", and a depth-map cascade has no such problem. DayZ carries it on the visual LODs —
`b_fagussylvatica_1f` has 6 occurrences over 8 LODs, `b_piceaabies_1f` has 4.

Measure it, do not assume it:

```sh
grep -aoi "lodnoshadow" <model>.p3d | wc -l      # the property is plaintext in ODOL
WGR_IGNORE_LODNOSHADOW=1                          # ablation: ignore it entirely
```

On Chernarus the ablation changed **nothing**, which is what took the property off the suspect
list. Run the ablation before writing a fix for it.

---

## The cause, when the ladder comes up empty: the foliage lighting model

If the shadow pass runs, the map has canopy in it, and the trees still have no lit side, the
answer is almost certainly that the shading is deliberately flattening them.

`Engine::FoliageSettings` (`Graphics/Core/Engine.hpp`) exists so low-poly alpha cards do not
split into a hard lit/black pair at harsh sun angles. Its defaults were dialled in by eye on
2026-07-12 against **CWA's small trees**, and they are far too strong for a dense
Arma-2/DayZ-era crown:

| Knob | Default | What it does to the look |
|---|---|---|
| `treeBend` | 0.7 | **The big one.** Replaces each leaf card's geometric normal with a radial one about the crown centre — the crown *becomes* one smooth ball, so individual cards cannot be lit differently |
| `normalBend` | 0.8 | the same for bushes |
| `wrap` | 0.5 | fills in the terminator, i.e. exactly the lit/dark boundary |
| `ambientBoost` | **1.6** (was 2.5) | sky irradiance multiplier on foliage — see the finding below |
| `transScale` | 0.54 | subsurface fill on the shaded side |

To see the contribution in isolation:

```
WGR_FOLIAGE_ABLATE=1
```

Zeroes the fill, the wrap, the ambient boost and both spherical-normal terms for one run
(sway is untouched — it ablates lighting, not motion). On Chernarus this is what finally
produced a tree with a lit side and a shaded side, and a background canopy that separates into
individual trees.

**The ablation is not the fix.** Those knobs exist for a real reason and at zero the cards
break up again at low sun. The fix is a weakening — and it is live on the dev panel's
**Foliage** tab, so tune it there with the camera moving rather than guessing at defaults.

---

### Finding: the right ambient boost is generation-dependent, and 1.6 is a compromise

Owner's call, 2026-08-29, after looking at Chernarus and OFP content side by side:

- **On DayZ-era vegetation, `ambientBoost = 0` looks best.** Those crowns are dense and carry
  their own internal depth; any sky lift on top of that only washes it out. Zero — not a small
  value, zero.
- **On OFP's own trees, 0 is worse.** They are sparse, low-card, and genuinely need the fill;
  without it they read as cut-out paper.
- **1.6 is the shipped compromise**, and it is a compromise rather than an optimum for either
  side. It was 2.5, which was tuned against CWA's small trees alone and is what flattened the
  Chernarus canopy.

The slider's range now starts at **0**, not 0.5, precisely so the DayZ look is reachable
without a rebuild.

The real answer is a per-generation split — DayZ/Arma-2-era content near 0, OFP content at the
old lift — keyed off whatever already distinguishes the two (the same authorship signal the
A1 plant-card routing uses is the obvious candidate). Until that exists, do not "improve" 1.6
by testing on one world: moving it toward either optimum makes the other world worse, and the
worse one is the one you did not have open.

---

## Still open

- **The right default set.** Weakening (`treeBend` ≈ 0.3, `normalBend` ≈ 0.4, `wrap` ≈ 0.25,
  `ambientBoost` ≈ 1.6) is proposed but **not** validated against CWA's own trees at low sun,
  which is the case the current values were chosen for. Check both worlds before changing a
  default.
- **Individual blown-out trees.** At `freefly [1025.37, 2708.48, 21.56] az 331.5 el 4.6` on
  Chernarus one tree renders near-white and uniform while its neighbours have tonal range.
  Not yet isolated. Two candidates, both unproven: the ambient boost clipping a pale-leaved
  species, or the material — that world logs 163 `Wgpu material: no albedo for …` warnings,
  many of them `dz\plants\*` leaf and trunk rvmats, which fall back to the face texture.
  Establish which by killing the material before theorising about the lighting.

---

## Reproducing any of this

```powershell
scripts\farfield-bench.ps1 -Label <name> `
  -World "packages\dayz-compat\world\chernarusplus\chernarusplus.wrp" `
  -AddonRoot "D:\SteamLibrary\steamapps\common\DayZ\Addons" `
  -Mission "dev-missions\devtest-dawn.abel" `
  -Freefly 1025.37,2708.48,21.56,331.5,4.6 `
  -WorldHour 7.5 -Width 2560 -Height 1440 -WarmupSeconds 32 `
  -Out ".tmp-dayz-trees" -Env @{ WGR_FOLIAGE_ABLATE = '1' }
```

`-Freefly` takes **X, Z, Y, azimuth, elevation** — the dev panel's readout order, not the
log's `x, y, z`. `-WorldHour 7.5` for a low sun. Streaming needs ~30 s before the world is
settled; a 25 s warmup photographs a half-loaded forest.

Two captures compared pixel-for-pixel will always differ in the **sky**, because the cloud
deck keeps moving between runs. Crop to the vegetation before drawing a conclusion from a
difference image, or the noise buries the signal.
