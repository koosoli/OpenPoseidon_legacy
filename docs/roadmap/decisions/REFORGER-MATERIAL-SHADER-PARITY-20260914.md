# Reforger material/shader parity: audit and investigation plan

Date: 2026-09-14. Owner brief: match native Reforger rendering by fixing
material/shader/lighting semantics, never by artistic fudge factors.

## 1. Scope and provenance

- Active work line: `snowline-jitter-001` @ `12061d7a` (worktree
  `.claude/worktrees/farvisible`). Installed game build is this commit
  (stamp verified). Trunk push (`ship-trunk` merge into
  `safety/consolidated-v2`) interrupted mid-build, NOT pushed, parked.
- Reference: owner screenshots of original Reforger vs OpenPoseidon at
  freefly [4665.50, 7132.80, 192.09] az 123.7 el -11.8, 16:00 (Everon
  church/village). Geometry placement matches; vegetation/buildings/
  terrain/atmosphere response differs strongly.
- Other-AI worktree `.claude/worktrees/asset-obs` (branch `asset-obs-001`
  @ `7d871e92` + uncommitted changes) reviewed read-only below. Nothing
  there was modified. Its committed VBSmallDiscardable result (path
  inactive in tested scenes, diagnostics only) is accepted as evidence;
  its uncommitted native/far/culling work is judged hunk-by-hunk below.
- Prior art consulted (not re-litigated): `UPSTREAM-CROSS-AUDIT-20260913`,
  `HANDOVER-20260815-reforger-renders`, `HANDOVER-20260823b-everon-renders`,
  `HANDOVER-20260826`, `compat-c1-capability-matrix`, `SEPTEMBER9-VISUAL-
  FOLLOWUP` (evidence index).

## 2. Prior measured findings (do not re-guess these)

- `BCRMap` = albedo, `OpacityMap` = cutout. Reforger `NTCMap`/`NMOMap` are
  NOT plain tangent normal maps and are still unwired
  (`HANDOVER-20260815-reforger-renders:280-295`).
- WGPU terrain ignores rvmat `ambient`/`diffuse`; measured albedo-to-pixel
  transfer is ~3.0x and compressive (same source).
- Granite albedo measured identical before/after material work while the
  image stayed wrong: "start at the lighting/exposure path, not the
  materials" (`HANDOVER-20260823b:261`).
- `EmatMaterialAdapter` does NOT map `GeometryAO*`, `EllipsoidAO*`,
  `WorldColorizeMap`, SSS (`UPSTREAM-CROSS-AUDIT` Reforger section).
- Forest-leaf-light ablation did NOT solve pale foliage; global legacy
  foliage-light ablation must not ship (same source).
- BCR alpha is roughness (AST-016 series); DayZ `TreeAdv` colorization
  binds raw albedo; RVMAT-`ambient`-applied-literally tested (see handover
  20260815-next-session:120).
- Auto-exposure default-on; suspected exposure regression vs `aa24c32`;
  Hable curve at exposure 3.625 noted (`HANDOVER-20260813:182`).
- God rays driven by sky Mie + overcast + fog, 1200 m aerosol falloff.
- Baked-LOD `MatPBRBasic` flat-`Color` materials deploy a colour-only
  material (MAT-054).

## 3. Core question A-M: initial status (evidence, not verdict)

| # | Area | Status | Evidence / next probe |
|---|---|---|---|
| A | Texture channels | PARTIAL (BCR/Opacity/NHO-ok; NTC/NMO unwired) | wire NTC/NMO per actual convention, verify sRGB/linear per channel |
| B | Material semantics | PARTIAL (adapter gaps above) | map or explicitly reject GeometryAO/EllipsoidAO/WorldColorize/SSS |
| C | Normals | UNKNOWN (suspect legacy rounded-foliage leak, handedness) | Phase 1 normal views on control tree |
| D | Foliage shading | INCORRECT (pale, flat; ablation failed) | Phase 3 isolation ladder |
| E | Roughness/specular | PARTIAL (BCR-alpha known; response unverified) | specular debug view on control assets |
| F | AO | PARTIAL (ground NHO ok; authored _AS: no path) | count AO applications (0/1/2) via views |
| G | Sky/environment light | UNKNOWN (ambient suspected too strong) | sky-only vs sun-only views |
| H | Direct sun | UNKNOWN (color/intensity unverified) | sun-only view + albedo reference |
| I | Shadows | PARTIAL (CSM + contact work recent; darkness unverified) | shadow-term view |
| J | Atmosphere/haze | UNKNOWN (froxel exists; distance response unverified) | fog-contribution view + range ramp |
| K | Exposure/tonemap | SUSPECT (3.0x compressive; regression suspected) | pre/post-tonemap views, fixed-exposure A/B |
| L | Terrain blend/detail | PARTIAL (LCA + native ground exist; macro/detail normals unverified) | Phase 5 layer isolation |
| M | LOD materials | PARTIAL (flat-Color handled; far tiers unmeasured) | compare LOD ladder per asset |

