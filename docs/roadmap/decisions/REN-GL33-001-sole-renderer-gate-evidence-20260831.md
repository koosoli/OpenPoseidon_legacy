# REN-GL33-001 — GL33 is already frozen, but CI runs on it, so the gate is not close (2026-08-31)

Roadmap **Phase 2.2, "WGPU sole-renderer gate"**. Read at `aa2eea3e` (branch
`asset-obs-001`); every count and file:line below was taken from that tree, and the build
result is from a real `cmake --build` at it. **No game was launched for this document** —
another agent held the single allowed instance. Everything that can only be settled by a
run is named as such in §3 and §5 rather than guessed at.

## Finding

**GL33 is already frozen in practice (2.1 is essentially done) and it is cheap to keep
(~10 k lines in its own target, and the shared-code coupling is 101 comment-mentions with
only a handful of real forks). What is expensive is that the project's entire automated
integration signal runs *on* GL33: 111 of 117 Trident scenarios execute `--render gl33` in
CI, all 6 WGPU-pinned scenarios are tagged `ci-host-blocked` and skipped, and no CI job
asserts anywhere that WGPU starts at all.** Deleting GL33 today would not simplify the
renderer; it would delete the test harness's only working backend and leave the project
with zero automated rendering coverage. The gate's own bullets say this: two of the eight
fail outright, and the compatibility-smoke bullet fails because those tests **do not
exist**.

Recommendation: **freeze-and-keep**, and spend the next unit of effort on making WGPU start
under Xvfb, not on removal. §4 names a genuinely safe first step that is not the backend.

## 1. What GL33 still costs

### Code that is GL33 and nothing else

| Where | Files | Lines | Note |
|---|---|---|---|
| `engine/PoseidonGL33/**` | 23 `.cpp`/`.hpp` + `CMakeLists.txt` | **10,233** | the backend proper; `EngineGL33_Shaders.cpp` alone is 1,837 lines of inline `#version 330 core` GLSL |
| `thirdparty/glad/` (`src/gl.c` + `include/glad/gl.h`) | 2 | **22,151** | vendored GL loader, compiled *into* `PoseidonGL33` (`engine/PoseidonGL33/CMakeLists.txt:15`) |
| `engine/Poseidon/Graphics/Core/GL*.hpp` | 8 | **452** | `GLBlendState`, `GLBufferMap`, `GLClear`, `GLCullState`, `GLDepthStencilState`, `GLIndexBuffer`, `GLPipelineState`, `GLSampler` — they live in *shared* Poseidon but every non-test consumer is under `engine/PoseidonGL33/` |
| GL33-referencing unit tests | 11 | **3,123** | `test_gl33_rendering.cpp` is 1,062 of them |

Total GL33-attributable: **~36 k lines**, of which 22 k is vendored third-party loader that
costs nothing to maintain. The hand-written, actually-maintained surface is **~10.7 k
lines** (backend + the 8 shared GL headers).

### Shared-code coupling

`grep -ri gl33 engine/Poseidon --include=*.cpp --include=*.hpp` returns **101 hits across
45 files**. The important number is how few of those are executable:

**Live forks that exist only because GL33 exists (6 sites):**

| Site | What it does |
|---|---|
| `World/Scene/SceneDraw.cpp:490` | `AppConfig::Instance().GetRenderBackend() == "wgpu"` — a *string* compare, per light, per frame, picking a flare alpha of 0.18 vs 0.35 |
| `World/Scene/SceneDraw.cpp:2615` | same string compare, gating the `HalfLight` preloaded shape |
| `Dev/Debug/DebugOverlay.cpp:113-118, 6577-6865` | a whole `enum class RenderBackend { OpenGL3, … }` and five `if (s_backend == RenderBackend::OpenGL3)` arms wrapping `ImGui_ImplOpenGL3_*` — the dev panel carries a second ImGui render backend |
| `Graphics/GraphicsEngineFactory.cpp:83-84, 213-214` | `GraphicsBackend::GL33` arms in `CodeForBackend` / `GetBackendName` |
| `Foundation/Platform/AppConfig.cpp:360-361` | `"gl33"` in the `--render` help string and the `CLI::IsMember` allowlist |
| `apps/cwr/Game/GameApplication.cpp:2386` + `2463-2474` | registration, plus the Auto-fallback arm |

