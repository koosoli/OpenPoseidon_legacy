# Native Reforger and upstream cross-audit

## Scope and provenance

Inspected worktree: asset-obs, branch asset-obs-001. Starting HEAD db3b0183,
also the fetched safety/consolidated-v2 HEAD, not the attachment's historical
212019e8. Fetched ofpisnotdead/main: 0e24d744. PR head refs are evidence, not
proof that those PRs have merged. No blanket cherry-pick or GL33 port.
Foreign Cargo.lock and scratch content remain untouched.

This is an INCOMPLETE audit, not acceptance of all 17 candidates. In particular,
the many-unit, marker, fixed-seed/save/load and multiplayer matrices below have
not been run. Missing benchmark evidence is not evidence of a speed improvement.
The user's native far coverage and forest appearance requests remain OPEN.

## Reforger: concrete changes and remaining cause

d8404fa7 corrects DdsTextureSource::DoDecodeImpl in
engine/Poseidon/Graphics/Textures/DdsImport.cpp: the decoded native RG-normal
path previously overwrote alpha with red, losing an authored auxiliary channel.
Native NMO/NTC/NHO keep RGBA; legacy BC5 retains its existing AG conversion.
Synthetic BC7 regression preserves R=128, G=128, B=40, A=200. No original asset
is redistributed. Native BC5 has no authored alpha and correctly remains opaque.

dae19d91 connects native NHO alpha to ambient ground shading in
engine/WgpuRenderer/rust/src/terrain/terrain.wgsl. This is AO, not colour or
normal data. It reuses the existing normal fetch, blends at cell/detail edges,
and fades under snow. Legacy materials retain AO=1. No extra texture allocation
or sampler, no collision/physics/geometry change. WGR_NATIVE_GROUND_AO=0 is the
same-build diagnostic rollback. Default is on.

Contract: https://community.bistudio.com/wiki/Arma_Reforger%3ATextures
NHO = normal RG, height B, AO A; NTC = normal RG, transmittance B, cavity A.

Validation: full Game/Tests build; DDS suite including WGR_NATIVE_DDS_PREPARE=1
passes 1201 assertions in 29 cases; six native/legacy terrain tests and
entry-shader composition pass. Installed Coast-ground-ao-off/on both pass capture
and clean shutdown: 22369 registered, 8501 main instances, 15938748 triangles;
1920x1080 native, MSAA4, DLSS off, governor range 1. Endpoint GPU 36.945/35.870 ms
is ONE ordered pair, NOT an FPS improvement. Inspected images show subtle ground
shading changes, not parity with the reference or a resolved pale forest.
Artifacts: build/native-reforger-perf/Coast-ground-ao-{off,on}/run-01.*.

Native far omission remains the roughly 20k resident admission cap (~563 m in
the measured forest), not a lack of authored XOB LODs. LandLoadEnfusion.cpp builds
the ladders, but LandSave.cpp::UpdateModernObjectResidency stops admitting cells
at the count budget. Native does not publish the generic SetFarProxyInstances
path; that path's plain prisms/cards would not satisfy authored far meshes anyway.
Do not increase fog, reduce viewing distance or claim that bigger residency is
free. Required integration remains coarse visual-only authored instances through
the existing model preparer/cache, near-instance exclusion, bounded publication
and world-generation retirement. Not implemented by this change.

Actual material extracted with PoseidonTools pak cat:
Assets/Vegetation/Bush/Corylus_Avellana/Data/b_corylus_avellana_2_polyplane.emat,
inheriting Assets/Vegetation/_Data/crown_base.emat. No simple Color tint in either;
the suspected missing multiplicative tint is NOT confirmed for this crown.
The source does contain GeometryAO*, EllipsoidAO*, WorldColorizeMap and SSS
parameters. EmatMaterialAdapter.hpp does not map the spatial AO/colour variation.
Their exact shader reconstruction is not established; do not invent equivalent
semantics or turn a global contrast multiplier into an alleged material fix.
Forest-leaf-light-ablate passed lifecycle but did not solve the pale appearance;
do not ship global legacy foliage-light ablation as a fix.

## Candidate decisions

Statuses distinguish code presence from runtime proof. SHIP requires its listed
tests; EXPERIMENT below is not permission to merge an unmeasured optimization.
Paths below are repository-relative.

### 1. Head/Tank initialization (#173, a5268534)