## 4. Asset-obs work review (read-only diff vs `snowline-jitter-001`)

Committed diagnostics (VB lifecycle census, `7d871e92`) already in ancestry:
ACCEPTED as evidence. Uncommitted copies of my sim/snow files: stale,
identical-or-older, IGNORED.

ADOPT (Phase 1 debug-view stack + safe fixes; no behavior change at defaults):
- `frame.wgsl`: `veg_sway_phase` (legacy `leaf>=0` path bit-identical) +
  `MATDBG_VIEW_* 0-6` stable IDs.
- `gpu_driven.wgsl`: matdbg views 3-6 (AO/spec/gloss/UV), specular_debug,
  constant-ID renumber (default FULL falls through). Sway line excluded.
- `shader3d.wgsl`: global matdbg with per-draw-lane fallback (default FULL
  = old behavior), specular constants, UV view (blue=0, no uv1 lane).
- `gfx3d/mod.rs`: shared-ID test only.
- `shaders/mod.rs`: `veg_sway_phase` asserts (gpu assert excluded, see below).
- `EngineWgpu.hpp`: 1:1 rawView passthrough (fixes unreachable views 3-6).
- `Engine.hpp`, `ffi.rs`, `lib.rs`: comment-only ID documentation.
- `DebugOverlay.cpp`: views 0-6 combo + honest Screen-AO help.
- `EngineWgpu.cpp`: debug env aliases + drain timing/counters logging.
- `ObjectStreamPrepare.cpp`: native-gated EMAT texture noting (early-return
  branch preserves loose-PAA path).

DEFERRED (needs measurement or out of scope for Phase 1):
- RFG-104 far-proxy tier, authored-far residency, satmap build+bind,
  far-distance follow, `SceneFarObjectCreated` path, `far.wgsl` sun flip,
  cull.wgsl sub-pixel 0.5 + bias bit + `authored_far_level`, gpu sway line,
  terrain.wgsl native crossfade distances, MODERNISATION doc delta.
- Rationale recorded per hunk in session notes; each needs its stated
  capture (coverage/cost/pop-in) before adoption.
- Explicitly NOT ported: hardcoded fallback colors/palette defaults stay
  out; hardcoded coverage/perf knobs stay out.

## 5. Phase 1 plan (in progress)

Port the ADOPT set above into `snowline-jitter-001`, adjusting:
- `shaders/mod.rs`: drop the `gpu.contains("veg_sway_phase(phase_xz, leaf)")`
  assert (deferred sway line), keep the rest.
- `gfx3d/mod.rs`: port only `material_debug_views_are_shared_by_both_object_paths`;
  skip `authored_far_lod_preserves_small_coarse_silhouettes` (asserts deferred cull).
Then: Rust gfx3d tests, full build, deploy, verify all 7 views on the Everon
church spot via fixed-camera captures (views must show distinct, sane output;
FULL must be pixel-comparable to pre-change).

Pre-existing failure (NOT introduced by this port, NOT fixed here):
`gfx3d::normal_map_cavity_only_darkens_the_ambient_and_is_inert_on_a_flat_map`
fails at base `12061d7a` because `shading.wgsl:501` ends the sun term with
`* sun_vis * leaf_cavity` while the test requires `* sun_vis;` — i.e. the
shader currently touches the direct sun term the test forbids, on the foliage
path. Neither file is touched by the Phase 1 port (verified via `git status`).
Flagged as an open Phase 3 (foliage) question: is `leaf_cavity` on sun
intended or a regression? Do not weaken the test to make it pass.

