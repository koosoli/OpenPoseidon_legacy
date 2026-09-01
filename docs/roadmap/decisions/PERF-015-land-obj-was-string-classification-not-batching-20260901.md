# PERF-015 — `land:obj` halves, and the answer was neither of the two routes on the table

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Follows:** PERF-014, which measured `Shape::Draw` at 75–77% of `land:obj` on Chernarus+,
found `runs=0` for two independent reasons, and left one question open: whether to collapse
the per-object submission cost by **implementing CPU instanced runs for the wgpu backend**
or by **widening the GPU-driven retained path** past the 22% of objects it already takes.
**Neither.** The dominant term was not submission overhead at all.
**World:** `packages/dayz-compat/world/chernarusplus/chernarusplus.wrp`, DayZ addons, pose
`5704.58 2523.37` alt 120 az 80.9, hour 12, `perf_combat`, 1280x720, DLSS off,
`WGR_RENDER_THREAD=0` pinned in every arm.
**Binary:** `dist/x64-win-rwdi`, exe and `wgpu_renderer.dll` staged from the same build and
byte-compared against the build tree before measuring. Both arms are the **same binary**,
separated only by `WGR_SECTION_CLASS_CACHE`.

---

## What the 1.8 us per section actually was

PERF-014 stopped at "`Shape::Draw` is 75–77% of the phase, ~1.8 us per section" and priced
what batching would be worth. Reading the code under it first — before picking a route —
found that the per-section cost is not the cost of *queueing a draw*. It is the cost of
**re-deriving, from path text, answers that never change**, once per section per frame.

Every section that reaches `EngineWgpu::DrawSectionTL` asks its material and its texture a
series of classification questions — is this a lamp fixture, does it emit at night, is it a
window, is this bark or trunk, does the material use leaf cards. Each is a pure function of
the asset's **path string**. Each was recomputed on every draw, and the spelling of the
underlying test was:

```cpp
std::string haystack(text); std::transform(...tolower...);
std::string needle(part);   std::transform(...tolower...);
return haystack.find(needle) != std::string::npos;
```

Two heap allocations and two full lowercase passes **per question asked**. Counted at the
call sites in `DrawSectionTL`, one section asks:

| classifier | questions | notes |
|---|---:|---|
| `StaticEmitterRadianceScale` → `StaticEmitterRadianceTarget` | up to 6 | |
| `MaterialEmitsAtNight` → `StaticEmitterRadianceTarget` **again** | up to 6 | same material, same frame |
| `MaterialEmitsAtNight` → `MaterialIsWindow` | 2 | |
| `GpuTextureIsBarkOrTrunk` | 5 | full texture path, five scans |
| `GpuSectionMaterialUsesLeafCards` | — | `std::string` build + **mutex** + string hash |

That is on the order of **forty std::string constructions per section per frame**. At the
~6,000 sections this pose draws, roughly a quarter of a million heap allocations per frame,
inside the phase the previous pass had already established was 38–51% of the CPU frame.

**PERF-014's `runs=0` was true and its 72.9%-batchable census was true, but batching was not
the largest thing wrong.** The submission loop was not slow because it submits one object at
a time; it was slow because each submission recomputed a constant.

---

## The change

`WGR_SECTION_CLASS_CACHE`, one lever over two halves — they are one change and measuring
them apart would report two numbers, neither of which is what the frame does.

1. **`render::ContainsNoCaseAscii`** (`engine/Poseidon/Graphics/Rendering/AsciiSearch.hpp`) —
   allocation-free case-insensitive search. Equivalence is not an approximation: the old form
   lowered with `std::tolower` under the process default "C" locale, which folds `A`..`Z` and
   leaves every other byte alone, so an ASCII-only fold is the *same function*.
   `tests/unit/.../test_ascii_search.cpp` asserts that against `std::tolower` over all 256
   bytes, and asserts the new answer equals the old one over the real classifier needles
   crossed with authored paths from the A1/A2/A3/DayZ corpora.