**One layering violation:** `engine/Poseidon/World/Viewer.cpp:44` does
`#include <PoseidonGL33/TextureGL33.hpp>` — shared engine code including a header from a
backend target that `Poseidon` does not link. The include is **dead**: no `TextureGL33`
symbol appears anywhere else in the file, and the one thing it was presumably for
(`ForceReloadAll`, line 721) goes through the `AbstractTextBank` virtual. It compiles today
only because the include path is global.

**The other ~94 hits are comments and capability-virtual defaults**, and this is the part
that matters for the retire-vs-keep decision: the abstraction that lets GL33 differ is not
a GL33 abstraction. `Engine.hpp` carries ~11 "default returns no-feature" virtuals
(`GetWaterRenderer` 1627, `ProceduralSkyActive` 2111, `ResolveSceneToDisplay` 1757,
`UsesGpuSkinning` 1593, `SupportsTemporalTuning` 2696, `GetShadowMapTuning` 905, the
`OverlayVertex` path 1597…). Their *other* implementor is `EngineDummy`, which the
dedicated server, `PoseidonFormats`, `poseidon`, `pbo` and `PoseidonTools` all instantiate
(`docs/roadmap/compat-c1-capability-matrix.md`, `HEADLESS_RUNS_WITHOUT_A_GRAPHICS_BACKEND`,
VALIDATED). **Deleting GL33 does not delete one of them.** The call sites survive too:
`ProceduralSkyActive()` has 5 call sites, `GGpuTerrainConform` 14, `CanZBias()` 4 — all
still needed to keep Dummy and the tools from crashing.

**What would actually disappear if GL33 went:** the 6 live forks above, the dead
`Viewer.cpp` include, the 8 `GL*.hpp` headers (452 lines), glad, the `PoseidonGL33` link
line in 4 app/test CMakeLists, and the `ImGui_ImplOpenGL3` dependency. Call it **~11 k
lines and one string-typed backend test**. That is a real cleanup and it is *not* the
"renderer abstractions that exist only to support two production backends" that Phase 2.3
imagines — those abstractions are load-bearing for Dummy.

## 2. What GL33 still provides

**It is reachable.** `--render` accepts `gl33` (`AppConfig.cpp:360-361`); `_renderBackend`
defaults to `"wgpu"` (`AppConfig.hpp:400`). The factory is a runtime registry, not an
`#ifdef` — `RegisterGL33GraphicsBackend()` is called unconditionally at
`GameApplication.cpp:2386` while the WGPU registration sits behind
`#if POSEIDON_ENABLE_WGPU` (2387-2389).

**It still builds.** Measured, not assumed: `cmake --preset win-x64-clang-rwdi` then
`cmake --build build/win-x64-clang-rwdi --target PoseidonGL33` completed **605/605, exit 0**,
linking `PoseidonGL33.lib`, at `aa2eea3e` on 2026-08-31.

**It is frozen.** `git log -- engine/PoseidonGL33` shows the last touch on **2026-08-23**,
and all six commits that day were upstream CWR-CE backports (`271bcf62`, `1c773e2e`,
`4f7d22c3`) or the cross-cutting `VertexIndex` widening and its revert (`5c796723`,
`b175a60a`, `802fecb6`). Nothing since. No GL33 feature work in over a week of very active
renderer development. **Phase 2.1 is de facto satisfied.**

**What it does that WGPU cannot:**

1. **Start on an adapter WGPU refuses.** `engine/WgpuRenderer/docs/runtime-capability-matrix.md`
   lists two hard refusals: missing `TEXTURE_BINDING_ARRAY` + non-uniform indexing, and
   missing `VERTEX_WRITABLE_STORAGE`. On such hardware WGPU *correctly* refuses to create,
   and GL33 is the only thing that renders. Whether any adapter the project supports is in
   that set is **not establishable from code** — see §3.
2. **Start under Xvfb on the hosted CI runner.** This is the load-bearing one.
   `tests/integration/rendering/shadow_map_scene_dump.test.toml:10-23` says in as many
   words that it "has never started on the hosted" runner, and the 6 WGPU-pinned scenarios
   are all tagged `ci-host-blocked`.
3. **Be the second witness.** The roadmap bullet itself names this. It has real, recorded
   value: `WLD-021` decided road banding with a paired GL33/wgpu capture on identical data
   (`compat-c1-capability-matrix.md`, `ROAD_BANDING_IS_BACKEND_NOT_DATA`, VALIDATED), and
   `RND-031` found six dead diagnostics on wgpu by noticing they worked under GL33. It is
   also **not universally trustworthy**: `MAT-030-treeadv-shader-family:146` records
   "GL33 is not a control for this" — asked the treeadv question it draws the birch as one
   opaque mass with no cutout at all.