PARTIAL. The real Head constructor over poisoned storage reproduces three NaNs
in actual/wanted/speed random lip (3 failed checks, 2 passed). These three floats
now initialize to zero. Correction to the initial audit: next-change is NOT
uninitialized; Foundation::Time already defaults to zero. Keep that deadline and
the existing unconditional Simulate/NextRandomLip schedule, rather than introduce
a distant deadline or skip disabled ticks and thereby change shared RNG draws.
Tank.cpp still leaves _doGearSound unset before Sound reads it; NOT patched here.
The constructor regression is targeted evidence, not campaign/save/load/MP
acceptance or proof that this caused the reported face screenshot. Those runtime
gates and a fixed-seed sequence comparison remain open. Old undefined values
cannot be a valid checksum baseline.

### 2. NetServer teardown (#122, 36029253)

PRESENT. NetTransportNet.cpp uses JoinServerPeerWithoutPoolLock with retained
localServerPeer, closes outside poolLock and retires afterward. No new sleep or
second locking scheme. Rebuilt PoseidonCoreTests and reran the concurrent
poolLock/join helper regression: 2 assertions in 1 case pass. This models the
ctrlReceive lock dependency; it is not a live full CreatePlayer/MP lifetime test.
No new implementation benefit demonstrated. Risk high if disturbed. REJECT duplicate.

### 3. Script street lamps (#310, 9eff4a62)

MISSING at baseline; patched and installed-regression tested as d91645bc.
ObjectClasses.cpp constructor dereferenced the passed null shape instead of the
base-resolved type shape; InitShape also dereferenced an abstract empty model.
VehicleTypes.cpp now refuses the abstract no-model lamp with balanced type release;
the constructor reads GetShape and abstract InitShape skips hit-point setup.
Integration create_vehicle_street_lamp.test.sqf passes in the installed WGPU game
(20.4 seconds): concrete Wood/Metal, abstract objNull, deletion and clean exit 0.
Log confirms the intended no-model refusal and Shutdown complete. Artifacts:
build/upstream-crossaudit/streetlamp/create_vehicle_street_lamp/game_*.log.
Same existing model/light/simulation behavior for valid
lamps; not a lighting-performance change. Save/load/MP not separately exercised.
Risk low, independent of renderer. SHIP; this is not complete MP/save-load acceptance.

### 4. Invalid simple path (#237, 2445dc39)

MISSING. AIUnit.cpp::IsSimplePath checks distance then unlocks and converts grid
coordinates to integers. NaN can pass the distance comparison; extreme absolute
coordinates can be near each other yet invalid for conversion. No hang repro run.
Small fix must reject nonfinite ALL endpoint components and out-of-range grid
coordinates before unlock, not merely copy upstream's NaN-only check.
Test NaN/Inf/extremes, boundaries, finite oracle and lock state, then fixed-seed
ordinary missions/save-load. Risk medium. EXPERIMENT, no patch yet.

### 5. Static crew assignment (#209, 0c82386c)

NOT REPRODUCED at runtime; upstream guard absent in AIGroupCmd.cpp::AssignVehicles.
Existing driver-assigned condition excludes driverless static guns, supporting
the crew-reassignment hypothesis. Need two grouped weapons, kill one gunner,
observe surviving gunner across repeated group ticks. Only then skip settled
assigned crews, with car/tank/transport regression and MP authority tests.
Gameplay reassignment risk medium. EXPERIMENT, do not call code match a repro.

### 6. Vehicle leg transform (#305, 759a2803)

PARTIAL. SoldierOldMove.cpp resets _legTrans on boarding, but SoldierOldSim.cpp
still updates on change without upstream's hierarchy-parent condition. This is
not proof boarding reset prevents later slope writes. Required Jeep/UH60/Bike
slope entry, FFV ready/stow/reload and on-foot slope comparisons not run.
Small semantic parent guard only if reproduced; preserve bike animation.
Animation/save-load/pose checksum risk medium. EXPERIMENT.

### 7. Proxy conform state (#303, 0a8b4321)

PARTIAL. Object.cpp already publishes/restores WGPU conform planes; the upstream
GL state call is not applicable verbatim. Need weapon/building proxy interleaving
beside ForestPlain/ClipLand in colour AND shadow passes, including FFV. Parent vs
child shape correctness not proven by the presence of a restore alone. Small fix
would establish child identity at the actual direct/proxy submission boundary.
Simulation-neutral, rendering risk medium. EXPERIMENT, no duplicate global state.

### 8. nearbyint (#105, 5e1fd705)

NOT REPRODUCED as a hotspot. FltOpts.hpp still uses std::nearbyint; Release
assembly and exclusive workload time not collected. Hardware intrinsic proposal
needs ties-even/saturation/NaN/Inf and rounding-environment parity, boundary and
millions-of-values tests, fixed-seed/save-load/MP checksum equality.
Risk high for a foundational numeric change. PARK until measured; no 28% claim.

