# REN-TEMP-001 — Temporal Rendering & Optional Upscaling (revised plan)

Status: reviewed against the codebase 2026-08-29, branch `ren-temp-001` (base `safety/consolidated-v2` @ 96bcc6e5).
This document supersedes the externally-authored REN-TEMP-001 draft. The phase ordering of that
draft was sound and is kept; everything below that differs from it is a correction grounded in
the actual renderer source, with file:line evidence. DLSS 5 remains TRACK ONLY.

Verdict: **feasible**. The renderer's existing shape is unusually favourable in three places
(the scene→UI seam, the exposure texture, GPU skinning) and unfavourable in three others
(a single global resolution, an identity-free immediate draw path, and an explicit
"MSAA + no TAA" design pin). One issue is not technical at all: licensing.

Owner direction (2026-08-29): the primary goal is **performance headroom**, with DLSS as the
quality/perf target on the owner's NVIDIA RTX 3070, and DLSS 5 adopted later once its public
custom-engine SDK exists. Builds are private (private mirror, no published releases). All open
decisions in this document are resolved accordingly — see §6.

Performance expectation, stated honestly up front: upscaling only buys frame time in
**GPU-bound** scenes. Measured history here says stock worlds at high resolution are
GPU-bound and grass-dominated (good candidates), but some scenes are CPU-bound (a measured
case had the GPU at 8.9% of the frame) — those gain nothing from DLSS and need other work,
which is out of scope for REN-TEMP-001. Phase 0 exists to separate the two before any
architecture work is justified by assumed FPS.

---

## 0. Corrections to the original draft

1. **Licensing (resolved 2026-08-29 — private-build posture).** This repo is GPL-3.0-or-later
   with Section 7 additional terms (`LICENSE:1-3`). The DLSS SDK/NGX binaries are proprietary
   and GPL-incompatible (likewise XeSS; FSR 3.1 is MIT and fully compatible). GPL obligations
   trigger **on distribution only** — and this project is a private mirror with no published
   releases, so DLSS may be **linked directly into `wgpu_renderer.dll` behind a Cargo feature
   that is OFF by default** (`dlss`, plus a matching CMake option). The GPL cdylib must always
   build and run with the feature off and no vendor SDK present.
   Note also that the renderer is not single-author (52 commits by Paavo Huhtala in
   `engine/WgpuRenderer/`), so no part of it can be unilaterally declared non-GPL.
   **Standing gate:** before any *public* release or binary distribution that includes DLSS,
   this decision must be revisited — the vendor-neutral `TemporalUpscaler` boundary (§Phase 13)
   is deliberately kept clean so that moving DLSS out into a separately-distributed plugin DLL
   is a packaging change, not a redesign.
2. **Integration path: direct NGX Vulkan, not Streamline.** The renderer is a Rust wgpu 29
   cdylib (`rust/Cargo.toml:16`); Streamline is a D3D-oriented C++ interposer and a poor fit.
   The proven path for wgpu is hal interop + the NGX Vulkan API (as used by Bevy's DLSS
   integration): `wgpu::Instance::from_hal` / `Adapter::create_device_from_hal` with the
   extension lists from `NVSDK_NGX_VULKAN_RequiredExtensions()`. wgpu-hal is already reachable
   with **no Cargo change** (`wgpu` re-exports `wgpu_hal`; `Device::as_hal` etc. are available
   on native). What must change: instance/device creation at `rust/src/lib.rs:709-907` becomes
   the hal route, and the backend must be **forced to Vulkan** when a temporal upscaler is
   requested (today it is env-seeded auto-select, `lib.rs:719`; Vulkan is what gets picked on
   the dev box, but nothing guarantees it).
3. **Motion vectors: the draft underestimates the identity problem.** The ABI is hybrid:
   a retained instance path with stable slots (`wgr_instance_add`, `include/wgpu_renderer.hpp:1841`)
   that is **opt-in and off by default** (`WGR_GPU_DRIVEN`, `EngineWgpu.cpp:1050-1052`), and an
   immediate path (`WgrFrame.draws3d`) rebuilt from scratch every frame
   (`EngineWgpu.cpp:1523-1536`) whose `WgrDraw3D` has **no object identity at all**
   (`ffi.rs:165-171`, 272-byte layout pin at `ffi.rs:1446`). Vehicles, units and all animated
   characters go through the immediate path today. Rigid/skinned motion vectors therefore
   require an ABI change: a stable `object_id` on `WgrDraw3D` (C++ has the `Object*` to key it,
   `EngineWgpu.hpp:647`). This is Phase 8's first task, not an afterthought.