**What it does NOT provide:** headless (that is Dummy, and the server links neither
backend — `apps/cwr/Server/CMakeLists.txt`); tools (Dummy); compat-layer support for
Arma 1/2/3/DayZ/Reforger (never received the work — `docs/roadmap/evidence/gl33-freeze/README.md`,
scope note); the dev panel is *not* GL33-dependent (wgpu has a native `OverlayVertex` path,
`Engine.hpp:1595-1600`).

### Every test target/scenario that exercises GL33

| Kind | What | Where |
|---|---|---|
| **Trident, CI** | **111 of 117 scenarios**, via `TRIDENT_RENDER: gl33` | `.github/workflows/build.yml:29`, consumed at `:446`; 4 shards, `-j2`, 35 min bound |
| Trident, pinned GL33 | `gl33_msaa_screenshot_readback` (tag `gl33-only`) | `tests/integration/rendering/` — also `ci-host-blocked` |
| Trident, pinned WGPU | 6 scenarios: `shadow_map_scene_dump`, `water_alive`, `water_aerial_lod`, `water_demo_island_alive`, `water_lifecycle.seq`, `water_underwater_probe` | all 6 carry `ci-host-blocked`, and `build.yml:451` passes `--skip-tag ci-host-blocked`. **All 6 are skipped in CI.** |
| Unit (linked) | `PoseidonTests` and the Tetris test target link `PoseidonGL33` | `tests/unit/engine/Poseidon/CMakeLists.txt:606,636,642`; `tests/unit/apps/Tetris/CMakeLists.txt:26` |
| Unit (source audits) | `test_gl_state_ownership_audit`, `test_gl_state_cache_audit`, `test_buffer_usage_audit`, `test_ibo_bind_audit`, `test_debug_group_audit`, `test_shutdown_order_audit`, `test_render_conventions_audit`, `test_spec_bit_backend_boundary`, `test_shader_named_outputs`, `test_zbias_math` | `tests/unit/engine/Poseidon/Graphics/` |
| Unit (behavioural) | `test_gl33_rendering.cpp`, 1,062 lines | same |
| Pester smoke | "starts with GL33 render in check mode"; "compiles all GL33 shader programs without link errors" | `tests/smoke/boot_logs.tests.ps1:94-127` — **not run by any workflow** (no `.github/workflows/*` references it) |
| Release script | `run_preview0_wgpu_check.ps1 -Backend gl33` arm | `scripts/run_preview0_wgpu_check.ps1:5,72-73` |

**Note a stale comment** at `tests/smoke/boot_logs.tests.ps1:101-103`: "the bare default is
also gl33". It has not been since 2026-08-06. Left unedited — this document's lane is
`docs/` — but it is worth a one-line fix.

## 3. The gate, bullet by bullet

