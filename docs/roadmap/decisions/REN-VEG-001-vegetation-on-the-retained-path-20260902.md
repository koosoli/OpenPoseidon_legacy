# REN-VEG-001 — Vegetation on the retained path: the CPU prize is real, and the GPU pays ten times for it

**Date:** 2026-09-02
**Branch:** `asset-obs-001`
**Follows:** WLD-026's attribution (77% of Chernarus+'s visible objects are CPU-drawn trees
refused by the TreeAdv leaf-card bail), the bail's own history in `EngineWgpu.cpp` (on for one
session, reverted for three regressions)

## What was done

The three regressions the bail was reverted for are addressed in code, not argued away:

1. **One ownership question.** `SectionGpuOwned` (`ClipShape.hpp`, defined in `ShapeDraw.cpp`)
   is now asked by all three sites that used to answer it separately — Shape::Draw's skip for a
   Partial object, RegisterGpuModel's coverage decision, ClassifyGpuSection's admission. A
   section is owned when its spec is plain opaque and the retained path does not refuse it
   (today's rule), **and also** when its spec is IsAlpha but the texture is a cutout: Cutout
   histogram, legacy plant cutout, or Blend histogram under a material that authors a cutout or
   opaque route. That is the per-draw path's own rule for "IsAlpha over a Cutout texture draws in
   the opaque pass as a hard alpha test", and it is what lets a tree whose LOD 0 is all IsAlpha
   leaf cards register that LOD — the "near trees kept a coarse LOD" regression.
2. **Bark is solid.** ClassifyGpuSection applies DrawSectionTL's bark rule (`GpuTextureIsBarkOrTrunk`
   → no alpha test) and its opaque-route rule (Blend histogram under a leaf-card material with
   no alpha holes → opaque, no discard). Before, such a section was owned by spec and declined by
   the classifier, so nobody drew it — the "pine trunks disappeared" regression, explained.
3. Shadows: the cascade gate was already fixed before this.

Then `WGR_TREEADV_RETAINED` was flipped on for one deploy and measured.

## Measured (Chernarus+, WLD-025 pose, DayZ addons, 90 s warmup, one run per arm, 800×600)

| | bail ON (old default) | bail OFF (retained) |
|---|---:|---:|
| coverage full / partial / none | 1270 / 239 / **4987** | 5590 / 228 / **150** |
| pass1 objects submitted per frame | 4332 | 234 |
| `land:obj` (with probes) | 7.65 ms | **1.05 ms** |
| pass1 submit | 7.14 ms | **0.74 ms** |
| registered instances | 11,065 | 25,100 |
| main-view triangles | 3.8 M | **30.5 M** |
| Objects: prepass alpha-test / colour alpha-test | 0.26 / 0.23 ms | **2.92 / 9.78 ms** |
| GPU frame | 30.3 ms | **49.4 ms** |
| CPU frame avg / fps | 31.1 / 32 | 55.3 / **18** |

Ground pose (alt 2 m, facing the denser side): 24.1 → 27.6 ms, triangles 2.8 → 12.4 M.
Everon `perf_combat` (no TreeAdv content, so only the ownership change applies): coverage
identical (15,477 / 562 / 4089 vs 4054), objects regions within 0.1 ms — neutral.

**The pictures** (`.tmp-veg/ground_old_new.png`, `aerial_old_new.png`, old above the line):
trunks present, no holes, no sheets; the aerial shows *more* distant foliage detail in the
retained arm — which is the problem, not a bonus.

## Why the GPU pays

The per-draw path chooses each object's LOD under the frame-time governor (`_lodInvWidth`,
fed back from measured frame duration — see `ownership-authority.md` row on `SceneDraw.cpp:1094`).
The GPU-driven cull chooses LODs from a **fixed** `lod_inv_width` (`cull.wgsl:18-30`, "fixed,
not fed back"). Handing 4,800 trees to the GPU therefore hands them over at the detail the
governor would never have allowed on this frame: eight times the triangles, ten milliseconds
of alpha-tested foliage. The CPU millisecond it saves is real; it buys ten GPU milliseconds.

## Decision

`WGR_TREEADV_RETAINED` stays **OFF** by default. The parity fixes (1, 2) ship, so that with the
flag on trunks, shadows and near LODs are right — they were the reasons it could not be tried
before, and they are no longer. The lever now has exactly one blocker, and it is not in the
vegetation code: **the GPU-driven LOD selection must honour the same governor the per-draw path
does** (or the retained vegetation must be registered with a LOD bias). That is the next
target on this line, and it is the one that turns the 7.65 → 1.05 ms into a frame the player
sees.

## Not measured

One run per arm; the numbers are far outside spread. Not measured: the same flip on Stratis,
whose earlier 42.7 → 0.185 ms figure came from a run the LOD governor had already throttled to
17 fps on the CPU side, so the GPU there had headroom Chernarus+ does not.