4. **MSAA + A2C interactions the draft missed.**
   - Keeping 4× MSAA in front of the upscaler is confirmed workable: color resolve is already a
     dedicated pass (`lib.rs:1685-1709`), and **single-sample depth resolves already exist**
     (custom near/far passes, `gfx3d/mod.rs:4443-4451` — WebGPU has no hardware depth resolve).
   - But the "no TAA" assumption is *pinned by tests*: `gfx3d/mod.rs:1491-1519` asserts the GTAO
     shader contains no `frame_index`/`jitter`, and shader comments state the constraint
     (`gtao.wgsl:8`, `godrays.wgsl:102`). These pins must be consciously retired, and the
     static-noise passes (GTAO dither with no history, god-ray IGN) re-evaluated under jitter —
     static noise that was invisible under MSAA becomes crawling shimmer to a temporal upscaler.
   - A2C stipple patterns under sub-pixel jitter are a known shimmer source; vegetation may need
     the reactive mask earlier than the draft's Phase 11 suggests. Validation, not redesign.
   - At 1× MSAA, A2C silently disables (`gfx3d/mod.rs:3471-3474`) — the draft's rule "do not
     start with a 1× path" is confirmed and stays.
5. **The render/output split is wider than "create RenderExtent".** `config.width/height` is the
   *only* notion of size and is consumed in ~20 places (HDR/depth/normal/underwater/DoF targets,
   bloom/exposure/god-ray/GTAO/planar divisors, 2D screen uniform, screenshots, water-visibility
   coverage). Two couplings the draft missed entirely:
   - **Resolution-governed LOD**: `far.set_view_metrics(config.height * …)` (`lib.rs:3073`) and
     `_lodInvWidth` (`EngineWgpu.cpp:2500-2511`) must be fed **OutputExtent**, or lowering render
     scale visibly coarsens object/grass LOD and corrupts every A/B comparison.
   - **Scene-phase 2D**: rain streaks, sky quads, horizon are drawn *into the HDR scene target*
     in pixel space via `globals.screen` (`lib.rs:2243-2245`, `gfx2d/shader.wgsl`). That uniform
     must be RenderExtent for the scene phase and OutputExtent for the display phase.
   - Good news: the engine already has a render-scale option with UI (Off/125/150/175/200%,
     `GraphicsPage.cpp:495-501`, `GraphicsConfig.hpp:88`) that WGPU simply ignores —
     `Engine::SetRenderScale` is a no-op virtual (`Engine.hpp:460`) with only a GL33 override.
     Implement the `EngineWgpu` override and extend the range below 100% rather than inventing a
     parallel setting.
6. **The seam and the exposure input already exist.** `WGR_CMD_RESOLVE`
   (`EngineWgpu.cpp:8288-8294`, emitted from `World.cpp:1732`) already separates the HDR scene
   from post-tonemap UI, which is drawn by a separate 1× pipeline set straight to the swapchain
   (`gfx2d/mod.rs:183`). The upscaler slots inside `run_tonemap` (`lib.rs:1671`) after the MSAA
   resolve. Auto-exposure already produces exactly what NGX wants as the exposure input: a 1×1
   `R32Float` texture (`exposure.rs:35-40`), consumed in `tonemap.wgsl:129-130`.
7. **Skinning is GPU-side, which changes Phase 9 for the better.** A compute skin-bake writes
   posed vertices into a mega vertex buffer pre-pass (`lib.rs:2254-2258`,
   `gfx3d/skin_bake.wgsl`); bone palettes arrive per frame in `WgrFrame.palette`
   (`EngineWgpu.cpp:2686-2719`). Skinned motion vectors = double-buffer the bake output and read
   previous baked positions — no per-bone previous palette needed in the vertex shader. The
   blocker is again identity: `palette_slot` is a per-frame arena index in submission order.
   The CPU-skinning fallback (`RtAnimation.cpp:1008-1025`) stays reactive-masked, not motion-vectored.
8. **Camera state facts for Phases 4/6.** Rendering is **camera-relative** (view translation
   stripped, `EngineWgpu.cpp:2470-2476`) with **reversed-Z, infinite far**
   (`EngineWgpu.cpp:2477-2489`; clears to 0.0, `GreaterEqual`). Static-world reprojection must
   carry the per-frame `cam_pos` delta explicitly, and DLSS must be initialised with the
   depth-inverted flag. Multiple cameras per frame exist (HUD/optics push their own,
   `WgrDraw3D::camera` indexes them) — jitter applies to the scene camera only.