| # | Gate bullet | Verdict |
|---|---|---|
| 1 | WGPU starts without silent fallback on the supported adapter/backend matrix | **PARTIAL — half proven in code, half needs a run.** The *no-silent-fallback* half is **satisfied and enforced**: `GameApplication.cpp:2467-2472` refuses to fall through on an explicit `wgpu` request and logs `"Requested WGPU renderer is unavailable; refusing automatic GL33 fallback"`; the intent is written at `AppConfig.hpp:396-399`; `run_preview0_wgpu_check.ps1:71` throws if the log contains `GL33`. The *adapter matrix* half is **not establishable from code** — no such matrix is documented anywhere in the tree, and the capability matrix describes behaviour, not coverage. **Run needed:** `run_preview0_wgpu_check.ps1` on each supported adapter/OS, reading the `wgpu capabilities:` startup line; the artefact that closes this is a table of adapters, not a screenshot. |
| 2 | Arma 1/2/3/DayZ/Reforger compatibility smoke tests pass | **NOT SATISFIED — the tests do not exist.** A search of `tests/` and `scripts/` for Takistan / Stratis / Chernarus / DayZ / Reforger returns exactly one file, `scripts/farfield-bench.ps1`, which is a benchmark. All existing compat evidence is hand-run captures recorded in `docs/roadmap/decisions/`. This bullet cannot be *failed* by a run; it has to be *built* first. |
| 3 | Menus, mission loading, screenshots, headless, dev workflows no longer depend on GL33 | **MOSTLY SATISFIED, one real dependency left.** Headless is Dummy, not GL33 (`compat-c1-capability-matrix.md`, VALIDATED, 5 binaries). Mission loading + screenshots work on WGPU: `run_preview0_wgpu_check.ps1:266` drives `--render=wgpu --test-mission … --test-type screenshot`. The dev panel has a native wgpu path. **But the developer workflow that is still GL33 is the test suite itself** — see bullet 5. |
| 4 | Release/recovery documentation no longer promises GL33 as a supported fallback | **NOT SATISFIED, and deliberately so today.** `docs/release/preview0/README.md:45-46` ("GL33 is the fallback, not a deprecated path — it is kept working"), `:132` and `:150` ("If the WGPU backend fails to start, `--render gl33` is the supported path back") are live promises to users. This is a documentation edit, cheap, but it must not be made before bullet 1 — retracting the recovery path while an unproven adapter matrix exists is the one change here that could strand a real player. |
| 5 | CI contains an explicit WGPU-start assertion and no GL33-only required job | **PARTIAL (was NOT SATISFIED) — and the first half is now known to be blocked by a feature gate, not by hardware.** See `REN-GL33-003` (2026-08-31). **Established:** every runner is GitHub-hosted, and the binding constraint is not the missing GPU — `Renderer::new` refuses any adapter without `TEXTURE_BINDING_ARRAY` + non-uniform indexing (`rust/src/lib.rs:982-989`) or `VERTEX_WRITABLE_STORAGE` (`:1015-1018`), while `force_fallback_adapter: false` (`:956`) does *not* reject a software adapter that has them. wgpu's GL backend **can never** expose `TEXTURE_BINDING_ARRAY` (it appears nowhere in `wgpu-hal-29.0.4/src/gles/`), so `build.yml`'s `WGPU_BACKEND: 'gl'` for the WGPU scenarios was incapable of working — corrected to `vulkan`, the only value a software ICD could satisfy. **Wired:** a blocking `wgpu-refusal` job asserting the no-silent-fallback contract against the built client (`--render wgpu` + `WGR_FORCE_INIT_FAIL=1` must exit non-zero, log the probe *and* the refusal, and show no `GL33: Initializing engine` / `Wgpu: creating renderer` / `defaulting to Auto`), with a `--render dummy` control and a probe-line-only-in-the-forced-arm check so it cannot pass vacuously on a machine that fails WGPU anyway; plus a `lint.yml` self-test that drives the checker against 8 stubs and requires 1 pass and 7 failures (run 2026-08-31, all 8 as specified). **Still open:** this asserts the *refusal*, not a *start* — the start half needs either a green `wgpu-start-probe` (new, `continue-on-error`, measures lavapipe under Xvfb on every push and prints its own verdict) or hardware. **And the second half is unchanged: the required integration job is still GL33-only** — `build.yml:29` still pins `TRIDENT_RENDER: gl33` and all six WGPU scenarios are still `ci-host-blocked` and skipped. `REN-GL33-003` §5 costs the four ways out, of which only the first is free. |
| 6 | Important visual reference scenes have archived before/after evidence | **PARTIAL, improved 2026-08-31.** `docs/roadmap/evidence/gl33-freeze/` now holds 6 scenes: the 2 original Everon ones (GL33 only, no WGPU counterpart) plus town/water/combat/field as GL33+WGPU pairs at 1600×900 (`319e07d6`). Four pairable scenes, all stock CWA — the compat families (Arma 1/2/3, DayZ, Reforger) have no coverage at all, which is what keeps bullet 8 partial. Two archive defects to fix: the README's table promises a `.log` beside each of the four new scenes and none was committed, and its provenance block names two different commits (`99ac0f91`, `92ebd1d8`) for the same captures. **Correction to the earlier reading of `lint.yml:79-84`:** it is not a capture-diff *gate*. It runs the harness twice over one fixed pair to self-test the acceptance policy (accept the signed-off review; reject the same review reclassified `CORRECTNESS_BUG`). No archived scene is regression-gated in CI. |
| 7 | An intentional WGPU init failure produces a clear error, not a hidden switch | **SATISFIED, 2026-08-31 — and it was not satisfied before, contrary to what this row said.** The row read "SATISFIED in code; the mechanism is unambiguous" and cited the `RptF` refusal at `GameApplication.cpp`. **`RptF` is `#define RptF(...) ((void)0)` in every build** (`DebugLog.hpp:61`), so that message compiled to nothing: the process refused correctly and said nothing about why. The row was reading source as if it were behaviour. Now a real `LOG_ERROR` naming the refusal and the alternative, and the gap the row itself identified — "nothing in the tree currently forces that failure on demand" — is closed by `WGR_FORCE_INIT_FAIL=1`. **Measured:** forced, `exit=1` with the probe line and the refusal line in the log; unforced, `exit=0` and no refusal. CI-testable now, which is what makes this row worth anything. |
| 8 | A renderer-bug triage story exists that does not require launching GL33 | **PARTIAL (was NOT SATISFIED) — the procedure now exists and is honest about the one question it cannot answer.** `docs/roadmap/renderer-bug-triage.md` (2026-08-31) routes a report into four questions and says for each what to run and when to stop. Scored against the three recorded cases: **`RND-031` yes** (an on/off ablation reading exactly `0.00000` names the dead submission path, and needs neither GL33 nor the archive); **`MAT-030:146` yes, by not asking it** — correctness questions are routed away from cross-renderer comparison, which is what made GL33 wrong there; **`WLD-021` no**, and that is the residual gap: the archive is stock CWA only, so nothing in it can exclude *data* as the cause for Arma 1/2/3, DayZ or Reforger content. The replacement for that step is ablation plus reading the asset directly — which is what actually closed `WLD-021`'s seven hypotheses — and it is slower, not equivalent. **Two corrections to this table's earlier reading, both measured:** (a) `lint.yml` does **not** gate any archived scene against drift; its step runs the harness twice over one fixed pair to self-test that the review policy accepts and that a `CORRECTNESS_BUG` classification rejects. No capture is regression-gated in CI. (b) `changed_pixel_ratio` cannot rank severity across backends — a 200×200 object blacked out scores **0.027** while the signed-off, non-blocking GL33-vs-WGPU `town` difference scores **0.55**. `--regions` was added to the harness for this reason: the blackout concentrates into 2 of 16 tiles with 0.000 in the rest, where every cross-backend pair fills all 16. |

