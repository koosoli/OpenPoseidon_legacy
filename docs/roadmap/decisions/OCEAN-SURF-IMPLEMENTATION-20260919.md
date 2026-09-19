# Ocean and shore surf implementation — 19 September 2026

Continuation of [the Stage A evidence](OCEAN-SURF-20260919.md), after the owner requested autonomous completion. The implementation retains the FFT/CDLOD ocean and adds an opt-in, bounded spilling approximation. It does not implement an overturning liquid surface or change authoritative water physics.

## Delivered implementation

- **Consistency:** the earlier global scale correction remains. A second producer defect was found during calibration: `fft_compose` computed compression/Jacobian and curvature before applying the displacement/choppiness multiplier. These now describe the output displacement. A zero-displacement GPU regression verifies height=0 and J=1; artist normal strength remains independent.
- **Reference seas:** three entries in the existing Water tab, using the existing cascade resources. Shared `OceanReferencePresets.def` coefficients drive production and GPU calibration. Choosing one resets artistic scale/speed/amplitude to 1, disables amplitude-to-wind coupling and the old independent shore train. Sheltered, incoming-swell and rough reference states have measured significant heights (4× spatial standard deviation) of **0.1784 / 0.8261 / 1.9896 m**, respectively. Measurement: actual 512 FFT/compose, seed1471/8623, t40s, 512m square sampled at2m, local wind8m/s; these are reference measurements, not a guaranteed height at every shoreline or LOD. The exposed/rough long cascade retains its authored incoming direction and energy independently of local wind. Domain lengths are 257/57/16m; they are not wave-height or wavelength labels.
- **Shared surf:** `shore_surf.wgsl` evaluates one local arrival state for displaced water, normals, foam injection, surface whitewater, wash and existing spray origins. It uses the current terrain heightmap, its origin/spacing/enabled state and the existing generation-controlled bindings. No derived coastal texture, asynchronous producer, new island-sized allocation or second simulation is introduced.
- **Exposure:** only a shallow, gently sloping, incoming-facing coast contributes. Four up-wave terrain samples conservatively reject blocking land; cliffs, flat shelves and off-map points fall back. Lakes/rivers are excluded by the existing body selection/table. This is coarse exposure, not diffraction around a headland or an arbitrary-direction refraction solver.
- **Overlap:** local geometry replaces a weighted part of the FFT displacement; it does not add a full new train on top. The authored pilot is a westward incoming wave, k=0.18rad/m and omega=sqrt(gk), approximately35m spacing and4.73s period. Local displacement is bounded by0.354m, wash by0.12m and wet-film coverage by0.02m. These are rendering limits, not new gameplay water levels. The local phase is stable in terrain-relative coordinates, not absolute position multiplied by a varying shoreline vector. It is an analytic nearshore approximation blended with the existing spectrum, not extraction of a particular FFT spectral crest.
- **Foam/wetness:** the existing history channels carry breaker/wake/aeration. Coverage uses a continuous source/decay balance; all diffusion weights and boundary absorption depend on dt, and pause preserves the full history. The full producer catch-up budget of0.05s is accepted. The pilot reuses alpha for stationary beach wetness, with decay; foam advection follows drift and wash rather than indefinite crest-speed translation. The same main water surface owns the thin wet film—no stacked translucent terrain overlay. Outside the bounded history, instantaneous shared crest shading remains available; full material history remains camera-local.
- **Cost/diagnostics:** no higher FFT resolution or denser mesh. Existing terrain bindings are reused by foam/spray. Opt-in `WGR_OCEAN_PROFILE=1` reports CPU water preparation/submission mean/p95/max and spectrum configuration changes. The triangle counter now includes the768 skirt triangles per node rather than omitting them.

## Tests and evidence

`build/ocean-release-water-tests.log`: **61 Rust water tests passed**, including actual GPU scale, calibrated FFT, shared-surf and full foam-pipeline tests. `build/ocean-release-shader-tests.log`: all entry-shader composition passed. `build/ocean-physics-tests.log`: **11 CPU water tests / 4109 assertions passed**, covering deterministic surface queries, raised levels, body containment/flow, buoyancy/contact and related contracts. Tests were rebuilt in this worktree. GPU tests report explicit skips if their adapter requirements are unavailable; the local RTX3070/Vulkan runs executed them.

The production-function/GPU matrix covers:

- Scale0.5/1/2 displacement/normal/source consistency; zero displacement/Jacobian.
- Equal elapsed source/decay at20/30/60/120Hz; full RGBA16F history at actual30/60/120Hz producer cadence, centre and edge pause, and teleport/reset.
- A translated synthetic terrain, straight and curved exposed slopes, an up-wave blocking headland, a cliff, flat shelf, off-map and disabled evaluation.
- Rendering toggle separation: no new physics/network inputs, GPU readbacks or GPU dependency in authoritative queries. The CPU water suite verifies those unchanged contracts; this is not a claim of a newly executed multiplayer session.

