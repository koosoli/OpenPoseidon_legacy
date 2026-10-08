# ReShade depth and screen-space tracing audit, 2026-10-08

**The existing integration does not contain RTGI or all ReShade effects.** It
loads the official optional 6.8.0 Vulkan runtime, exposes live global effects
controls and final-presentation before/after captures, and supplies one original
color-only sharpen/contrast shader (`OpenPoseidonLook.fx`). The existing installed
color-only acceptance remains useful but establishes neither depth access nor
screen-space GI. This audit inspected the current parent worktree's
`scripts/Prepare-ReShade.py`, `tools/reshade-bridge/ReShadeBridge.cpp`,
`engine/Poseidon/Dev/Debug/ReShadeOptional.cpp` and renderer depth conventions.

## What the linked guide actually describes

The [requested forum guide](https://reshade.me/forum/shader-discussion/5450-guide-to-ray-tracing-with-reshade)
describes Marty McFly's depth-dependent **screen-space** ray tracing. Its old
Patreon tiers, beta status, predicted future availability and parameter recipes
are historical information, not current download terms or engine-specific
calibration. Its useful constraints are that valid scene depth is needed,
off-screen information is missing, and fog/water/halos/temporal artifacts require
care. Do not repeat its blanket superiority claims as verified measurements.

The current [author's RTGI page](https://www.martysmods.com/rtgi/) still directs
downloads to Patreon. No user-provided licensed RTGI archive was identified or
installed. No purchase, third-party mirror, paywall bypass or redistribution is
part of this work. [qUINT](https://github.com/martymcmodding/qUINT) is a separate
collection (MXAO/ADOF/Lightroom/Bloom/SSR/sharpen/deband); its README says all rights
reserved. Public availability alone does not authorize bundling its shaders.

## Actual depth prerequisites

ReShade's [pinned add-on manager](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/source/addon_manager.cpp)
loads its built-in **Generic Depth** add-on unless disabled. Our bridge supplies
no custom engine-depth handoff. Generic Depth tracks candidate depth resources
and [binds the selected view to the DEPTH semantic](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/examples/09-depth/generic_depth_addon.cpp).
That source also updates `bufready_depth`, which the new probe uses. Therefore
the optional official runtime may already expose valid WGPU Vulkan depth, but
the existing color-effect receipts do not prove that it does.

The engine uses reversed-Z, cleared to zero, with an infinite-far projection:
main depth is `near / viewZ`. This is established in
`engine/WgpuRenderer/rust/src/gfx3d/mod.rs` and its depth reconstruction tests.
Shadow maps also exist and are unsuitable substitutes for scene depth. Scene
resource selection, resolution/offset, vertical orientation, pre-clear
preservation, MSAA and render scaling must all be checked. Reversed-Z alone is
insufficient. The [official common header](https://github.com/crosire/reshade-shaders/blob/fd0022170615ce0d8162d219bff07232fa6dd84f/Shaders/ReShade.fxh)
has orientation/reversal/scale and finite far-plane linearization controls;
its default far-plane value is not the engine camera's exact metric projection.

## Bounded addition

`scripts/Prepare-ReShadeDepth.py` stages an **original GPL depth availability
probe**, separate off/probe presets, and optionally the author's freely licensed
[DH_UBER_RT](https://github.com/AlucardDH/dh-reshade-shaders/tree/0783b8fde77df48412d79070be7de4bc3743b898).
The author describes that shader as screen-space GI/AO/reflections. Its pinned
[LICENSE](https://github.com/AlucardDH/dh-reshade-shaders/blob/0783b8fde77df48412d79070be7de4bc3743b898/LICENSE)
is GPL-2.0. It remains a separately downloaded FX source, not linked/copied into
the engine. The preparer retains its license and source without modification,
plus the official `ReShade.fxh` with CC0-1.0 SPDX. It writes URLs, exact SHA256s
and byte sizes to `DEPTH-PROVENANCE.json`. No paid Marty shader is included.

The probe samples raw DEPTH directly and expands its range for visibility. Its
top strip is red when Generic Depth reports no bound resource and green when a
resource is bound. **Green is not proof of correct scene depth.** Check aligned
geometry and changing depth in the image. The probe deliberately does not claim
metric linearization, AO, GI or ray tracing.

The optional DH preview has SSR and source-ambient removal off, moderate AO/GI,
one-frame accumulation and a one-third ray-generation scale. It is a starting
point for a **stationary** comparison, not an accepted quality preset. Its motion
texture is not supplied by another effect, so this does not establish temporal
motion support. The full shader's GPU cost can be substantial even with SSR off.
Its DX9-only external noise image is not needed by this Vulkan experiment.

Preparation never changes `ReShade.ini`, the selected preset, `OpenPoseidon.ini`,
the engine preference or any global Vulkan registration. Existing differing
files cause a refusal, preserving user tuning. Only explicit preset selection
enables a new technique. `--probe-only` uses no downloads and does not stage the
large DH effect, keeping the first depth test small. A normal game launch remains
off unless the user has independently opted into the existing ReShade feature.

## Exact parent-owned validation/capture plan

The parent session owns game launches/deployment and the shared game lock. No
game was launched and no shared runtime was changed for this addition.

1. Read `DEPLOYED-FROM.txt`, verify the paired binaries, and hold the game lock.
   Use an **isolated copy** of the already verified optional runtime for this
   experiment. Preserve the owner's active cache, preset and preferences.
2. Stage only the probe first:
   `python scripts/Prepare-ReShadeDepth.py <isolated-runtime> --probe-only`.
   In that isolated runtime's copied `ReShade.ini`, explicitly select
   `PresetPath=.\Presets\OpenPoseidon-Depth-Probe.ini`. Use the existing launcher
   with `-RuntimeDirectory <isolated-runtime>` so layer scope stays process local.
3. Reuse the exact stock Everon scene and pose from
   `scripts/Test-OptionalReShade.ps1`: 1600x900, view distance 3000, freefly
   `8131.43 4086.37 191.04 229.1 -11.3`, 1985-06-21 16:00, fog/overcast .25,
   rain zero. Keep DLSS off and exposure fixed at .5. Wait for compilation and
   at least six seconds after the startup overlay finishes.
4. Check `ReShade.log` for successful probe compilation and no runtime errors.
   Capture with the existing final-presentation `optional_reshade/capture`
   command, polling `captureCount`/`captureStage` and checking the actual files.
   This gives a labeled native-versus-depth-diagnostic pair, not a beauty pair.
   The depth image should show aligned building/terrain silhouettes and farther
   surfaces brighter than nearby ones; sky should remain at the far end.
5. If red/uniform/wrong: inspect Generic Depth in ReShade's Add-ons tab. Confirm
   it is enabled, inspect candidate resource dimensions/draw counts, and try
   only the matching scene resource plus depth-copy-before-clears where needed.
   Record exact choices and a second probe capture. If no correct resource is
   available, stop the bounded test and report that blocker; do not present
   color grading or a black effect as working tracing. A dedicated engine-depth
   handoff is a separate renderer task.
6. Only after correct depth is established, run the preparer without
   `--probe-only` against the same isolated runtime. It downloads exactly three
   pinned source/license files, total **132580 bytes**. Select
   `OpenPoseidon-DH-Depth-Debug.ini` to check DH's own depth reconstruction;
   calibrate depth settings to the actual projection before selecting
   `OpenPoseidon-DH-RTGI-Preview.ini`. Default reversal is 1 and upside-down is
   0; these are test starting points, not a visual acceptance claim.
7. For the new GI/AO comparison, use the same locked stationary camera, settings
   and native effect state in both images. For an isolated external AO/GI trial,
   set `WGR_GTAO=0` and `WGR_GI=0` **in both arms**, then disclose that choice.
   Capture native versus DH using ReShade's final-frame capture. Recheck the
   selected technique and log, because the bridge's count includes all compiled
   techniques and does not identify which one produced the pixels.
8. Inspect roofs/wall-ground contacts, thin foliage, sky, weapon/HUD, fog and
   water for halos/leaks. Then make a small camera movement and return to the
   recorded pose to test disocclusion/flicker. Report motion as unsupported if
   the missing motion provider prevents a clean result. Measure total frame time
   externally: engine GPU timestamps exclude injected work. A passing still
   does not establish resize, upscaling, multiplayer or all-effect compatibility.
9. Restore the original selected preset/preferences; leave ordinary runtime
   loading off unless the user chose otherwise. Keep screenshots, logs, preset,
   provenance, GPU/driver and stamped build together. Label **depth diagnostic**,
   **DH screen-space GI/AO**, and **existing color look** separately.

## Verification completed here

Python syntax compilation and three offline tests pass: probe-only preparation
makes no network calls and preserves active configuration; corrupt source hashes
write no shader; conflicting user shader files are preserved before other writes.
Full source preparation was run in an isolated build directory, verified all
three pinned hashes, and subsequently repeated from local cached sources. The
final staged assets are in `build/reshade-depth-final` in the feature agent's
worktree. **FX compilation, actual depth availability, new before/after images
and total frame cost remain unverified until the parent runs the capture plan.**

## Installed bounded capture helper

The parent/operator can run `scripts/Test-ReShadeDepthInstalled.ps1` under the
shared game lock. Stage the official core runtime plus the prepared depth files
into one isolated input directory first; the test itself downloads nothing.
For example, with the installed stamp's full commit:

```powershell
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <installed-commit> -RuntimeDirectory C:/Temp/op-depth-staged -Mode Probe
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <installed-commit> -RuntimeDirectory C:/Temp/op-depth-staged -Mode DHDebug
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <installed-commit> -RuntimeDirectory C:/Temp/op-depth-staged -Mode DH
```

Each invocation launches exactly one injected game and uses the existing bridge
to capture its native/processed final-frame pair. It copies only required staged
files into a fresh short temporary runtime, writes a fresh isolated config/profile,
verifies pinned official binary and DH licence/source hashes, fixes the stock
Everon camera/weather/date, and sets `setAccTime 0`. Default MSAA is one sample
to avoid confusing a first depth test with depth resolve selection; explicit
`-MsaaSamples 4` is a separate optional follow-up. Engine GTAO/GI are disabled
for both images. All changed environment variables are restored, and only its
owned process can be terminated. Caller runtime files and normal user profiles
are never written. Compile readiness has a 90 second deadline. Scratch files
are retained for diagnosis; the receipt records their absolute path.

The result logs the exact preset/technique, shader compilation evidence, active
technique count, hashes, captures, exit code and engine performance counters.
`captureSucceeded` reports the automation/capture result; `visualAccepted` stays
false, and depth availability awaits inspection. Probe green means a selected
resource, not correct scene depth. Engine frame performance explicitly excludes
injected ReShade GPU cost, so these counters cannot establish total overhead.
No runtime execution was performed by the feature agent.

The parent's first installed probe (2026-10-08 09:12, installed `c35727c7`)
loaded Everon but never loaded the optional runtime: the scratch runtime had
no `ReShade.log`. This was an injection failure, not evidence against depth
support. Read-only follow-up found the fresh profile lacked the startup opt-in,
and a unique-variable local check demonstrated that this host's .NET 10.0.11
null environment setter creates a present empty entry. The harness now writes
`optionalReShade=1` into its new profile, clears inherited selectors with
`Remove-Item Env:...`, restores originally absent keys by deletion, and records
`VK_LOADER_DEBUG=error,warn,layer` output in stderr and the result receipt. After
renderer/harness startup it allows only ten seconds for an isolated runtime log
before reporting injection failure. Target shader compilation keeps its separate
90 second bound. The feature agent has not executed the revised game test.

## Selected resource contains cleared depth: bounded follow-up

Historical test rationale below; the completed three-mode result and current
blocker are recorded in the following section. Do not repeat this mode sweep.

The parent's 09:30 probe on installed `c35727c7` successfully compiled and
captured the requested effect. With scene overlay zero, its processed image is
white below the green availability strip. That is consistent with sampled raw
depth zero throughout, and does not establish usable scene depth. The captured
config had no `[DEPTH]` settings; the engine log identifies the main format as
`Depth32FloatStencil8`.

The pinned [official GenericDepth source](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/examples/09-depth/generic_depth_addon.cpp)
initializes preservation to zero. Its device setup reads
`[DEPTH] DepthCopyBeforeClears`; value 1 copies before depth clears, including a
render pass whose attachment load operation clears. Value 2 also copies when
depth is unbound on Vulkan/D3D12 and prevents the end-of-frame fallback copy.
`DepthCopyAtClearIndex=0` keeps the automatic workload-based clear choice.
The source's aspect enum value 3 filters exact output resolution, and format
filter value 6 selects D32S8. GenericDepth itself adds Vulkan copy-source usage
when resources are created, so the engine's missing explicit COPY_SRC flag is
not sufficient evidence that copies are impossible.

Renderer source inspection provides a concrete reason to try preservation:
`rust/src/gfx3d/mod.rs::ensure_depth` creates the scene depth; at one sample and
render scale 1 the UI reuses that same resource. In
`rust/src/lib.rs`, the first segment's prepass clears depth to zero and stores
it; the world color pass loads/stores it. Later nonprepassed segments clear
depth again, including the post-Resolve UI segment even when it contains no
world geometry. Thus the final resource can be empty while the completed
world had valid depth earlier. This is a source-based explanation, not a
captured resource identity proof. Scene passes inspected use Store rather
than Discard. Shadow buffers are separate and normally fail exact-resolution
filtering.

The installed harness now writes only its new isolated config with
`DepthCopyBeforeClears=1`, automatic clear choice, exact resolution, and D32S8
filter. The receipt records every value and primary source. It requires the
engine's actual float-stencil format log. The parent can rerun the raw probe
once with that setting. If preservation still fails, one explicit optional
`-DepthCopyBeforeClears 2` probe tests Vulkan unbind preservation; no automated
parameter sweep is provided. `-DepthCopyBeforeClears 0` reproduces the earlier
end-of-frame configuration. No paid shader, engine edit, build, deployment,
runtime launch or depth handoff was performed here. DH GI remains gated on
correct scene-aligned depth; a green availability strip alone never qualifies.

## Installed conclusion: usable scene depth is blocked

The parent completed the bounded preservation tests on the same installed
`codex/whole-world-horizon-20261008 c35727c7` build, fixed stock Everon camera,
1600x900 output, one sample and zero scene overlay. All three produced a green
selected-resource strip and an otherwise entirely white depth diagnostic:

| Preservation | Parent receipt directory under `build/reshade-depth` | Processed capture time | Result |
|---|---|---|---|
| 0, official default/end-of-frame | `20261008-093000-Probe` | 09:30:17 | Empty sampled depth |
| 1, copy before clears | `20261008-093710-Probe` | 09:37:29 | Empty sampled depth |
| 2, Vulkan during-frame/unbind preservation | `20261008-093912-Probe` | 09:39:30 | Empty sampled depth |

The feature agent inspected all three processed images and read the receipts
and official runtime logs. Captures succeeded; `visualAccepted` remained false.
The last receipt records exactly one active requested probe technique, paired
native/processed capture completion, the expected D32S8/exact-resolution
filters, and exit code zero. The runtime compiled the actual diagnostic shader.
These facts establish functioning injection, effect execution and capture, but
not usable geometry depth. The green strip establishes only a non-null selected
view. With reversed-Z and overlay zero, uniform white is consistent with raw
zero everywhere. It cannot distinguish wrong-resource selection, ineffective
backup copies, and depth invalidation without resource-level evidence.

The renderer's later clears remain a real source finding, but the failed
preservation tests show that enabling GenericDepth's supported settings does
not resolve this build's handoff. **Usable scene depth is a reproducible blocker
for this slice.** No further configuration sweep, DH debug/beauty launch or
visual GI/RT claim is warranted. No DH before/after images or injected GPU-cost
measurements have been produced. The optional depth-dependent effects remain
disabled and unaccepted; the explicit runtime opt-in applied only to the
isolated test profiles and exited processes. This documentation update changes
no running process, normal profile or active preset.

### Next bounded renderer handoff, separate implementation slice

1. Add a developer opt-in snapshot of the completed **world** depth before the
   first ClearDepth/Resolve boundary in `rust/src/lib.rs`. Start with the already
   tested Vulkan, one-sample, render-scale-1 case. Preserve raw reversed-Z into
   a dedicated single-sample resource rather than borrowing the mutable depth
   later reused by UI. Keep ordinary launches unchanged. Publish dimensions,
   frame/generation, actual near plane and matching camera projection with it.
2. Prove that snapshot first with bounded numeric readback: nonzero count and
   min/max plus a small grid at known visible terrain/building/sky locations.
   A non-null handle or successful copy alone is insufficient. Do not add a
   full-frame CPU readback to every presentation frame.
3. Extend the existing versioned optional bridge to consume that snapshot on
   the same Vulkan device, with explicit ownership, layout/queue synchronization
   and resize/teardown invalidation. The pinned official API documents resource
   handles as Vulkan `VkImage` and exposes `update_texture_bindings`; check the
   exact native-device/resource contract before wiring the Rust/C++ boundary.
   Use an engine-specific semantic for the first diagnostic so GenericDepth
   cannot silently overwrite the binding. Keep the engine independent of the
   optional DLL and never hold a borrowed texture beyond its generation.
4. Repeat only the fixed raw diagnostic native/processed pair. Acceptance needs
   visible scene-aligned depth with meaningful foreground/background ordering,
   sky behavior and no GPU validation errors. Then test one resize and teardown
   to reject stale resources. The parent remains sole build/deploy/game owner.
5. Only after that proof, calibrate DH reconstruction against the actual
   infinite reversed-Z projection and near plane. The common header's finite
   far-plane defaults do not establish metric correctness. DH debug precedes
   any GI/AO beauty comparison; external total frame timing is still required
   because engine GPU timers exclude injected work.

This is a scoped future task, not an implemented handoff. No additional shader,
executable tooling, renderer change, build, deployment or runtime attempt was
made for this diagnosis update.

## Continued implementation: explicit world snapshot (runtime proof pending)

The next authorized implementation slice adds an opt-in handoff rather than
another GenericDepth setting test. `WGR_RESHADE_SCENE_DEPTH=1` is effective only
with optional loading enabled in a developer single-player WGPU launch. It is
absent/off by default and is not a persistent look preference. The bounded first
case requires Vulkan, HDR, exactly one sample, scene/output dimensions equal,
and the engine's finite forward infinite-far projection. Unsupported cases do
not bind a fallback or claim usable depth.

`rust/src/scene_depth.rs` records an original WGSL pass immediately after the
first completed world segment, before the ClearDepth/Resolve/UI seam. It samples
the depth-only scene view into its own R32Float texture; the original attachment
and all terrain/water/sky rendering remain unchanged. A 16x16 grid samples the
centres of screen bins at the same boundary, at most once per 60 frames. Only a
4096-byte padded staging buffer is mapped asynchronously; numeric counts and
min/max describe those 256 samples, not a full-image reduction. The witness
reports its own frame and resource generation separately from the live image.
Empty, uniform, nonfinite, out-of-range and stale-generation witnesses cannot
qualify the binding. This is finite numeric evidence, not visual alignment proof.

The versioned POD contract is checked on both sides (1208-byte publication,
1232-byte query state). The Rust producer pins only an already loaded add-on and
calls optional exports; normal startup has no new required DLL. The add-on
checks API, native device and queue, tracked image/view identity, dimensions,
format and numeric witness before binding `OP_SCENE_DEPTH`. It does not touch
GenericDepth's `DEPTH` binding or `bufready_depth`. The original probe's new
`OPENPOSEIDON_ENGINE_DEPTH=1` variant reads the independent semantic with zero
scene overlay and a bridge-driven readiness strip. A green strip here still
does not prove pixel alignment or reconstruction correctness.

The contract follows the pinned official API rather than reinterpreting opaque
handles speculatively:

- [Resource/view handles](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/include/reshade_api_resource.hpp)
  document Vulkan VkImage/VkImageView.
- [Native device/queue access](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/include/reshade_api_device.hpp)
  documents VkDevice/VkQueue and queue idle waiting.
- [Texture semantic binding](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/include/reshade_api.hpp)
  requires shader-resource state when effects execute. The producer records
  wgpu 29's explicit native-interoperability transition to RESOURCE, which the
  installed Vulkan HAL maps to SHADER_READ_ONLY_OPTIMAL for R32Float.

Publication follows the engine's queue submission and precedes presentation on
that same native queue. Subsequent engine writes also use that queue; the
external consumer performs only shader reads and leaves the documented layout
unchanged. Before resize/replacement/renderer destruction, the producer calls
invalidation on its serialized render/present thread. The bridge clears borrowed
bindings, waits the matching external queue (wgpu's submission fences cannot
cover injected work), then permits owner resources to drop. The add-on never
destroys the engine's image/view. Resource generations prevent old numeric
witnesses from authorizing replacements. A separate present queue, MSAA,
upscaling, reconstruction and vendor-specific multi-queue support are outside
this first implementation.

The optional state harness now exposes `sceneDepth` with binding status,
publication/bound/sample frames, generation, dimensions, near plane, projection,
sample counts, range and row-major grid. Status 1 means waiting for valid numeric
proof; 2 means binding accepted; 3 identifies device/queue mismatch; 4 identifies
dimension/sample mismatch; 5 identifies invalid resource/publication. Numeric
success alone never sets `visualAccepted`.
The add-on recomputes the grid's range/count to reject inconsistent claimed
statistics and will not bind using a witness older than 120 producer frames.

The parent must integrate/build the matching engine/DLL and build the updated
optional add-on before runtime verification. A CPU-compiled add-on is available
in this worktree at `build/scene-depth-bridge/OpenPoseidonReShade.addon64`; it has
not been installed. Stage a **fresh** isolated runtime from the already prepared
official core cache, this updated add-on, `OpenPoseidon.ini` and the original
`Shaders/OpenPoseidonLook.fx`, then run the zero-download preparer from the new
source tree:

```powershell
python ./scripts/Prepare-ReShadeDepth.py C:/Temp/op-scene-depth-staged --probe-only
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <new-installed-commit> -RuntimeDirectory C:/Temp/op-scene-depth-staged -Mode SceneProbe
```

The installed capture helper opts into the new world snapshot only for
`SceneProbe`, forces one sample/HDR/render scale 1, requires a valid grid sampled
after the locked camera pose, and rejects a generation change during capture.
GenericDepth preservation is zero for this mode; its selected DEPTH resource
is unused by the independent engine-semantic probe.
It captures the bridge's final-frame native/processed pair and preserves the
numeric receipt. Parent inspection must find terrain/building/foliage depth
aligned with the native image, meaningful foreground ordering and correct sky,
then verify resize and clean teardown. No DH debug/beauty comparison is enabled
by this mode. Reconstruction and GI remain gated until this proof succeeds.

Verification completed without GPU access: four focused Rust tests pass
(including Naga parsing/validation of both new fragment entries), the optional
add-on compiles against the pinned locally prepared official headers, two CPU
tests of that built add-on's POD exports reject invalid ABI/stale/invalid numeric
publication and prove publication alone does not equal binding, and three
offline preparation tests pass. PowerShell parsing succeeds. The CPU add-on test
does not call AddonInit or create any runtime/device. No integrated engine build,
game launch, deployment, actual numeric depth, ReShade FX compilation, resize
capture, screenshot acceptance or total injected GPU timing has been performed
in this implementation worktree. The earlier three failed GenericDepth probes
remain valid evidence for the old path; this explicit snapshot is unverified
until the parent runs it.

## Parent runtime confirmation and metric DH adapter

The parent executed the new `SceneProbe` on installed commit `959d1133`.
`build/reshade-depth/20261008-110657-SceneProbe/result.json` records 256 finite
bin samples, 227 nonzero, range 0..0.12434253096580505 and actual near plane
0.0957142934203148 m. Projection p00=1.0714285 and p55=1.904762 came from the
engine. The parent visually checked aligned terrain/tree silhouettes and weapon
against the native capture, with white sky in the raw-depth visualization.
This establishes the explicit handoff for that fixture. It does not validate
GenericDepth, metric DH reconstruction, GI, resize or other camera paths.

The continued optional slice prepares `OpenPoseidonDH.fx` as a **marked local
adaptation** of the same pinned GPL-2.0 upstream source. The unmodified upstream
file and its exact licence remain staged; hashes of the derivative and original
helper appear in `DEPTH-PROVENANCE.json`. No upstream shader is committed here.
The new original `OpenPoseidonSceneDepth.fxh` uses MIT with the complete notice,
so it can accompany the GPL-2.0 derivative without requiring GPL-3-only code in
that combined shader. Preparation fails on an unknown upstream hash or changed
patch boundary, never guesses a patch or overwrites a different existing file.
This slice used the existing offline source cache and made zero downloads.

The adapter reads `OP_SCENE_DEPTH`. The bridge drives its near plane, p00/p55 and
projection offsets p08/p09 from the current publication immediately before
ReShade effects, and explicitly clears readiness on invalidation. Metric view z
is `near/raw`; engine forward projection followed by vertex reversed-Z makes
that inversion exact. View x/y use the live projection, and inverse projection
returns the corresponding screen coordinates. DH uses screen-down Y; a matching
orthonormal reflection preserves distances and angles. Unsupported skew or
nonpositive projection scales are rejected by the producer rather than silently
reconstructed. No estimated FOV, near=1 or finite engine far plane is used.

DH retains normalized depth for its existing heuristics. The explicit working
range is 1000 m, distinct from the engine's infinite far plane. Metric positions
are recovered by multiplying normalized depth by that range. Raw zero/absent
bindings use a beyond-range sky sentinel; samples beyond the working range are
excluded from tracing. Game-specific weapon scaling, multiplier-five and
roughness-driven position displacement are removed from the local adapter.
There is no separately projected weapon-depth classification or motion provider.
Accumulation stays one; SSR and ambient removal are disabled. This preserves
bounded screen-space limitations: no off-screen/hidden geometry, no hardware RT,
no validated temporal stability or total injected GPU performance yet.

`SceneDHDebug` selects only `DH_UBER_RT@OpenPoseidonDH.fx`, debug=5. Its original
adapter visualization uses `log2(1+metres)/log2(1+1000)` grayscale, with blue sky
or beyond-range samples. `SceneDH` selects the same adapted effect, debug=0,
with conservative 1/3 rendering scale and two rays. Both explicitly enable the
same checked engine snapshot under one-sample/HDR/render-scale-1 gates; they do
not use GenericDepth. Legacy generic DH presets remain separate and unaccepted.
No preset or persistent preference is automatically activated.

CPU tests evaluate the **actual FX helper arithmetic expressions** against the
engine forward-projection/reversed-Z pipeline for many known metric positions,
off-axis offsets, multiple tracing ranges, and sky/unready cases. This checks
round trips and metres, rather than only matching shader text. Four Rust tests,
four preparation tests, three reconstruction tests and two built add-on ABI
checks pass; the add-on rebuild and PowerShell parsing pass. The generated DH
FX has not yet been compiled by ReShade or visually accepted in this slice.

An optional `-VerifyResize` uses the existing tested `triSendAltEnter` command,
then returns to windowed mode. It requires changed dimensions, a newer resource
generation on each transition, matching binding dimensions, fresh-generation
numeric evidence and exactly one recovered technique. This is opt-in and adds
no new engine command. Clean exit code zero plus renderer/log checks records
teardown completion, not an independent GPU race proof. The parent can first
run this lightweight check with `SceneProbe`, then the DH debug/beauty gates:

```powershell
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <new-installed-commit> -RuntimeDirectory <fresh-stage> -Mode SceneProbe -VerifyResize
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <new-installed-commit> -RuntimeDirectory <fresh-stage> -Mode SceneDHDebug
# Only after the parent accepts the debug reconstruction/alignment:
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <new-installed-commit> -RuntimeDirectory <fresh-stage> -Mode SceneDH
```

A complete isolated staging directory is prepared at this worktree's
`build/scene-dh-stage`, using the already verified official 6.8 core, the newly
CPU-built add-on, the original helper and pinned GPL/CC0 files. No game launch,
GPU access, integrated build or deployment occurred here. Parent captures and
external total-frame timing remain required before any beauty/performance claim.

## Runtime lifecycle proof and Reinhard feedback correction

The parent ran installed `905ba2dc` with `SceneProbe -VerifyResize`: resource
generations changed 1 -> 2 at 3441x1440 -> 3 at 1600x900, each with a fresh bound
numeric grid, then clean exit. This confirms the tested resize/rebind/teardown
fixture. The parent also visually checked the metric DH debug image: aligned
logarithmic depth and blue sky. However, the strict harness correctly failed the
`20261008-113420-SceneDHDebug/ReShade.log` X3020 diagnostic in both original and
adapted DH: reinhardTex was sampled and attached in the same pass. ReShade calls
this a successful compilation with warnings in its summary, but the nested
`error X3020` is a real correctness failure and remains a hard gate.

The pinned shader's `PS_ReinhardPass` calls `compureResult(..., true)`. That shared
function's other branch samples reinhardSampler, and ReShade 6.8 records the
transitive read even when the caller passes the constant true. The generated
local adaptation now keeps the original 1/64 temporal blend in a separate
retained 1x1 RGBA16F work target. A subsequent pass copies its complete value to
reinhardTex with blending explicitly disabled, before the final merger. The
work and published textures retain identical values after every frame, including
frames discarded when Reinhard is disabled; current-frame merge timing and the
upstream blend are preserved. No pass samples its own attached image.

The untouched, hash-verified GPL source is now staged as
`Sources/dh_uber_rt.fx`, outside the `Shaders` effect scan. The installed helper
copies it for provenance without scanning/compiling it. Legacy generic DH modes
are explicitly refused; use `SceneDHDebug` / `SceneDH`. The preparer refuses an
old staging directory containing an original shader under active Shaders and
asks for a fresh isolated destination, preserving existing files. All original
source/licence/hash receipts remain available; no upstream shader is committed.

Six preparation tests now cover exact retained-work/publish transformation,
unchanged original/hash provenance, active-scan exclusion and no-overwrite
failure, as well as temporal blend equivalence including discarded frames.
Three metric expression tests and two built add-on ABI tests pass. PowerShell
parsing passes. Actual pinned-source preparation completed from the existing
offline cache. This correction changes no engine, renderer or add-on code and
needs no new engine build. No game/GPU launch was made by this agent.

Fresh full stage: this worktree's `build/scene-dh-feedback-fixed-v2`. The parent
must rerun `SceneDHDebug` with that stage, retain the strict error gate, inspect
the aligned depth result, then run `SceneDH` only after the debug gate succeeds.
The corrected shader's ReShade compilation and GI beauty/performance are still
pending that runtime check; prior visual depth evidence does not waive X3020.

## Optional 4x MSAA source support (runtime proof pending)

The parent confirmed corrected `SceneDHDebug` and `SceneDH` on installed
`905ba2dc` at 11:50/11:53, with visible depth effects. Distance lifting/paling
remains a quality concern; this is not final beauty or cost acceptance. A newer
installed `25abb7dc` SceneProbe at 12:23 again confirmed the bound/aligned depth
fixture (256 finite, 227 nonzero).

The minimal 4x extension reuses the existing nearest-covered-sample depth
resolve, explicitly refreshed at the world/UI snapshot boundary. That resolve
already outputs a single-sample Depth32Float texture compatible with the
original snapshot shader. It does not use the water farthest-sample resolve,
which can remove partial-coverage foliage silhouettes. No new world rendering,
MSAA geometry or material pipeline is introduced. The published R32Float image
remains single-sample; publication flag 4 records that the source was resolved
4x MSAA. Changing between 1x and 4x invalidates the resource generation and its
numeric witness. Other source sample counts and scaling remain unsupported.

The resolve runs only inside the opt-in snapshot block, avoiding work on normal
launches. This source slice does not silently change user graphics settings.
The installed helper permits explicit `-MsaaSamples 4` and requires the reported
source count to match the fixture. CPU checks validate the support/rejection
and contract flags. Actual 4x edge alignment and numeric evidence remain for
the parent to capture with `SceneProbe -MsaaSamples 4`, then DH debug only after
acceptance. This is separate from the forthcoming live UI selection/gating work.