**Score: 1 satisfied, 4 partial, 2 not satisfied, 1 partial-pending-run.**

Bullet 8 moved from NOT SATISFIED to PARTIAL on 2026-08-31; it closes fully when bullet
6 covers a non-CWA family, since that is the only case the written procedure has to
hand back undecided. Bullet 5 moved from NOT SATISFIED to PARTIAL the same day
(`REN-GL33-003`): its refusal half is asserted in CI and demonstrated to fail, its start
half is now known to be blocked by a renderer feature gate rather than by the absence of
a GPU, and its "no GL33-only required job" half is untouched.

## 4. Recommendation and risk

**Freeze and keep. Do not schedule 2.3.** This is not a close call, and it agrees with the
standing owner position on record (WGPU is the target; GL33 is fallback-only; keep it
working; never chase parity). GL33 costs ~10.7 k maintained lines, has not been touched in
feature terms since 2026-08-23, and builds clean. The gate is failed on four bullets, two
of which (compat smoke tests, CI WGPU assertion) require *building things that do not
exist* rather than merely running something.

**What breaks first if GL33 is removed today,** in order:

1. **The entire CI integration suite.** 111 of 117 scenarios lose their renderer. The 6
   WGPU scenarios that would replace them are the 6 currently skipped for not starting
   under Xvfb. Net automated rendering coverage after removal: **zero**. This is the
   whole argument.
2. **Renderer-bug triage.** Every capture-diff answer would have to come from
   `gl33-freeze/` — four pairable stock-CWA scenes as of `319e07d6`, and nothing at all
   for the compat families. `docs/roadmap/renderer-bug-triage.md` says which questions
   that can and cannot answer; the one it cannot is "data or renderer?", which is the
   only thing GL33 uniquely provided.
3. **The documented user recovery path.** `README.md:150` promises it and no adapter
   matrix exists to prove nobody needs it.
4. **`PoseidonTests` and the Tetris test target**, which link `PoseidonGL33` — mechanical,
   but it is 3,123 lines of test to delete or port.

**The smallest safe first step is not a removal at all — it is unblocking WGPU under
Xvfb.** Until `shadow_map_scene_dump` starts on the hosted runner, every other item on this
list is blocked behind it, and flipping `TRIDENT_RENDER` to `wgpu` is the single change
that converts four failing gate bullets into testable ones.