## 6. Control assets (selected after Phase 1 verifies)

- Building: church at the Everon comparison spot (visible in both screenshots).
- Tree: large deciduous tree near the church (foreground right in ours).
- Exact asset/material IDs to be recorded here once the debug views are live.
- Trace per asset: source material -> loader/importer -> internal material ->
  CPU state -> GPU bindings -> shader -> framebuffer.

## 7. A/B route (to be fixed before Phase 2 captures)

Fixed camera/pose/FOV/time-of-day(16:00)/weather/exposure/resolution/AA/scale
via existing capture harness (`farfield-bench`/movecam scripts + fixed LOD
governor). Views: building close-up, tree close-up, village mix, mid vegetation,
long landscape. No hand-positioned cameras.

## 8. Guardrails (from owner brief, enforced)

- No darkening textures, no ambient cuts, no AO boosts, no sun/exposure tweaks
  to match screenshots. No source-texture edits. No baked per-asset factors.
- Native-only branches keyed on asset/material origin; legacy OFP/Arma1 output
  covered by existing regression tests plus before/after captures on a legacy
  scene for every rendering change.
- One mismatch per change: demonstrate, cause, test, fix, capture, verify.
- Perf: mean/median/p95/p99 over repeated runs; no FPS claims from single samples.

## 9. Open questions / next actions

1. Execute Phase 1 port + verify views (this session).
2. Select control assets, run Phase 2 trace.
3. Strongest pale-vegetation suspects to test first: sky-ambient strength,
   legacy rounded-foliage normals on native assets, NTC/NMO unwired (flat
   shading), exposure transfer. In that order only if views implicate them.
4. Trunk ship (`ship-trunk` -> `safety/consolidated-v2`) remains PARKED until
   the owner reprioritizes; installed build stays `snowline-jitter-001`.

## 10. Phase 1 verification (2026-09-15, build 5b4db766)

All 7 views captured valid at the Reference spot (Everon church,
[4665.50, 7132.80, 192.09] az 123.7 el -11.8, 16:00, 1920x1080, MSAA4,
no DLSS, fixed LOD): `build/native-reforger-perf/Reference-matdbg-*/run-01.png`
(~22.3k instances, ~15M tris, ~36-49 ms frames — perf numbers, not claims).
Each view is distinct and sane; FULL matches the pre-change framing.

Findings (evidence, not fixes):
- BASE (first capture): VOID — see section 11. Do not cite it.
- BASE (re-captured after the decode fix, build 94c2065f): tree crowns render
  deep saturated green, matching the authored BCR file pixels (opaque mean
  58,69,40). Birch trunks white, roofs red, container blue. ALBEDO SIDE
  EXONERATED: the bound albedo is correct; no tint/colorize/sRGB defect on
  this asset. WorldColorizeMap absence only costs per-instance variation.
- LIGHTING (white albedo): foliage multiplier ≈1.0 with no variation while
  buildings model sun/shade correctly. Lighting-side blowout is
  tree-specific and valid evidence (white needs no decode). Pale foliage =
  correct albedo × flat ~1.0 lighting. THE isolated cause is in the foliage
  lighting terms, not the textures.
- NORMAL (map viz): flat almost everywhere = few/no normal maps bound on
  native assets. Consistent with NTC/NMO still unwired.
- AO: foliage ≈1.0 (no occlusion term anywhere on trees).
- SPEC: ≈0 everywhere. Specular ruled OUT as the paleness cause.
- UV: sane tiled coordinates, no catastrophe.
- Pre-existing (recorded, not introduced): cavity test failure
  (`leaf_cavity` on the sun term vs test invariant) stays an open Phase 3
  question.

Phase 2/3 attack order from this evidence: (1) legacy rounded-foliage
normals leaking onto native assets (would explain flat ~1.0 lighting),
(2) sky-ambient strength on foliage, (3) albedo tint/colorize path
(`WGR_LAZY_BCR_TINT=0` A/B), (4) `leaf_cavity`-on-sun semantics.
No fix is authorized by these captures alone; each needs its control-asset
trace from section 6 first.