### 9. AI validators (#111, 95e385d6)

PARTIAL. DebugLog.hpp AI_ERROR deliberately evaluates expressions in Release;
AIUnitImpl.cpp::AssertValid and WorldImpl.cpp::CheckVehicleStructure still exist.
Purity and actual exclusive cost not audited end-to-end. Need 80/160/320-unit
idle/combat, camera near/away, fixed graphics, p50/p95/p99 CPU categories. Only
gate individually proven pure expensive validators; never globally remove
AI_ERROR evaluation. Check simulation hashes/MP/save-load. Risk medium. EXPERIMENT.

### 10. Vector3P transform SIMD (#112, 0b9b8506)

NOT REPRODUCED as a hotspot; current generated-code parity not established.
Profile Vector3P::SetFastTransform before implementing. Require alias tests,
operation-order preservation, 20M random and special values, bitwise output and
fixed-seed simulation checks. New SIMD is not inherently a measured improvement.
Numerical/MP risk high. PARK pending profile.

### 11. Discardable vertex buffers (#126, d45a73c6)

PARTIAL. WGPU still classifies VBSmallDiscardable dynamic, but clean-buffer and
content-hash skips already avoid redundant updates. Count Update/hash CPU,
wgr_mesh_update bytes/calls and tails on Everon/DayZ/combat/shadows before changing
semantics. Do not break animated/shared buffers (including helicopter rotors).
Visual/ownership risk high; simulation normally unchanged. EXPERIMENT, no speed claim.

### 12. Opaque buckets (#126 residual)

SUPERSEDED as a high-priority target, not literally implemented: SceneDraw already
uses packed keys/radix sort. Earlier local diagnostics put combined sorts near
0.058 ms, not the large native loading spikes. Not a fresh matched bucket A/B.
Only reconsider if Pass1Submit shows a material fraction; preserve transparent/
decal ordering and exact object sets/frame output. Risk medium. PARK for now.

### 13. Benchmark LOD lock (c9a5b238 reference)

PRESENT as a fixed-quality governor setting: WGR_LOD_GOVERNOR_RANGE=1 is already
used by Measure-NativeReforger.ps1. The measured forest selects LOD0 with that
setting; normal range selects cheaper LODs and is a different quality comparison.
This is not a universal force-level API. Keep the diagnostic out of gameplay
defaults and record actual LOD histogram. No duplicate needed. REJECT duplicate.

### 14. Marker side index (#95, 7b3d10da)

MISSING side index; NOT REPRODUCED as bottleneck. AICenterImpl.cpp markersMap is
canonical AutoArray, GameStateExtWorldConfig.cpp scans it. Need 100/1000/10000
marker CTI-like query/update timings. If material, auxiliary name index only,
with create/delete/rename/JIP/save-load/net update and iteration-order tests.
State consistency risk medium. PARK until cost justifies extra maintained state.

### 15. Cosmetic RNG (#172, 28c531a6)

PARTIAL instrumentation, not completed separation. Random/randomGen.hpp and
SIM815 tick recording expose sequential draw counts; this does not classify
GRandGen consumers. Full authoritative/animation/cosmetic/UI inventory missing.
No consumer moved. Compare tick hashes across render/effect/camera/FPS variations
and save/load/MP before separating any stream. Gameplay/authority risk high.
EXPERIMENT as an audit, not blanket cosmetic replacement.

### 16. Runtime dumps (#189 ae1ed293 / #190 b5763ae2)

PARTIAL. PoseidonTools already provides config, PBO/PAK and offline mining tools;
complete runtime property-origin/merge-order/bank/type export not established.
Start with missing provenance questions rather than a second dump subsystem.
Test conflicting addons/prefixes and effective values; no per-frame work when
unused. Developer-only output risk low, resource lifetime risk medium. EXPERIMENT.

### 17. In-process remount (#296, 519a46f8)

PARTIAL. ScenePreloader shutdown handling was previously adapted, not the whole
GL texture/remount patch. All bullet mark, particle, decal, helper and cache
references have not been traced across mount generations. Required repeated
A/B mount, old-world shutdown and resource-retirement tests with visible effects.
Do not clear resources still referenced by render overlap. Risk high. EXPERIMENT;
no claim that a successful normal exit proves in-process remount safe.

## Priority and acceptance gates

1. Finish native authored far coverage and material fidelity as separate tracked
   work; the ground AO correction is useful but does not close either request.