If a code-shrinking step is wanted in parallel, the safe ones in ascending risk are:

1. **Delete `#include <PoseidonGL33/TextureGL33.hpp>` at `World/Viewer.cpp:44`.** Provably
   dead (no `TextureGL33` symbol in the file; the reload goes through the
   `AbstractTextBank` virtual at :721), and it removes a shared-engine include of a target
   `Poseidon` does not link. One line, zero behaviour. *Not done here — this document's
   lane is `docs/`, and `World/` is being edited by another agent right now.*
2. **Replace the two string compares at `SceneDraw.cpp:490` and `:2615`** with a capability
   virtual on `Engine`. They are per-frame `std::string` comparisons against a CLI field,
   in scene draw; they are the wrong shape regardless of whether GL33 lives or dies, and
   fixing them makes the eventual removal a no-op at those sites.
3. **Fix `tests/smoke/boot_logs.tests.ps1:101-103`**, which still says the bare default is
   gl33.

None of these touch `engine/PoseidonGL33/` and none of them are the freeze-forbidden kind
of change — they do not alter a single GL33 pixel.

## What this does not establish

- **Whether WGPU starts on any adapter the project supports.** Read entirely from code and
  docs; not one binary was launched for this document. Bullet 1 is open on the run, not on
  the reading.
- **Whether GL33 still renders correctly.** It *compiles* (measured). Whether the built
  library still draws Everon the way `gl33-freeze/gl33-everon-day.png` says it did on
  2026-08-30 at `92ebd1d8` is unverified at `aa2eea3e` — 6 commits touched it since the
  freeze captures' commit lineage, including a `VertexIndex` width change and its revert.
- **Whether the 6 WGPU-pinned scenarios pass locally.** Their `.toml` comments claim
  `shadow_map_scene_dump` "passes locally in ~6s"; that is the test author's claim, not a
  measurement made here.
- **The true size of the maintenance burden.** Lines of code is a proxy. Nobody has been
  paying GL33 maintenance since 2026-08-23, so the honest measured burden over the last
  week is zero, which argues for keeping rather than against it.

## Open questions that need a run

| # | Question | Exact run |
|---|---|---|
| R1 | Does WGPU start on each supported adapter? | `.\scripts\run_preview0_wgpu_check.ps1 -BuildDir build\win-x64-clang-rwdi -SkipBuild -LogPath …` per adapter/OS; record the `wgpu capabilities:` line and whether `TEXTURE_BINDING_ARRAY` / `VERTEX_WRITABLE_STORAGE` are present. Closes gate bullet 1. |
| R2 | Does WGPU start under Xvfb on the hosted CI image? | **Now measured on every push, and the question was sharpened first.** `REN-GL33-003` §1 shows the GL backend can never clear the renderer's `TEXTURE_BINDING_ARRAY` gate, so the old plan (`WGPU_BACKEND=gl` under Xvfb) could not have answered it. The `wgpu-start-probe` job in `build.yml` instead runs `--render wgpu --check` on Mesa's lavapipe ICD under Xvfb, records `vulkaninfo` beside it, and prints `PROBE RESULT: WGPU STARTED` / `did not start`. It is `continue-on-error`; read a run's log for the answer. Still the blocker for bullets 2, 3, 5 and 8. |
| R3 | Does the frozen GL33 still render Everon as captured? | `--render gl33 --check --test-mission dev-missions\devtest.abel --test-type screenshot`, 1600×900 windowed, diffed against `docs/roadmap/evidence/gl33-freeze/gl33-everon-day.png` with `scripts/compare_preview0_captures.py`. A drift here means the freeze already leaked. |
| R4 | Does a forced WGPU init failure exit cleanly? | **Closed.** `WGR_FORCE_INIT_FAIL=1` exists (`GraphicsBackendWgpu.cpp:29`) and was measured the same day: forced, `exit=1` with the probe and refusal lines; unforced, `exit=0`. It is now asserted on every push by `build.yml`'s `wgpu-refusal` job via `scripts/check_wgpu_refusal.py`, with a control arm and a mechanism arm so it cannot pass vacuously on a GPU-free runner (`REN-GL33-003` §3). |
| R5 | Do the 5 compat families render on WGPU today? | No test exists. The run is: pick one representative world per family (Arma 1/2/3, DayZ, Reforger), capture under `--render wgpu`, and *write the scenarios*. Bullet 2 is a build task, not a run task. |