## 11. Method note: the base view lied (fixed 2026-09-15)

Albedo textures upload as Unorm (no GPU sRGB decode); `shade()` decodes
under the HDR `linear` flag and the output encodes again. The BASE_COLOR
early return bypassed `shade()`, so raw gamma values were encoded twice and
everything read pale — most visibly on dark saturated greens like leaf
albedo. The Tilia BCR file itself decodes (via PoseidonTools) to a rich dark
green (opaque mean 58,69,40); transparent texels are dark too (49,58,32), so
white-fringe is also ruled out. Both object paths now decode the base view
under the HDR flag, mirroring `shade()` exactly: the view shows what the
shader consumes. Re-capture base before citing albedo values; the earlier
base capture's ABSOLUTE brightness is void (relative hue differences stand).
The LIT view was always valid (white needs no decode).

## 12. Foliage A/B captures (2026-09-15, Reference spot, 16:00)

- `foliage-ablate` (`WGR_FOLIAGE_ABLATE=1`, leaf model off): trees gain real
  light/shade modeling and read volumetric, far closer to the reference.
  CONFIRMS the leaf-lighting model (SSS fill + wrap + ambient boost +
  spherical bends) causes the flat-bright canopy. Ablate-all is not
  shippable (harsh terminators, no translucency) — it bounds the problem.
- `foliage-screenano` (`WGR_FOLIAGE_SCREEN_AO=1`): no visible change vs
  baseline. Missing screen-space occlusion is RULED OUT as the cause.

## 13. Granular attribution (2026-09-15, Reference spot, 16:00)

- `foliage-boost1` (`WGR_FOLIAGE_AMBIENT_BOOST=1.0`, new knob): crowns regain
  light/shade modeling, close to the reference. DOMINANT term confirmed.
- `foliage-bend0` (`WGR_FOLIAGE_TREE_BEND=0`, new knob): slight structure
  gain, still brightish. Secondary; deferred as follow-up.
- Fix shipped on `parity/foliage-lighting-001`: native Enfusion worlds push
  ambientBoost 1.0, legacy worlds keep configured values (branch keyed on the
  existing `GetEnfusionSurfaceCount()>0` native signal; no shader/ABI change,
  uniform-only so no perf delta to measure). Env override still wins for A/B.
  This is the per-generation split `FoliageSettings` anticipated, not a global
  ambient cut: legacy output is bit-identical by construction (branch dead on
  legacy worlds).

## 14. NTC contradiction resolved (2026-09-15, no capture needed)

Asset-obs "NTC bound" vs diagnostic "flat normal response": BOTH TRUE, no
contradiction. The Tilia NTC file is rich (rainbow leaf normals; channel std
31-53, no flat channel), the adapter maps NTCMap->NormalMap, the section
resolve binds it (log: 441 bound incl. MatPBRTreeCrown polyplanes), and a
full-resolution crop of the normal view shows mottled per-leaf variation.
The earlier "flat" reading was a downsampling artifact: high-frequency normal
detail averages to lavender at reduced scale. Lesson: judge normal views only
at full resolution. Consequence: per-pixel normals VARY, yet lighting stays
flat — the wash happens downstream (foliage model terms), which is exactly
what the wrap/transmission/bend A/Bs isolate next.

## 15. Fix verification (2026-09-15, build 0bd80d2e)

`Reference-foliage-nativeboost` (no overrides — the shipped default path):
crowns show light/shade modeling with real depth, close to the reference.
The per-generation split works end to end. Legacy worlds provably keep old
values (branch dead without Enfusion surfaces); uniform-only change, no
perf delta.

## 16. Wrap/transmission arms + leaf_cavity resolution (2026-09-15)

- `foliage-wrap0` (`WGR_FOLIAGE_WRAP=0`): no significant change at 16:00 high
  sun. Expected: `wrap_fill` ≈ 0 on lit sides by formula. Wrap is NOT a
  contributor at issue angles; keep 0.5 for grazing/backlit geometry.
- `foliage-trans0` (`WGR_FOLIAGE_TRANS_SCALE=0`): no significant change here.
  Transmission is dark-side/backlit only. Keep 0.54.