2. **Per-material and per-texture classification caches** — `MaterialClassSlot` and the
   `GpuTextureIsBarkOrTrunk` front cache. **Keyed on the object's address, validated on its
   path text.** The address alone would be unsafe (a released material's address can be
   reused), but every one of these answers is a pure function of the path, so an entry whose
   stored path still compares equal *is* the right answer whatever object now lives there,
   and a mismatch recomputes. The cache is correct by construction rather than by an argument
   about when materials are freed. `thread_local`, so the per-section path takes no lock —
   the leaf-card cache it fronts holds a mutex, and a mutex acquisition per section per frame
   was part of what is being removed.

`WGR_SECTION_CLASS_CACHE=0` restores the 2026-09-01 behaviour exactly, in the same binary.

---

## Measured

Windowed deltas between the 512- and 1024-frame reports, two runs per arm, same binary,
`WGR_PASS1_STATS=1` in all four so the probe cost is identical on both sides.

| windowed 512→1024, ms/frame | off a | off b | **on a** | **on b** | change |
|---|---:|---:|---:|---:|---:|
| **`land:obj` (Pass1 total)** | 13.212 | 12.980 | **5.831** | **5.891** | **−55.3%** |
| submit | 12.650 | 12.408 | 5.272 | 5.330 | −57.7% |
| **shapeDraw** | **10.267** | **10.036** | **3.841** | **3.885** | **−62.0%** |
| proxies (recursion, same work inside) | 1.262 | 1.227 | 0.412 | 0.415 | −66.7% |
| scan | 0.209 | 0.215 | 0.161 | 0.165 | |
| complexity / sort (untouched) | 0.326 / 0.186 | 0.335 / 0.187 | 0.335 / 0.176 | 0.334 / 0.178 | flat |

Run-to-run spread **1.8%** within the off arm (12.980 / 13.212) and **1.0%** within the on
arm — far smaller than the 7.3 ms the arms differ by.

Per item, and the denominators that make it a like-for-like comparison:

| | off a | off b | on a | on b |
|---|---:|---:|---:|---:|
| top-level `Object::Draw` bodies | 2,448 | 2,451 | 2,448 | 2,447 |
| sections walked | 6,179 | 5,999 | 6,060 | 6,060 |
| sections drawn | 5,482 | 5,322 | 5,279 | 5,280 |
| GPU-driven coverage full / partial / none | 1021 / 252 / 3391 | 1022 / 253 / 3399 | 1022 / 253 / 3393 | 1021 / 253 / 3404 |
| **shapeDraw us/section** | **1.662** | **1.673** | **0.634** | **0.641** |
| **shapeDraw us/object** | **4.194** | **4.095** | **1.569** | **1.588** |
| instanced runs formed | 0 | 0 | 0 | 0 |

**Nothing was drawn less.** The object count is identical to three digits, the section counts
overlap between arms, and the GPU-driven full/partial/none split is unchanged — so this is not
a coarser LOD or a wider skip, it is the same frame submitted more cheaply. `runs` is still 0:
this change did not touch batching, and does not depend on it.

### Was it actually enabled — the PERF-014 trap, answered

PERF-014 warned that the obvious one-line fix (lifting the light veto) "would measure as *no
effect* and be misread as *batching does not help*". A before/after alone cannot tell a
change that did nothing from a lever that was never on, so the cache counts itself:

```
off arm:  PERF section class cache: enabled=0 calls/frame=0     misses/frame=0.00  hitRate=0.000%
on  arm:  PERF section class cache: enabled=1 calls/frame=11626 misses/frame=1.16  hitRate=99.990%
```

Windowed, the on arm fields **15,370 classification questions per frame with zero misses** —
2.5 questions per section, every one previously two to six allocating substring searches. The
off arm's `calls=0` proves the lever was genuinely off there, not merely ineffective.

### Visually verified

