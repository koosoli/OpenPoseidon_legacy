# WGPU ocean scale coherence - 19 September 2026

## Baseline and ownership

Work branch `codex/ocean-surf`, continuing in this task's clean, exclusively owned `.claude/worktrees/combat-audit` worktree from `2bf64ee4f19f8d9093dc3772443e0ad507da6446` (`codex/squad-ai`). Reusing its build cache did not switch or edit another checkout. That baseline contains the prior opt-in AI experiments, independently off. Parity remains e486e719. Root consolidated-v2 at 5ca4be45 has foreign deletions, Cargo.lock edits and untracked documents; none were incorporated. The WGPU water files have no difference between parity and this task baseline.

The owner-supplied revision-1 plan was copied unchanged: SHA256 7BA13F79BA9E3D84177FC1DF70370E0DD73A2EE1DF05DE724C70B4A141BF8FE1. The installed matched pair remains `codex/combat-fixes 5457e729 2026-09-19_16:59 deploy-ps1`. Measurements below use explicitly identified staged matched EXE/DLL pairs against installed data, separate profiles, and the game lock. No GL33 work or change to authoritative water queries, physics, multiplayer, resources or C ABI.

## Stage A: reproduced and corrected

The producer `fft_compose.wgsl` writes displacement after per-cascade amplitude/choppiness controls, a separately art-directed slope, dimensionless Jacobian/compression, curvature and slope variance. The global scale is applied later by the consumer. The supported artistic contract is D_S(x,t)=S*D(x/S,t): unchanged period, wavelength and phase speed multiplied by S, slopes/Jacobian invariant under S, curvature divided by S. This is not a claim of a different physically consistent sea state.

Before this change displacement and normal lookups used x/S, but normal slopes were additionally divided by S. Surface auxiliary fields, foam injection, spray placement and caustics sampled x. At the owner's saved Everon scale 0.65 these mappings disagree; at S=1 they coincide. This proves an active consistency defect, not the full cause of the September 14 community footage.

Changes:
- Shared `fft_scaled_uv` mapping for displacement, dynamics, surface auxiliary/debug fields, foam, spray and caustics. The already-scaled underwater surface-height path remains intact.
- Remove the extra normal division; retain independent per-cascade artistic normal strengths.
- Scale displacement/approximate orbital velocity at spray origins, curvature by 1/S, and projected filtering/texel footprints by S. Keep the intentionally normal-only short cascade classification in source-domain units.
- Reuse existing developer freeze/settings through a bounded `water_probe` harness command, plus an isolated Nogova camera fixture and capture/review launcher. No second shader clock, foam history or wave solver.

GPU regression `water::scale_tests::gpu_ocean_scale_normals_and_foam_follow_displacement` executes extracted production sampling functions against one analytic texture wave (unity artistic strength, no horizontal chop or warp), compares shader normals against finite differences of the actual displacement sampler, and compares production foam injection with the displaced crest. It does not merely repeat the implementation in CPU arithmetic. On Vulkan / RTX 3070, driver 595.79:

| Scale | Before max slope error | After | Before crest/source error | After |
|---|---:|---:|---:|---:|
| 0.5 | 0.197415 | 0.001367 | 5.249988 | 0.000333 |
| 1 | 0.001367 | 0.001367 | 0.000333 | 0.000333 |
| 2 | 0.098287 | 0.001367 | 5.249100 | 0.000333 |

The residual is reconstruction/finite-difference error (thresholds 0.002 slope, 0.005 source). GPU-less machines explicitly skip this GPU test; a skip is not runtime acceptance. The local run executed it. Full water-filtered Rust tests: 57 passed; all entry-shader composition: 1 passed. PoseidonGame built and WGPU launched without validation/device errors.

## Captures and costs

Artifacts are under `build/ocean-runs/`:
- `baseline-locate-20260919-194627` is INVALID: fixture omitted the scalar argument to triPerfReset; corrected before comparisons.
- `baseline-locate2-20260919-194922` locates the camera, at exposure 0.15; not the matched visual comparison.
- `baseline-20260919-195043`: unchanged water shader with the water_probe diagnostic hook.
- `corrected-20260919-195542`: corrected shaders, same hook/settings.
- `committed-20260919-200243`: final clean production commit `ed6a2989`, rebuilt through Build.ps1, isolated WGPU quick capture passed at scales 0.5/1/2; normal exit 0, no validation/device errors. Build and runtime logs: `build/ocean-committed-build.log`, `build/ocean-committed-runtime.log`.

The baseline matched pair and its tracked-source patch are preserved in `build/ocean-diagnostic-baseline`; the original task pair is separately in `build/ocean-baseline`. Each run records binary hashes, arguments, environment, fixture hash, effective settings, harness calls, source patch, screenshots and engine log. Exploratory binaries are not described as clean committed builds. Installed data hashes (Noe.pbo, CONFIG.BIN and remaster.cpp) were additionally recorded after capture in `build/ocean-comparison/data-hashes.json`. Installed executable/DLL hashes and stamp, parity HEAD and its foreign Cargo.lock hash were rechecked unchanged. Production commit `ed6a2989` is backed up on `safety/codex/ocean-surf`.