2. Small confirmed crashes/uninitialized fields, each independent and regression
   tested. No AI or numeric optimization bundled with them.
3. Profile streaming and many-unit validators before adding threads or SIMD.
4. Low measured sort cost, marker indexing and tooling are lower priority.

Every performance comparison must preserve scene, camera, resolution, DLSS,
shadows and LOD quality; record frame p50/p95/p99, loading spikes and RAM/VRAM.
The current single-pose GPU endpoints do not satisfy that performance gate.
No performance optimization from this audit is accepted into consolidated-v2.

## September 13 follow-up: current upstream renderer candidates

Re-fetched `upstream/main` at `0e24d744` and rechecked the current fork at
`c4ccadab` before making changes. The old candidate hashes had moved, but the
three relevant ideas remain represented by current upstream commits
`02c0d741` (benchmark LOD lock), `cc4bd699` (opaque grouping) and the
discardable-buffer work discussed in issue/PR #126. GL33 buffer teardown,
heightmap-texture and local-light UBO commits remain backend-specific and were
not transplanted into WGPU.

### LOD lock: already present, no duplicate

`scripts/Measure-NativeReforger.ps1` and `scripts/Measure-AIScale.ps1` use the
existing `WGR_LOD_GOVERNOR_RANGE=1` fixed-quality control. It freezes the
governor's range while retaining the fork's own retained-instance accounting.
Adding upstream's second `POSEIDON_LOD_FIX` control would create two authorities
for the same benchmark condition. No production or diagnostic code was added.

### Discardable buffers: instrumented and rejected as an optimization target

`WGR_VB_UPDATE_STATS=1` now records, per `VBType`, create/destroy/cancel
counts and bytes plus update/build/hash/upload counts, bytes, total CPU time and
maximum call time. It is read once at startup; with the switch unset, the
`VertexBufferWgpu` path performs no clocks or atomic counter operations.
`Measure-AIScale.ps1` carries the switch and shadow-map override into its
captured environment so the result is reproducible.

Installed evidence, all at fixed LOD quality:

- Native Reforger Reference, 1920x1080 native, MSAA4, DLSS off:
  `build/native-reforger-perf/Reference-vbdiag/run-01.*`. The eight screenshot
  readbacks make this run invalid for an FPS comparison, but not for the
  lifecycle census. No `VBSmallDiscardable` row occurred: zero creates, updates
  and uploads. Settled static `Update()` gates handled about 182,108 calls per
  two seconds in 5.87-6.19 ms total, about 0.05 ms/frame at the observed rate,
  with zero builds and uploads. Pass-1 sorting settled at 0.086 ms/frame.
- 80 visible idle soldiers, 1280x720, fixed LOD, normal CSM:
  `build/ai-scale/vbdiag-idle-80-Near/run-01.*`. Across about 60 seconds,
  2,373,718 dynamic update calls produced eight builds, two uploads and only
  544 uploaded bytes. Total instrumented update time was 68.863 ms, about
  1.15 ms per wall-clock second (roughly 0.011 ms/frame at 100 FPS). No
  discardable-buffer row occurred.
- Same 80-soldier fixture with `WGR_SHADOW_MAPS=0`:
  `build/ai-scale/vbdiag-lifecycle-idle-80-Near/run-01.*`. Startup requested 33
  dynamic buffers (795,812 bytes) and 26 static buffers (238,828 bytes).
  `VBSmallDiscardable` and `VBBigDiscardable` again had zero creates, destroys,
  updates and uploads. The steady captured frame was 8.760 ms, but this is a
  diagnostic arm, not a same-quality CSM performance comparison.

Conclusion: changing `VBSmallDiscardable` from dynamic to static cannot improve
these active WGPU paths because the candidate class performs no measured work.
The existing clean-buffer and content-hash gates already suppress the update
storm that motivated the upstream GL change. Keep the opt-in census for a
future scene that actually produces the class; do not change its semantics
without such a reproduction, especially because animated shadows and rotor
poses share the surrounding lifetime machinery.

### Opaque bucketing: not implemented after the measurement gate

The native run measured pass-1 sort at 0.086 ms/frame; the 80-soldier runs
settled at 0.004 ms/frame. Replacing packed-key/radix sorting with mutable
per-`LODShape` bucket epochs would therefore have no material whole-frame
benefit in these fixtures and would add ordering/thread-safety risk. The
upstream bucket implementation remains a candidate only if a future matched
capture attributes a material frame tail to sorting. Transparent and decal
ordering must remain outside any such experiment.