Runtime evidence uses the game lock, installed game data, matched staged EXE/DLL pairs and private profiles. Each run records binary hashes, arguments, effective settings, harness calls, source patch and engine log. Earlier exploratory runs are not clean-commit claims.

- `surf-pilot-locate-20260919-210140`: INVALID rendering evidence. The new fragment terrain binding lacked fragment visibility; fixed before accepted captures.
- `surf-pilot-view-20260919-210546`: initial pilot view, overly rough provisional preset; superseded by metre calibration.
- `surf-off-calibrated-20260919-211512`: calibrated reference, surf disabled, 24s live clip.
- `surf-on-calibrated-20260919-211704`: first calibrated shared pilot clip.
- `surf-final-on-20260919-212334`: consolidated pilot/spray/wetness, 24s live clip plus moving camera, sudden turn/FOV, teleport, underwater and return captures; clean runtime exit with no validation/device errors.

`build/ocean-surf-comparison/ocean-live-comparison.mp4` pairs the live baseline and pilot sequences. These are live histories with matching camera/settings, **not phase-locked pixel comparisons**; the legacy global sea-level clock is unchanged. The clips cover several arrivals. Sampled frames show the bounded coastal wash and intact coastline; underwater and return frames were inspected. Optional overhanging crest ribbons are intentionally omitted: they are not needed for this bounded spilling version.

## Measured cost

RTX3070, Vulkan595.79,1280×720 native,MSAA4,FFT512,geometry1,reference incoming-swell preset4, private exposure0.5 profile. Undisturbed12s intervals precede screenshot capture; capture stalls are excluded.

| GPU region, median ms | Surf disabled | Consolidated pilot |
|---|---:|---:|
| Evolve |0.115|0.112|
| FFT horizontal / vertical |0.206 /0.199|0.207 /0.200|
| Compose |0.112|0.112|
| Water draw |2.149|2.217|
| Foam |0.177|0.233|
| Interaction |0.010|0.011|
| Reflection |1.416|1.417|
| Total water regions |4.391|4.696|

Six timing samples per arm; sequential short-run evidence, not a cross-hardware promise or established speedup. Approximately+0.305ms total meets the provisional+0.35ms pilot target in this view. An earlier provisional run had reflection variation and4.899ms total; it is retained rather than hidden. Consolidated CPU preparation/submission averaged0.093–0.098ms per reporting window, p95 0.118–0.167ms, max0.229ms. No slow-frame log entry occurred during that measured interval. This is CPU submission wall time, not GPU execution time or whole-game CPU time. Complete frame-time percentiles are not inferred from thresholded slow-frame logs.

The coastal view selected222 nodes:4,091,904 surface triangles plus170,496 skirt triangles = **4,262,400 submitted triangles**, before clipping. No pilot geometry is added. Added texture/buffer simulation memory is zero; only small bind-group/pipeline metadata is added. The existing foam allocation remains two1024²RGBA16F textures (16MiB total). No new coastal resource rebuild stream exists; existing terrain view generation controls rebinds.

## Activation and limits

The consistency/history fixes apply normally. The three reference seas are choices in the existing Water tab and do not replace saved defaults. Surf is explicitly opt-in at process start with `WGR_WATER_SURF_PILOT=1`; use the incoming-swell reference for its authored westward approach. The isolated launcher handles that setting:

```powershell
.\scripts\Test-OceanCoherence.ps1 -Review -Surf -Preset 4
```

Close it and omit `-Surf` to compare the reference without the pilot. Review has no timeout. `-Quick`, `-LiveClip`, `-Matrix`, `-LowQuality`, `-NoFft` and `-WindSpeed` provide bounded diagnostic cases. The main profile remains separate.

Preserved limitations: this is a single-valued, render-only spilling approximation, not a plunging breaker or wave-resolved boat solver. Normals in spatial transition/warped regions remain approximations. Exposure is conservative/coarse and the pilot uses its authored incoming heading; the existing FFT preset/weather rebuild latching remains, without adding a second spectrum crossfade allocation. Full history is bounded around the camera. No claim of pixel-equivalent CPU/FFT water, exhaustive every-map art acceptance, or a newly run multiplayer session is made.

Final integration/installed-build verification is recorded in the status ledger and the delivery addendum below.

## Water-only integration

`codex/ocean-release` starts at the actually installed `5457e729`, importing only the water changes from `ed6a2989`/`643abc64` and their diagnostics. It excludes the intervening unaccepted squad-AI implementation commits. The original `codex/ocean-surf` work is committed and backed up. Parity, the root checkout and their uncommitted files were not edited. Final committed-build and installed-runtime verification follows this integration.