9. **Baseline (Phase 0) pitfalls from this project's own history** (memory-verified):
   - `farfield-bench` defaults to 800×600 — always pass explicit resolutions, or GPU-boundness
     is overstated at exactly the question Phase 0 asks.
   - `--capture-metrics` GPU timer regions persist: one-shot passes look per-frame; only the GPU
     frame total is re-measured. Read upscaler cost from the frame total delta, not a region.
   - `--dev-map` has an empty retained set — measure on real worlds (Everon/Malden, and the
     grass-dense scenes, since the GPU frame is grass-dominated on stock worlds).
   - Some scenes are CPU-bound (a measured case had the GPU at 8.9% of frame). The draft's rule
     stands and is sharpened: report *per scene* whether it is GPU-bound at 1440p/4K before
     claiming any upscaling win.
10. **Mode switching**: `sample_count` and the target set are fixed at startup
    (`lib.rs:990-1014`, no runtime path). It is acceptable for Native↔Temporal mode changes to
    require a renderer restart in the first implementation; do not build live switching yet.

---

## 1. Architecture

```
Poseidon C++ (EngineWgpu)          Rust cdylib
─────────────────────────          ─────────────────
render-scale option (existing UI)  RenderExtent/OutputExtent split
explicit reset events              temporal camera state + jitter
object_id on WgrDraw3D             velocity pass + debug views
prev-pose palette upload (none —   TemporalUpscaler trait:
  bake is renderer-side)             - Native (passthrough)
                                     - TemporalDebug (in-tree)
                                     - DLSS (in-tree, cargo feature `dlss`,
                                             OFF by default; NGX Vulkan)
                                     - FSR 3.1 (in-tree, MIT — follow-up)
                                     - XeSS (later, same posture as DLSS)
                                   hal-route device creation
                                   (Vulkan forced when temporal)
```

Rules kept from the draft: no vendor types outside their backend module; native 4× MSAA path
remains authoritative and untouched; unsupported hardware falls back cleanly; renderer history
is renderer-owned (never mutate gameplay transforms); no Frame Generation work.

Placement decision (draft §22, resolved against the real chain in `run_tonemap`):

```
scene (4× MSAA HDR, jittered) → MSAA color resolve            @ RenderExtent
                              → [existing near/far depth resolves] @ RenderExtent
                              → velocity pass                  @ RenderExtent
                              → TemporalUpscaler               → OutputExtent
                              → underwater / DoF / bloom / exposure-adapt / tonemap  @ OutputExtent
                              → UI / dev overlay               @ OutputExtent (unchanged)
```

- God rays now composite into the scene target *before* transparents (commit 2ebd00f6,
  `lib.rs:4206-4211`) — they are scene content and upscale with it. No move needed.
- Exposure **metering** may stay on the render-res resolved HDR (cheaper, same average); the
  1×1 exposure texture is also handed to the upscaler as its exposure input.
- DoF/bloom/tonemap move after the upscaler (output res). Underwater compositing follows DoF's
  placement; evaluate in Phase 2, it reads the far-depth resolve which stays at RenderExtent.
- UI/HUD is already after the seam; nothing moves.

## 2. Phases (renumbered only where the draft was wrong)

**Phase 0 — Baseline.** As drafted (1080p/1440p/4K, scene matrix), with §0.9's pitfalls as
hard requirements. Deliverable: per-scene GPU/CPU-bound table. Gate: at least the grass/forest
scenes must be GPU-bound at 1440p+ or the project stops per draft §32.

**Phase 1 — RenderExtent/OutputExtent split.** Introduce `render_size` vs `output_size` in
`Renderer`; thread through every `ensure_*` and divisor consumer (§0.5 list). Implement
`EngineWgpu::SetRenderScale` (range extended to 0.5–2.0; ≥1.0 keeps GL33-style SSAA semantics —
actually ≥1.0 stays unsupported on wgpu for now, only ≤1.0 is in scope). Fixed presets
100/77/67/58/50%. Feed LOD metrics OutputExtent (§0.5). Scene-phase 2D `globals.screen` =
RenderExtent, display-phase = OutputExtent. Temporary bilinear upscale pass before tonemap for
validation. Screenshots need no work (swapchain readback, `lib.rs:4425-4449`).

