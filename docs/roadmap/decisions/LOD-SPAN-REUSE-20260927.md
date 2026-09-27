# Optional LOD payload reuse — controlled runtime and route screening

## 27 September 15:54 — compiled optional LOD payload reuse; controlled runtime passed

Source9d97c2fa built all five targets in18 scheduled steps and passed25 Rust tests with zero ignored/no adapter SKIP. The actual GPU tests include stable retired A and live B/C plus a live empty model, complete-generation mesh recreation, MODEL/LOD/material storage parity, all three main/shadow/color compute views and relative census/feedback isolation. The deliberate caught panic in the partial-clear transaction test is expected. This is not a pixel/all-pass or game-runtime result.

`WGR_CULL_LOD_REUSE=1` is an exact startup opt-in, default OFF and independently selected from SECTION reuse. An ON-only permanent zero LOD sentinel is excluded from allocation ownership/free lists. Empty births immediately use it. Retirement first compares the current range with its immutable active birth witness; only validated bounded old rows clear. The model birth is redirected to base0/count0/pad0, both MODEL and LOD uploads are marked, and retained-content epoch advances before old payload can become available. Foreign/missing/oversized witnesses tombstone without clearing another owner's rows and sticky-disable reuse. Model IDs never recycle, active LODs never relocate, current allocation/census follow relative remapped model ranges, and feedback retires the old target without assigning it to the new birth. Previously copied reports remain immutable cuts.

Owner8192 and pending/free range4096 caps remain. The4096-row/one-range cleanup limit is per retirement call, not a frame-global budget. Caps/invalid ownership/parallel mismatch/mutable escape preserve append fallback; partial-cleared prefixes remain unreused holes. This does not shrink GPU buffers or close whole-ownership/inflight/free gates. Default cost includes one additional nullable pool pointer, a startup lookup and guard plumbing; it is not zero footprint. The subsequent actual ON runtime reports container capacities below; these are logical payload accounting, not RSS.

The supported deployment installed clean9d97c2fa15:54. The attempted game fixture was refused before launch: game-lock found one external OpenPoseidon instance and the wrapper returned4. At that initial attempt game verification remained pending and no fixture result was invented; the subsequent successful runs are recorded below. No user game was stopped or overwritten. Exact build/test/refusal hashes and installed pair are in LOD-SPAN-REUSE-20260927.md. Historical SECTION notes that the LOD study was then unbuilt are historical; this checkpoint adds compile/GPU-unit evidence only.

Whole plan/StageG remain OPEN, experimental LOD/SECTION reuse stays OFF, defaultReady/performanceAccepted false. Earlier mixed default/SECTION screens, ResourceGrowth and stage-family-upload failures remain. The passing unit campaign and subsequent controlled runtime do not supply default performance acceptance.

| Artifact | SHA256 |
| --- | --- |
| build/build-lod-span-reuse-20260927.log | 2dddce06bf4d20cc06dbf9c360f396fc6c34752138b425fc2821015ae9fba02e |
| build/test-lod-span-reuse-20260927.log | 0f2378a72cbf79649c7e5d6b98de2002ec3b1f669e1f4028b26cbe50d558cf6f |
| build/runtime-disk-lod-section-reuse-on-20260927.log | dc1087b34fc4aafd29873a89cfa035af517af85a55404530f31f799576d03a71 |
| engine/WgpuRenderer/rust/src/gfx3d/cull.rs | 3435e81c8b8b54c7c77d884e238745db083951cd8a7c451b5d07df0cdb509281 |
| engine/WgpuRenderer/rust/src/gfx3d/mod.rs | e3b3b52b0323d5aa0c75ff5042d08e039fc3f3c3188d2319cecde165c5c79ae8 |
| engine/WgpuRenderer/rust/src/gfx3d/section_span_pool.rs | eee89a1bc640d988f487b366499a15b4e6f6930ae6928e8dc78c8d8c676a4e3c |

Clean installed pair supplied by root after supported Deploy.ps1: EXE `50697A56D0A147CE6108A92E0B8625E7E9D117CA0DBE8B3B7C52CED84955F4E9`, DLL `3BDDA97A852DAC35C99B47AE404B9482CA599F370C8A4428106D4D83A9A4BE20`. This identifies deployed code, not an executed fixture.

Actual game verification supersedes the initially blocked attempt: `disk-lod-section-reuse-on-20260927-20260927-160123` and OFF160201 passed/root0/cleanExit. ON shutdown16:01:36.603; OFF16:02:15.175. Both finish14Absent, jobs/reservations0, fineGeneration2 and helper hash unchanged. ON SECTION and LOD each emit one activation, three actual postcommit rows and zero disabled rows; each pool reports327680 logical container-capacity bytes, not allocator/RSS. Actual LOD events are birth3/base1/count1/lodLength4, birth4/base3/count1/lodLength4 and birth6/base4/count1/lodLength5. OFF has no activation/commit/disabled events. The original lock refusal remains a refused launch, not a game failure; its log is preserved.