- Stacking verdict: no problematic stacking at issue angles. The wash was
  single-term dominant (ambient boost); ablate ≈ boost-fix + harshness from
  lost translucency/wrap at terminators. No combined arm needed.
- `leaf_cavity` anomaly UNDERSTOOD (no fix needed): `m.native_cavity`
  defaults to 1.0; NTC-alpha fill sits behind `#ifdef NATIVE_LEAF_CAVITY`
  (off by default). At runtime the sun term multiplies by 1.0 — dormant.
  The failing test asserts source text, so it fails regardless; it guards
  the opt-in path. Pre-existing, untouched, now explained.
- GPU cost of the shipped fix: uniform-only (one float per frame). No
  measurable delta by construction; arm frame times 35–49 ms all within
  scene variance. No FPS claim made.

## 17. Asset-obs audit closure (5 areas)

- Native foliage normals: nothing beyond the adopted `veg_sway_phase`
  helper to port; no hidden normal fix exists there. The gpu sway line stays
  deferred (motion, unmeasured).
- EMAT dependency handling: adopted (`ReadNativeEmat` + texture noting).
- Material bindings: NTC→slot mapping predates both lines (verified live:
  441 bound incl. crowns). Nothing to port.
- Foliage lighting: no fix existed there; the ambient-boost split is new
  work on this branch.
- Sun-direction correction (`far.wgsl` flip): NOT RELEVANT TO THIS BRANCH
  (far pipeline only). Convention analysis supports it
  (`sl=-sun_dir` on the regular path); needs a far-distance A/B whenever
  far work resumes. Not ported.

## 18. Bend follow-up: native authored normals ship as default (2026-09-15)

- Arm `Reference-foliage-authorednormals` (`WGR_NATIVE_AUTHORED_NORMALS=1`,
  build 480cb11a, no rebuild needed): foreground crowns gain per-leaf
  structure and deeper shade pockets vs the shipped-default baseline; the
  smooth wash recedes. Authored normals read closer to the reference.
- Shipped on `parity/foliage-lighting-001`: the `NATIVE_AUTHORED_NORMALS`
  define is now default-ON (`mod.rs`: off only with
  `WGR_NATIVE_AUTHORED_NORMALS=0`). Zero shader-text change — the two gates
  already keyed on native-only signals: retained path skips bend for
  `INST_COHERENT_TREE_WIND` (set only for `.xob` names), direct path skips
  for `foliage_kind` 3 (assigned only for `.xob` names in `Object.cpp`).
  Legacy `.p3d` trees can never carry either signal, and the direct path
  keeps its CWA regression-guard bend for kinds 1/2. Legacy output is
  identical by construction — same argument class as the boost split, but
  per-instance rather than per-world.
- Merged native forests keep their bend (no coherent bit; separate question).
- Verification (build 25ca056e, deployed): `Reference-foliage-shippednormals`
  (no overrides) reproduces the experiment arm — structured crowns with
  shade depth on the default path. Owner gets this build on next launch.

## 19. Tint route A/B closes attack item (3) (2026-09-15)
- `Reference-foliage-notint` (`WGR_LAZY_BCR_TINT=0`, build 25ca056e):
  pixel-identical look vs the shipped default at the Reference spot.
- Non-vacuous: the lazy route fired (8 capped log lines) in the shipped run
  and 0 in the control — tinted textures are in frame, both loader routes ran,
  same pixels. The tint/colorize path is EXONERATED here; combined with the
  Tilia albedo match, the albedo side of the parity audit is closed.

## 20. Road/terrain snow evaluation re-shared (2026-09-15)

- The `road_and_terrain_share_snow_depth_evaluation` failure was mine: the
  snowline refactor factored `terrain.wgsl` (`snow_drift`/`snow_deficit`
  helpers) while `conform.wgsl` (road decals) kept the old inline body.
- Proved behavior-identical first (same value inside/outside the deficit
  window, same zero case), then copied the factored trio verbatim into
  `conform.wgsl`. Zero behavior change by arithmetic; the guard test is green
  again along with the full `shaders::` suite (11/11).