`perf_combat` on Chernarus+ animates (AI, particles, drifting cloud), so no two runs are
pixel-identical and a raw whole-frame diff has a noise floor. The comparison that means
something is **cross-arm difference against within-arm difference, on the region that
contains the affected geometry** — the lower 340 rows, which hold every building, tree,
road and vehicle the CPU path submits:

| ground region (0,380)–(1280,720) | meanAbsDiff | px >8 | px >32 |
|---|---:|---:|---:|
| off_a vs off_b — *within-arm noise* | 2.145 | 5.87% | 1.24% |
| off_a vs on_a — **cross-arm** | 1.407 | 4.63% | 0.40% |
| off_b vs on_b — **cross-arm** | 2.094 | 5.63% | 1.20% |

Cross-arm difference is **at or below the off arm's own run-to-run noise**. The residual sits
in the sky (cloud animation), which contains no CPU-submitted object sections. By eye the two
frames are the same scene: same buildings, same autumn tree cards with the same colours, same
road markings, same water, same shore. That matters here specifically because the classifiers
this change caches are what decide a section's blend mode and alpha reference — a wrong cached
answer would show as see-through canopies, solid leaf cards, black bark or glowing lamps, and
none of those appear.

---

## The route question PERF-014 left open, answered on this evidence

**Rejected: implementing CPU instanced runs for wgpu.** It would have worked — 72.9% of drawn
objects do sit in a run of ≥4 — but it was aimed at the wrong term. Instancing *amortises*
per-section work over a run; it does not remove it, so on a frame whose per-section work was
40 string allocations it would have divided that by the run length and left the rest, while
requiring a per-instance transform buffer, an ABI extension, shader changes, and an
interaction with the retained path to get wrong. It also does nothing for the 27% of objects
that are not in a run. Removing the work outright is strictly better than dividing it, helps
every object rather than 72.9% of them, and cost one file's worth of change.

**Rejected: widening GPU-driven coverage first.** The retained path takes 22% of objects and
those cost this phase nothing, so widening it is real and should still happen — but it is a
model-admission problem (the emitter bail, the blend/decal refusals) with per-family
correctness risk, and it would have moved objects off a path that was 2.6x more expensive per
section than it needed to be. Fixing the price of the direct path first makes that decision
cheaper and less urgent, and does not conflict with it: the coverage split is unchanged here,
so both routes remain open on top of this.

**What was done instead** is a third thing neither of those framings contained, and it was
only visible by reading what `Shape::Draw` spends its time on rather than assuming that a
phase dominated by per-object submission is dominated by *submission*.

---

## What this does not say, and what is next

- **`land:obj` is still the largest CPU phase**, now at ~5.9 ms rather than ~13.1. `shapeDraw`
  is still 66% of it, at 0.64 us/section. That remaining 0.64 us is **not attributed** by this
  work and should not be guessed at. The visible candidates, in the order a probe should test
  them, are: the two `_materialBindings.find(materialPath)` string-hash lookups per section
  (one in the binding block, one in `MaterialIsReflective`); the `_wbank->Load()` texture-bank
  lookup by string plus a `std::string` copy, paid per section per frame for base and normal;
  and `BuildRenderPassDescriptor`. The first is safe to fold into `MaterialClass` — those map
  nodes are stable and never erased, so a cached `TranslatedMaterial*` is sound. The second
  needs a decision about holding `Ref<Texture>` in a cache, which pins textures against
  residency; that is why it was not done here.
- **With that term gone, CPU instancing becomes worth re-pricing** — but against 5.9 ms, not
  13.1, and after the remaining per-section cost is attributed. A route chosen against the old
  number would be sized wrong.
- One machine, one pose, one mission, one world. Nothing here re-measures the stock CWA
  islands, where `land:obj` was 2–3 ms.
- The probe (`WGR_PASS1_STATS`) inflates what it reports; it is identical in both arms, so the
  *difference* is honest and the shares are honest, but the absolute milliseconds are upper
  bounds.
- **Not deployed.** The brief for this pass forbade `Deploy.ps1`. The owner's game folder does
  not have this change; the measurements above are from `dist/x64-win-rwdi`.