## Installed delivery

Production commit **e99d7262** was built from the committed water-only tree (`build/ocean-shipping-build.log`), pushed and deployed through `Deploy.ps1` under the game lock. Installed stamp: `codex/ocean-release e99d7262 2026-09-19_21:39 deploy-ps1`. The actual installed EXE/DLL then passed the full-FFT,1m/s-wind, surf-enabled quick/moving/turn/teleport/underwater/return scenario: `installed-e99d7262-20260919-213929`, exit0 and no renderer validation/device errors. The game was closed afterward. Binary and evidence hashes are in [the delivery manifest](../evidence/ocean-surf-20260919.json).

Additional runtime arms: `surf-final-off-20260919-213154` supplies the CPU-instrumented final disabled baseline in the table; its mean CPU windows were0.094–0.095ms versus0.093–0.098ms for the pilot. Both undisturbed intervals logged zero spectrum configuration changes and zero slow-frame entries. `surf-lowwind-fallback-20260919-213230` passed with1m/s wind, low quality and FFT disabled, including the view-transition matrix. Its fallback surface was inspected; it is not the calibrated FFT height measurement.

The old installed matched pair is preserved at `build/ocean-installed-backup-5457e729`. Default-content PBO payloads/attributes and mod manifests were compared with the installation before deployment: identical contents, with only archive metadata/hash differences from repacking. No unrelated squad implementation was installed. Parity remained e486e719 and its foreign Cargo.lock hash remained unchanged.

Implementation, integration and focused runtime verification are complete for this bounded version. The ledger remains **INTEGRATED**, rather than falsely recording owner visual acceptance: `validate_execution_ledger.py` requires an actual named owner review for a visual ticket's final VALIDATED/SHIPPABLE state. Existing conservative approximations and the unclaimed broader checks are listed above; no owner review or multiplayer session has been invented.

Final ledger check: the repository-wide compat-C1 validator stops at the pre-existing, unrelated AST-011B evidence-hash mismatch. Its entry was left untouched. The WTR-SURF-001 evidence hash was independently recomputed and checked against its four evidence files.

## Owner rejection and normal-start correction

Oliver rejected the normal-start shoreline on 2026-09-19: dense striped highlights and overlapping sheets still looked like slime. The earlier implementation-complete statement was premature as a visual claim. The pilot-only fixture bypassed the still-active legacy shore train; the saved intro profile used wave amplitude 4 and shore gain 1.

The follow-up makes shared surf default-on (explicit `WGR_WATER_SURF_PILOT=0` remains the diagnostic opt-out), including existing profiles. The legacy comparison path now uses fixed propagation direction/wavenumbers and terrain-relative coordinates: local direction or depth changes can no longer multiply a kilometre-scale world coordinate into a phase discontinuity. This does not add a refraction solver; the shared surf retains its conservative authored incoming heading and exposure bounds.

A new production-function GPU regression checks phase invariance under changing bathymetry/direction, translated terrain origin, and sub-metre spatial continuity. The current release branch passed 56 water tests and entry-shader composition; previous 61-test counts came from the earlier development branch. A new `-Menu` capture copies the owner's water settings into an isolated profile and launches the actual menu without a test mission or a wind override. `-LegacyShore` explicitly selects the comparison path; the ordinary test invocation no longer forces surf off.

Runtime verification and installed follow-up commit: pending while the owner's review game is open. No visual acceptance is claimed by this source/test checkpoint.

## Default rollback requested by Oliver

Oliver reported flat water and explicitly requires the old behavior to remain default until the new behavior is demonstrated and accepted. This supersedes the default-on proposal above. Both production pipeline defaults and the launcher now select the original path; its shader function is restored byte-for-byte from e99d7262. The installed e99d7262 was never replaced by the default-on proposal.

The opt-in experiment had blended the entire ocean displacement/normal into a fixed 0.30m coastal field. At full coastal weight this erased the underlying swell and horizontal displacement; the envelope was also applied twice. The revised opt-in path preserves the ocean displacement and adds the bounded local height once, adds its normal slope, and preserves the ocean contribution to spray position, velocity and breaking. A production WGSL GPU check confirms a full-strength local contribution preserves a 3m input wave and both horizontal components. 55 water tests and shader composition pass. This is a correction of destructive blending, not proof that the surf art is acceptable. The fixed-heading and approximate-normal limitations remain; old behavior stays default pending owner acceptance.

Runtime comparison and installation evidence will be appended after the focused checks. Live comparison generation now resamples recorded elapsed timestamps, rather than playing slow screenshot readbacks faster than real time.