**Phase 2 — Formalised resolve + vegetation regression.** As drafted. The resolve chain already
exists; the work is asserting it at RenderExtent and A/B-capturing vegetation (stock CWA,
TreeAdv, grass, A1/A2/OA sets) via the mission-capture smoke harness at 100% and 67%.
Abort condition unchanged.

**Phase 3 — Mip LOD bias.** `log2(render_width / output_width)` applied to the shared 3D
samplers; surfaced in the dev panel. Acceptance as drafted.

**Phase 4 — Temporal camera state + jitter.** Renderer-side (Rust) per-scene-camera:
current/previous view, proj, unjittered proj, jitter, extents, `cam_pos` (camera-relative —
§0.8). Jitter (Halton 2,3, replaceable) applied when building the camera UBO in Rust so the
C++ matrices — and therefore gameplay, culling truth, picking, AI — stay unjittered by
construction. GPU-driven cull may keep the unjittered matrix (sub-pixel error is irrelevant to
conservative bounds). HUD/optics cameras are never jittered.

**Phase 5 — History reset contract.** As drafted. Carrier: new fields on `WgrFrame` (the ABI is
version-guarded — `wgr_abi_version`/`wgr_abi_layout_hash`, `ffi.rs:1592-1636` — bump both).
C++ raises explicit reset events (mission load, teleport, cut, FOV jump, mode/scale change);
renderer surfaces the last reset reason in the dev panel.

**Phase 6 — Static-world motion vectors.** Camera-only velocity from the existing single-sample
depth resolve + prev/current unjittered VP + `cam_pos` delta. Covers terrain and all static
objects with zero per-object data. Output: RG16Float velocity target at RenderExtent.

**Phase 7 — Velocity debug views.** As drafted (velocity, zero-motion, camera-motion,
disocclusion inspection) in the dev panel alongside the existing debug surfaces.

**Phase 8 — Rigid motion.** First task: add stable `object_id` to `WgrDraw3D` (keyed off the
C++ `Object*`; ABI bump, size pin update at `ffi.rs:1446`) — §0.3. Renderer keeps
`object_id → previous world` (camera-relative rebased). Retained-path instances use their slot
(`gpu_driven.wgsl:398` already exposes it). Draws without id fall back to camera velocity.

**Phase 9 — Skinned motion.** Double-buffer the skin-bake output; velocity pass reads previous
baked position for skinned draws (§0.7). Requires stable identity from Phase 8 to associate
bake regions across frames. CPU-skinned fallback path: reactive mask, no false vectors.

**Phase 10 — Difficult content.** As drafted (prefer reactive/no-history over false vectors).
Known local specifics: FFT water displacement (no per-pixel motion — reactive), wind-swayed
foliage (vertex sway in shader — start reactive, revisit), clouds (froxel march — reactive),
rain/particles (reactive), planar-reflection surface (inherits water). The static-noise passes
(GTAO dither, god-ray IGN) are re-evaluated here under jitter — animate their noise per frame
*only* once a temporal accumulator exists to integrate it (§0.4).

**Phase 11 — Reactive/history-control data.** As drafted, engine-owned semantics. Likely first
producers: A2C foliage coverage, water, particles/smoke, rotor discs, muzzle flash emissives.

**Phase 12 — Temporal debug resolve.** As drafted: minimal reproject + reject + visualise, a
diagnostic, not a product TAA. Retire the "no TAA" test pins consciously here (§0.4).

**Phase 13 — TemporalUpscaler abstraction.** As drafted: a Rust trait, in-tree;
Native/TemporalDebug as the first two impls. No vendor type may appear outside its backend
module — this boundary is what later makes both the plugin split (§0.1 standing gate) and a
DLSS 5 backend a contained change.

**Phase 14 — DLSS SR (in-tree, feature-gated).** Direct NGX Vulkan (§0.2), behind the
off-by-default `dlss` cargo feature: forced Vulkan backend, hal-route device creation with
`NVSDK_NGX_VULKAN_RequiredExtensions()`, hardware/feature detection, clean fallback to Native
when unsupported or the feature is compiled out, Quality/Balanced/Performance queried not
hard-coded, depth-inverted + HDR + MV-scale flags set per §0.8. Input validation checklist
from draft §21 unchanged and mandatory: validate every input independently (MV orientation/
scale/convention, jitter sign, depth convention, exposure, extents, reset) before judging
image quality; never tune sharpening to hide a wrong input.