- Verified end to end: rebuilt, deployed, `Reference-foliage-snowshare`
  FULL capture matches the shipped look (shaders compose at runtime).
- Note: `Build.ps1` aborted twice on the Rust step with only a "Compiling"
  line — PowerShell 5.1 `$ErrorActionPreference='Stop'` turning the
  corrosion step's stderr into a terminating error, NOT a code failure.
  `cmake --build` directly exits 0. Prefer that when the wrapper dies early
  with no error text.

## 21. Terrain tint census confirms prior decisions (2026-09-15, no capture)

- Read 16 real surface `.emat` files via `pak cat` (Grass/Pebbles/Dirt/
  Asphalt/Concrete/Rock/Crop/Forest/Mountain sets).
- `Diffuse` is `1 1 1 1` wherever present: "terrain ignores diffuse" is
  HARMLESS, all identity. Closed.
- Base `Color` (0.59 concrete .. 0.88 asphalt, ignored by the renderer) is
  NOT an oversight: `TerrainWgpuEnfusionSurface.cpp` documents the RFG-073
  continuity measurement (detail-alone application rejected at 0.367 vs
  0.221 unused; unused vs both indistinguishable). Reproducing the census
  does not overturn it — absolute level vs Reforger needs ground truth the
  continuity metric cannot supply. Stays unbound. No code change.
- Still open, semantics unknown (no in-repo measurement found; RFG-065..073
  docs not present in this worktree): `MiddleBCRBlend` > 1 (Pebbles 2,
  ForestDeciduous 1.2 — clamped as interpretation), `NHOMiddleMap` dropped
  (mid-field normals go flat past detail range), `MiddleNOBlend/Power`,
  `SpecularIBL`/`DielectricReflectance` (spec ~0), `SatMapBrightness`
  (satmap paging is a project), `ParallaxScale`, `CavityFromAO`,
  `LocalShadowBias`. None gets a guess; each needs Reforger ground truth.

## 22. Far-tier sun sign: proven wrong, inert on native, NOT changed (2026-09-15)

- `far.wgsl::far_shade` computes `dot(normal, +sun_dir_world)` where every
  other path uses `sl = -sun_dir_world` (surface→light). For prism roofs/
  sun walls this is exactly inverted (roofs ambient-only, anti-sun walls
  lit). Convention proof is airtight; the asset-obs flip is correct.
- NOT fixed, deliberately: the tier is INERT on natively loaded worlds.
  `SetFarProxyInstances` has one caller (`LandSave.cpp`, converted-world
  placements); native streaming never calls it (no "Far instance tier" log
  line in any native run). `--vd 400` drops 3400 instances into nothing;
  `WGR_FAR_TIER=0` vs on is pixel-identical at ForestHigh (verified:
  mean abs diff 0.0 across sky/near/far bands).
- Verification plan (converted-world workstream, not this branch): Stratis/
  Abel scene with modern placements + `--vd` to force proxies, compare
  prism roofs/walls against the same buildings real-drawn. Until then the
  one-line flip stays out — no unverifiable changes.
- Cautionary notes from this probe: (a) the ForestHigh LIT view cannot show
  far shading — terrain and the far pipeline ignore `MATDBG_VIEW_LIGHTING`,
  so white roads there are correct (~0.84 sunlit decals) next to
  still-textured terrain, not a decal bug; (b) cloud drift between runs
  (~62% sky-strip diff) confounds naive pixel diffs — use band means and
  the log, not raw deltas.

## 23. NormalPower carried to the shader (2026-09-15, build a9e0b37b)

- Audit-driven find (§24): the `.emat` census shows `NormalPower` on 60
  crowns (0 = deliberately flat .. 3.0), 245 Multis, 62 Basics — parsed
  nowhere. The 0.0 values only make sense as an XY amplitude scale (a power
  curve has no use for 0), matching the water-ripple POWER amplitude
  vocabulary already in the shaders.
