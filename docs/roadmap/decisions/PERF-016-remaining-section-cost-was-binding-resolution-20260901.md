# PERF-016 — the 0.64 us/section PERF-015 left unattributed: 60% was re-resolving the material binding, and it caches the same way

**Date:** 2026-09-01
**Branch:** `asset-obs-001`
**Follows:** PERF-015, which halved `land:obj` by caching per-section string classification
and closed with: the remaining 0.64 us/section is **not attributed** and should not be
guessed at, naming three candidates in probe order — the two `_materialBindings.find()`
string-hash lookups, the `_wbank->Load()` by-string texture lookups, and
`BuildRenderPassDescriptor`.
**World / pose / mission:** identical to PERF-015 — Chernarus+ (`packages/dayz-compat`),
DayZ addons, `5704.58 2523.37` alt 120 az 80.9, hour 12, `perf_combat`, 1280x720, DLSS off,
`WGR_RENDER_THREAD=0` pinned in every arm.
**Binary:** `dist/x64-win-rwdi`, exe and `wgpu_renderer.dll` staged by the build
(md5 `bbab6207…` / `bf29c0e1…`), hash-verified unchanged after the last arm — checked
because another session committed to this shared worktree mid-campaign (`c274badc`, frame
pacer; none of its files overlap this change, and the binaries did not move). All arms are
the **same binary**, separated only by `WGR_SECTION_BIND_CACHE`.

---

## Attribution first: what the 0.64 us actually was

`WGR_PASS1_STATS` gained a seventh line, `PERF drawSection`, subdividing `DrawSectionTL`
itself (the half of `shapeDraw` the previous split could not see into) with a count per
bucket. Windowed 512→1024, lever off — this is the term PERF-015 measured as 0.64
us/section, inflated here by the new probe's own clock reads (identical in every arm):

| windowed, off arm | ms/frame (a / b) | us/call | share of DrawSectionTL |
|---|---:|---:|---:|
| **DrawSectionTL total** | 4.661 / 4.641 | 0.94 | 100% |
| **bind** — `_materialBindings.find` + `EffectiveBaseColourStage` + `loadName` string + 2x `_wbank->Load` | **2.173 / 2.181** | **0.44** | **46.7%** |
| **refl** — `MaterialIsReflective` x2 (each its own map find) | **0.619 / 0.636** | **0.13** | **13.5%** |
| alpha routing (leaf cards / bark / cutout) | 0.385 / 0.378 | 0.08 | 8.2% |
| desc — `BuildRenderPassDescriptor` | 0.103 / 0.105 | 0.02 | 2.2% |
| light fold | 0.135 / 0.132 | 0.03 | 2.9% |
| other (header, traces, memcpy, push_back, probe reads) | 1.246 / 1.211 | 0.25 | 26.4% |

Denominator: 4,962 DrawSectionTL calls/frame during Pass1 (5,278 sections drawn; contiguous
same-material sections merge into one call).

So PERF-015's probe order was right about the family and wrong about nothing: **60% of the
remaining per-section cost was the two map candidates it named first** — the binding block's
string-hashed `find`, the `std::string loadName` build, the two by-string texture-bank
`Load()`s (each interning an `RStringB` from `char*`), and `MaterialIsReflective` doing its
own string-hash find twice per section. `BuildRenderPassDescriptor`, the third candidate, is
0.02 us — dead as a lead, measured this time rather than assumed.

## The change

`WGR_SECTION_BIND_CACHE` (ON by default, `=0` restores the old path exactly, same binary).
The per-material half of the binding block joins the PERF-015 `MaterialClass` slot — same
address-keyed / path-validated contract, same thread_local cache:

* the resolved base-stage decision (`BaseImage` / `BaseConstant` / none) and its load name
  as an **interned `RStringB`** (so the per-section `Load()` no longer re-interns a path),
* the normal-map load name likewise,
* `IsCalmWater` and the DZ-003 `reflective` answer — `MaterialIsReflective` replays it
  instead of string-hashing `_materialBindings` twice per section.

What is deliberately **NOT cached: texture handles.** The `_wbank->Load()` +
`EnsureUploaded()` pair still runs per section, because that pair is what keeps residency
honest — `Load` refreshes the texture's LRU mark and `EnsureUploaded` repairs an eviction.
This is the `Ref<Texture>`-pinning decision PERF-015 deferred, and it stays deferred: the
cache removes the *name resolution*, not the *use marking*. The DZ-006 per-section half
(constant albedo only when the section has no face texture) is re-applied per section.

Any material-debug selector (`WGR_MATERIAL_DEBUG_RVMAT`) keeps every section on the full
path, so every diagnostic and one-shot log behaves exactly as before; the cache fills only
after one full successful resolution of a material, so an exception caches nothing.

## Measured

Windowed 512→1024, two runs per arm, same binary, `WGR_PASS1_STATS=1` in all four arms.