Actual `lod-section-dayz-on-20260927-disabled-20260927-160254` passed/root0/Shutdown16:04:11.185. Both domains each emit one activation,64 postcommit rows, one capped-prefix marker and zero disabled rows. This proves execution on the DayZ route, not total reuse/whole-table convergence.652 frames/drop0: raw mean55.0970475460ms, usual median24.6635, nearest-rank p95189.194/p99525.165/max899.061; quantiles use sorted index ceil(n*p)-1. Returned19923/required0/pendingfalse, geometry725795512/cap1275068416/retired0, textures2798329684 bytes/2529/backend5348500944. ModeDisabled uses water0/bothreuse1/TerrainPageTimings1/copiedCPU1/WarmProfileOFF; it is an instrumented feature screen, not normal untouched settings. The earlier7d SECTION-only route has different allocations/cache/async work and source, so there is no isolated default or causal performance control. No promotion/no-regression acceptance follows from mixed center/tail observations. Root compared ONboth and the historical7d default-OFF returned image at this POI: no obvious new missing geometry locally. The captures are not pixel-identical or whole-world/native-compatibility truth; no broad visual acceptance follows.

| Runtime artifact | SHA256 |
| --- | --- |
| build/stream-residency/disk-lod-section-reuse-on-20260927-20260927-160123/result.json | 59c15bd7cc080a97a9a05045cc4209040c4f9acc9e1f132f155cdff1ba96342d |
| build/stream-residency/disk-lod-section-reuse-on-20260927-20260927-160123/provenance.json | 06057be49bd457131d1f9545d33df1e6a1253200ce276d345068087e352de0e1 |
| build/stream-residency/disk-lod-section-reuse-on-20260927-20260927-160123/engine.log | 44dc81afdc80157a34153a68dd19e7097051cd3edb71ce8fad4e94f25312d1a7 |
| build/stream-residency/disk-lod-section-reuse-on-20260927-20260927-160123/harness.jsonl | 6a7e053bd2b2fb56f42ed9f603166086d82aa7ac8fbef7ee6bf34b7cdcc86ebb |
| build/stream-residency/disk-lod-section-reuse-off-20260927-20260927-160201/result.json | 9356d35ecb41b865fc0c109e5ff36b19280b7470efef41eaed8b943d0ac79e47 |
| build/stream-residency/disk-lod-section-reuse-off-20260927-20260927-160201/provenance.json | 10e18a5b98ff75319ab16385104b66103b481b61c57c20a7f27cb8b940310574 |
| build/stream-residency/disk-lod-section-reuse-off-20260927-20260927-160201/engine.log | 9b31983956f30e56ff7283c3046836bd729e0cc3b02c7a8b81773891a2406ec4 |
| build/stream-residency/disk-lod-section-reuse-off-20260927-20260927-160201/harness.jsonl | 0998c862dc129611846a36c851ee7a0013e6e31bd71e45e848a7ccdd9ebc0859 |
| build/stream-residency/lod-section-dayz-on-20260927-disabled-20260927-160254/result.json | b1e6af4034f1b692752483ccb49ba016b322155a2854109cabf057885f27dbef |
| build/stream-residency/lod-section-dayz-on-20260927-disabled-20260927-160254/provenance.json | 6c7480373b7cbdf536cb19a9c8b269d04b99110f541eab562e268ddccbee1ecd |
| build/stream-residency/lod-section-dayz-on-20260927-disabled-20260927-160254/engine.log | 84a9aac17faa76712c11760799f7e0dbbb52436cea69a67ec6741856126db81d |
| build/stream-residency/lod-section-dayz-on-20260927-disabled-20260927-160254/harness.jsonl | fb68c0acc6614f1038824c6b3df6577153b44dbeb3a7834273eee021aa82eafe |
| build/stream-residency/lod-section-dayz-on-20260927-disabled-20260927-160254/route-frame-ms.json | cc46f5a61db5dd720ef3b9eaa0c1553c5c91739d0f308d28f6a0a2586c21f0c4 |
| build/stream-residency/lod-section-dayz-on-20260927-disabled-20260927-160254/returned-settled.png | f55ca216ae2582692f0ae71edf443622ca50e276e623177214e26dcf3d1d297e |

Actual runtime provenance confirms clean9d installed pair above; ON/OFF fixture scriptSHA `45ABB555D3111D4F00FB9C61FC1CDD8493C8F9E90E410C56221D9B83486F2EE7`. DayZ source/script/settings are retained in its independent provenance.