- Shipped on `parity/foliage-lighting-001`: `TranslatedMaterial.normalPower`
  (parsed, clamped ≥ 0) → `WgrModelMaterial.normal_power` (280 → 288 bytes,
  both asserts) → `SectionMaterialGpu` (+12 pad, 256 → 272 stride) →
  `sm.normal_power` in both WGSL mirrors → post-blend tangent XY scale +
  Z rebuild. Unconditional: 1.0 is bit-identical (clamp no-op on |xy|≤1,
  same sqrt), so every legacy material and every silent .emat is untouched.
  `WGR_MATERIAL_NORMAL_POWER=0` forces 1.0 for the A/B. Direct (per-draw)
  path NOT yet carried (ABI-frozen draw struct, no spare lane) — retained
  owns the world; recorded follow-up, no regression (defaults to old look).
- A/B at Reference 16:00, build a9e0b37b: `foliage-normpower` (default ON)
  vs `foliage-nopower` (forced 1.0). Crown band mean abs diff 7.66 (village
  3.88, field 1.02 noise): ON shows deep shadow pockets and separated leaf
  masses where OFF washes flat. Owner verdict pending (yes/no pictures).
- Tests: new `TranslateEmat` NormalPower case green; `shaders::` 11/11;
  material suite 27/27. Build note: the struct's u64 alignment pads 284 →
  288 — the const assert caught it, both sides updated.

## 24. Audit judgement: census script yes, framework no (2026-09-15)
- Built `emat-census.py` (temp, ~60 lines): parses all 1828 deployed +
  dumped `.emat` files, counts keys per material class → `emat-census.json`.
  This directly produced the NormalPower lead (60 crowns + 245 Multis).
- Verdict on the proposed framework: a static asset→GPU→shader pipeline
  tracer is NOT worth building — the loader's handled-key lists are small,
  stable, and already half-documented in code comments; the expensive half
  is semantics, which no automation can supply. The script + a checked-in
  status table + the existing knob/harness A/B loop is the whole system
  needed. Kept the branch name: the work is one parity line, renaming
  fragments history for taxonomy.
- Tests: `native_authored_normals...` composes both states, green. The
  `road_and_terrain_share_snow_depth_evaluation` failure is PRE-EXISTING
  (fails on the clean tree; snow snippet drift, unrelated to foliage).

## 25. Reference found + clear weather + crown AO (2026-09-15, build 82e8d29c)

- Owner pointed at inspiration/armareforger_everon-1.jpg (Reference pose,
  16:00): deep green crowns with near-black interiors, saturated fields.
  Ours (cloudy captures): +33/+25 on crowns, pale fields. Sky comparison
  proved WEATHER mismatch (ref clear, ours overcast) on top of materials.
- 	ests/perf/missions/parity_field.eden: mission copy with
  startWeather/forecastWeather 0.05, fog 0. Clear-sky captures now match the
  reference mood; all future parity A/B use it (perf mission untouched).
- trans0 under clear sky: net ZERO (means identical to 0.1, diff = sway
  displacement). Transmission is not the brightness source. Closed again.
- Baked vertex colours (xob 0x20 stream, parsed but dropped everywhere):
  alnus crown byte2 spans 0..255 in 18 levels, dark core to open rim,
  openness fit R=0.74 against the authored GeometryAO cylinder; betula grey
  words consistent with the same byte2 signal; bark/solids carry none, so
  the runtime applies to cutout sections only. xob inspect --colours
  [--colour-dump N] audits any model.
- Shipped: GeometryAO volume (center/height/width/intensity) parsed whole
  or not at all; WgrModelMaterial 288 -> 320, SectionMaterialGpu 272 -> 304
  (both asserts); VS computes the factor per-vertex from model-space pos,
  FS multiplies albedo (vertex-colour semantics); WGR_FOLIAGE_CROWN_AO=0
  forces off. Forests excluded (patch space), direct path follow-up.
- Clear-sky A/B: crown std 44.9 (off) -> 52.3 (on), reference 51.6. The
  on-run was hazier (sky +13), which lifts means but suppresses contrast,
  so the contrast gain is the AO working against the haze. Interiors gain
  ref-like pockets but are not yet near-black: radial-only, no vertical or
  ellipsoid terms. Owner yes/no pending.
- A/B rigor notes: auto-exposure is OFF by default (no confound); weather
  still random-walks around the mission value between runs (compare
  structure metrics, not raw means); sway displaces leaves run to run.