Controlled profile: 1280x720 native, DLSS off, MSAA4, preset3, sea-state coupling on, amplitude1, wave scale0.65, choppiness1, speed1, warp5, shore gain0.1, cascade preset1, FFT512, geometry quality1, wind8m/s at90deg, exposure0.5. Normal and elevated cameras use the same script/FOV. Three stills isolate FFT at scale0.5/1/2 with shore gain0. Twelve-second clips sample explicit FFT times40..51.875 at8Hz, with foam history frozen; they assess surface/normal/source motion, not material transport or frame pacing. Live history is separately captured before freezing. The legacy global sea-level bobbing and actor animation are not controlled by the renderer freeze: these clips are NOT pixel-equivalence evidence or a matched physics test. The camera is in a bay, not an accepted exposed-surf pilot. A visible soldier supplies approximate scale, not a calibrated buoyancy test.

`build/ocean-comparison/ocean-shore-comparison.mp4` and `ocean-elevated-comparison.mp4` pair corresponding FFT times. Encode with `scripts/make_ocean_comparison.py`; 96 frames per clip verified. Still inspection found no new holes or rectangular history edges in these views. The surface remains a height field; no convincing breaking/wash cycle is claimed.

Six samples/arm from a separate 12-second live interval before screenshots, median milliseconds:

| Region | Before | After |
|---|---:|---:|
| Spectrum evolve | 0.111 | 0.112 |
| FFT horizontal / vertical | 0.209 / 0.205 | 0.206 / 0.201 |
| Compose | 0.115 | 0.116 |
| Main water draw | 5.196 | 4.650 |
| Foam / interaction | 0.174 / 0.010 | 0.175 / 0.010 |
| Reflection | 1.241 | 1.295 |
| Total water regions | 7.245 | 7.211 |

Short sequential samples support no obvious cost regression, not a statistically established speedup. Both logged230 selected nodes and4,239,360 surface triangles. Source audit shows the indexed mesh also submits768 skirt triangles/node, so the full230-instance draw submits4,416,000 triangles before clipping; the existing C++ counter omits skirts. No FFT resolution, CDLOD density, reflection cadence or resource allocation was increased. No new surf pass exists. Separate CPU-water-preparation timing, rebuild-event counts, memory telemetry and long-frame distributions were not measured; aggregate triPerfStats is not a CPU-water metric or reliable presentation-FPS claim.

## Remaining gates and next stage

This is the first consistency slice, not completion of Stages B-D. Do not retune defaults or stack surf effects simply to make screenshots whiter. Next requires visual judgement of the corrected motion, metre-calibrated sheltered/exposed/rough reference presets, independent low-wind swell, and a true exposed-beach pilot. Before that pilot, use the measured ~7.2ms water cost here as the hardware baseline: provisional added GPU-water budget <=0.35ms at this resolution/coverage, with a separately measured CPU-preparation budget before adding coastal work. This is a target to validate, not a cross-hardware promise.

Confirmed source limitations still needing their own work:
- Existing shore phase uses dot(p,d(p))*k(p); translated/curved synthetic beaches and its derivative terms have not been validated. No shared crest/break/foam/wash event exists yet.
- Normals under nonzero domain warp, spatial shoaling weights, horizontal chop and seabed clamps remain approximations. The GPU regression intentionally does not claim to validate all those terms.
- Foam dispatch is once per rendered update. C++ accumulates up to3 fixed1/60 steps into one dt, while foam clamps dt to0.033. Breaker-channel nonlinear history contrast was already fixed at baseline and was not reimplemented; wake/aeration diffusion/contrast still needs elapsed-time and pause controls.
- Full sheltered/lake/river/underwater/wake, moving-camera/teleport/reload, headless/network and 30/60/120FPS acceptance matrix remains open. No visual-surf physics matching claim.

The root/AI plan forbids shipping unaccepted squad experiments. This water branch inherits them from the verified current task baseline, so no whole-branch normal-install deploy is performed. A water-only integration can cherry-pick the water commit after visual acceptance, without the preceding squad commits. Until then the launcher makes the exact staged result reviewable without replacing the installation.

## Review / rollback

From this worktree, close any game first:

```powershell
.\scripts\Test-OceanCoherence.ps1 -Review
# Close the game, then compare the preserved water baseline:
.\scripts\Test-OceanCoherence.ps1 -Review -BinaryDir "$PWD\build\ocean-diagnostic-baseline"
```

Review has no automatic timeout; the Water tab remains available. It uses its own controlled profile, not the owner's saved water-look files. To roll back the experiment, close it and launch the unchanged installed OpenPoseidon.exe. No save or shared profile migration is required.