| windowed ms/frame | off a | off b | **on a** | **on b** | change |
|---|---:|---:|---:|---:|---:|
| pass1 submit | 6.543 | 6.458 | **3.743** | **3.857** | **−41.5%** |
| shapeDraw | 4.983 | 4.948 | 2.421 | 2.494 | −50.5% |
| **DrawSectionTL total** | 4.661 | 4.641 | **2.026** | **2.081** | **−55.8%** |
| bind | 2.173 | 2.181 | 0.445 | 0.456 | −79.3% |
| refl | 0.619 | 0.636 | 0.177 | 0.186 | −71.1% |
| desc / alpha / light (untouched) | 0.62 | 0.62 | 0.56 | 0.57 | ~flat |
| `land:obj` (pass1 six-bucket sum) | 7.106 | 7.012 | 4.291 | 4.426 | **−38.3%** |

Spread within arms: off 0.4–1.3%, on 2.7–3.0% — an order below the 2.6 ms the arms differ
by. **Denominators identical:** objDraws 2448/2447, sectionsSeen 6059–6060, sectionsDrawn
5278/5279, GPU-driven coverage 1020 / 251–252 / 3386–3410, runs=0 in all four arms — the
same frame submitted more cheaply, nothing drawn less.

**Probe-free absolutes** (no `WGR_PASS1_STATS`; FrameProfiler `land:obj` average over the
capture, two runs per arm, same binary, hash-verified before and after):

| unprobed ms/frame | off a | off b | **on a** | **on b** | change |
|---|---:|---:|---:|---:|---:|
| **`land:obj` avg** | 5.123 | 5.185 | **2.640** | **2.666** | **−48.6%** |
| `land:obj` p95 | 5.556 | 5.894 | 2.921 | 2.941 | −48.8% |
| `drw:land` avg (Pass2 shares DrawSectionTL) | 1.509 | 1.523 | 1.140 | 1.157 | −24.2% |

Spread 1.2% within the off arm, 1.0% within the on arm. The probed table above overstates
every absolute by the probe's own clock reads (identical in both arms — the differences are
honest, the milliseconds are upper bounds); this table is what the frame actually pays.
Sequence to date on this world: `land:obj` 13.2 ms → 5.9 (PERF-015) → **2.65** (this).

### Was it actually enabled

The lever counts itself: off arms `fast/frame=0 resolves=0` (never entered, not merely
ineffective); on arms `fast/frame=5654–5655` against `bindCalls/frame=5655–5656` — every
material-bearing section in the frame served from the cache — with 415–416 cumulative
resolves (the distinct-material population) and no re-resolutions in the window.

### Visually verified

Same methodology as PERF-015 (the scene animates, so cross-arm difference is judged against
within-arm difference on the ground region, rows 380–720, which holds every CPU-submitted
object):

| ground region | meanAbsDiff | px >8 | px >32 |
|---|---:|---:|---:|
| off_a vs off_b — within-arm noise | 2.376 | 7.08% | 0.95% |
| on_a vs on_b — within-arm noise | 1.358 | 4.59% | 0.32% |
| off_a vs on_a — cross-arm | 2.712 | 7.14% | 1.31% |
| off_b vs on_b — cross-arm | 1.846 | 4.29% | 0.90% |

Cross-arm sits inside the bracket the two within-arm rows span. By eye the frames are the
same scene: same panel buildings and window patterns, same autumn tree cards in the same
colours (the leaf-card / bark routing this code decides), same sea with the same CalmWater
reflection (the cached `reflective` answer), same roads and rooftop fixtures; the residual
is cloud animation and one HUD radio line. The failure modes a wrong cached binding would
produce — white untextured walls, missing normal maps, unreflective water, glowing snow
water — are all absent.

## What this does not say, and what is next

* **Frame rate did not move** (34.5 → 33.5–34.0 fps): at this pose the frame is GPU/swap
  bound (`swap` grew by what `land:obj` shrank). The win is CPU headroom, which is what the
  campaign is for — it cashes out where the CPU is the ceiling (busier scenes, weaker CPUs,
  render-thread contention), not at this bench's bottleneck.
* The remaining DrawSectionTL cost (on arm, windowed 2.05 ms, 0.41 us/call) is now: other
  0.86 (42%, including the probe's own reads), residual bind 0.45 (the two bank `Load` +
  `EnsureUploaded` calls — the residency-honest floor, plus the fast-path checks), alpha
  routing 0.35, refl residual 0.18, light 0.12, desc 0.10. Squeezing further means either
  caching handles (the residency argument above says not yet) or submitting fewer, larger
  draws — which is the instancing/batching question, now to be priced against a 2.65 ms
  phase (unprobed) rather than 5.9. At that size, `land:obj` may no longer be the largest
  CPU phase on this world — `sim:step` (3.8–4.7 ms windowed here) likely now is, and the
  next attribution belongs there before more renderer work.
* One machine, one pose, one world; stock CWA islands not re-measured.
* Windowed `sim:step` varies 3.8–4.7 ms run to run (AI), which is why frame-average FPS is
  a poor lever metric here; every conclusion above is drawn from the phase and bucket
  deltas, not FPS.