**Phase 15 — DLAA + validation.** Conservative 4×MSAA+DLAA comparison config; full §24
image-quality matrix (slow pan, fast rotation, fences, grass in wind, rotor blades, night
lights, camera cuts…); §25 benchmark matrix with CPU/GPU-bound reporting per scene.

**Phase 16 — FSR 3.1 SR (follow-up).** MIT, in-tree unconditionally (no feature gate needed),
all vendors. Must consume the identical `TemporalUpscaler` contract; if it needs renderer
changes beyond its own backend module, the abstraction failed — review it before continuing.
XeSS later still, same feature-gate posture as DLSS.

**DLSS 5 (TRACK ONLY — unchanged).** No speculative API work, no effort estimates from
announcement material. When the public custom-engine SDK exists, it enters as another backend
behind the same trait; the NR-intensity / colour-grade / masking controls from the owner's
notes are exposed through the dev panel *as the SDK defines them*, with 0% preserving the
conventional output and neural rendering never on by default. Nothing in REN-TEMP-001 may
hard-code assumptions that would block this. Note: whether specific DLSS 5 features run on an
RTX 3070 is unknown today — do not promise them for this hardware.

## 3. Stop conditions

All of draft §32, plus:
- **Licensing**: any plan to publish releases or distribute binaries while DLSS/XeSS is
  compiled in — stop and execute the plugin split from §0.1 first.
- **ABI**: if `object_id` on `WgrDraw3D` cannot be added without breaking the 272-byte layout
  discipline or the version handshake, stop and report rather than keying off `(mesh, offset)`.
- **hal interop**: if `Instance::from_hal`/`create_device_from_hal` cannot reproduce the current
  device (features/limits at `lib.rs:866-907`) bit-for-bit for the *native* path, the hal route
  must be conditional on temporal mode, never the unconditional default, until it can.

## 4. First implementation slice (updated)

Unchanged in spirit from draft §35 — measure, split resolutions, keep MSAA, resolve, bilinear
upscale, native UI, mip bias, validate vegetation — with these concrete anchors:

1. Phase 0 baseline with §0.9 pitfalls respected (explicit resolutions, real worlds).
2. `render_size`/`output_size` in `Renderer` + `EngineWgpu::SetRenderScale` override.
3. LOD metrics pinned to OutputExtent *in the same commit* as the split (§0.5).
4. Scene-phase vs display-phase `globals.screen` split.
5. Bilinear upscale pass between MSAA resolve and the post chain in `run_tonemap`.
6. Mip bias from the scale ratio.
7. Vegetation A/B captures at 100% vs 67% via the capture harness.

STOP and report after this slice, per the draft. Do not start jitter, motion vectors, or any
vendor SDK work before the review.

## 5. Milestones

Draft §33's lettering kept (A baseline … L abstraction, M DLSS SR bring-up, N DLAA+quality
modes, O validation/performance report); FSR and XeSS are separate follow-up milestones after
O. Definition of Done: draft §34 items 1–20, plus item 21: "the cdylib builds and runs with
the `dlss` feature off and no vendor SDK present on the machine."

## 6. Decisions taken on the owner's behalf (2026-08-29)

The owner's stated goals: more performance now; DLSS 5 eventually, for better graphics.
Resolved accordingly:

1. **Private-build posture** — DLSS linked in-tree behind an off-by-default cargo feature;
   plugin split deferred until public distribution is ever planned (§0.1). The `TemporalUpscaler`
   boundary keeps that door open at packaging cost only.
2. **DLSS before FSR** — the owner's GPU is an RTX 3070 and DLSS is the stated target; FSR's
   earlier ordering existed for legal reasons that the private posture removes. FSR remains a
   committed follow-up (all-vendor fallback + abstraction validation).
3. **Restart-on-mode-switch accepted** for the first implementation (`sample_count` and target
   sets are fixed at startup; live switching is deferred).
4. **Quality presets** map render scales the standard way: Quality 67%, Balanced 58%,
   Performance 50%; DLAA = 100%. Exact mode availability is queried from the SDK at runtime.
5. **Performance honesty rule** — every reported FPS gain names whether the scene was GPU- or
   CPU-bound at that resolution. CPU-bound scenes are explicitly out of scope here; if Phase 0
   shows the owner's actual play scenarios are predominantly CPU-bound, stop and report per
   §3 instead of continuing on momentum.
6. **The user-visible endpoint** is the existing Options render-scale control plus a Temporal
   section in the dev panel (draft §29, built incrementally, never ahead of the feature).
