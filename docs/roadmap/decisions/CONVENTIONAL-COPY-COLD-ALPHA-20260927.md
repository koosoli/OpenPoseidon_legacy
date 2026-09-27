# Conventional pool-copy trimming and operation-local cold alpha

The installed `dbe68b03` checkpoint passes the bounded source/unit checks and its narrow runtime probes. Pool growth/shrink now copies through the highest occupied allocation, preserving all interior holes and stable offsets. The optional cold texture handoff can classify the same owned top-mip blocks during admission and still upload the complete selected chain later. The cold feature remains off by default; this checkpoint does not establish a causal performance gain or default readiness.

## Implementation and validation

The allocator's sorted, coalesced trailing-free range supplies an O(1) exclusive high-water bound. Both grow and shrink retain existing capacities, policy, allocation offsets, generation changes, and retirement semantics. A zero prefix skips the empty copy submission. New allocations write their complete spans; unused trailing bytes have no live consumer contract. This trims copy work, not buffer capacity or physical device memory. Exact optional `WGR_GEO_POOL_COPY_DIAGNOSTICS=1` logs at most32 process-prefix notices and a cap marker; it has a cached flag branch during reallocations and no new per-frame scan.

All-five build203 passed. The first pool-test attempt failed compilation because the old test used `device.pop_error_scope` rather than wgpu29's returned `ErrorScopeGuard.pop`; preserve that failed log. The repaired actual pool suite passed17 tests with no skips, including full vertex/index GPU readback across growth and shrink, separated surviving allocations, and interior-hole reuse. The first cold ON run skipped all six cases with the default two workers and exited4; it is not successful coverage. The correctly configured single-worker ON run passed563 assertions/six cases without skips; the OFF test passed two assertions/one case.

`PeekTopAlpha` is a synchronous, joined-owner borrow inside the existing operation-local admission scope. It checks exact source member, bank-header dimensions/offsets, selected format, and cancellation token; classification remains caller-local until the post-visitor recheck succeeds. It neither consumes nor exports the chain. The later upload keeps its existing once-only claim. Refusal falls through to the existing reader. The experiment is covered by existing cold source16MiB/selected-chain16MiB and ready128MiB caps; these are logical capacity limits, not allocator/native-handle/RSS bounds. No second worker read, persistent alpha cache, policy change, or arbitrary texture readiness is claimed. Default cold handoff stays off, with its cached branch and scalar/TLS footprint acknowledged.

## Actual completed runtime

The DayZ prefix screen `build/stream-residency/occupied-prefix-dayz-20260927-disabled-20260927-192612` passed normal shutdown. Its12 contiguous notices1–12 showed nominal1,255,145,472 bytes versus copied1,252,122,152, saving3,023,320 bytes (~0.24%) in this observed process prefix. No cap marker occurred; this is not proof of complete route totals or capacity reclamation. Returned camera residency was19,923, required0, pendingfalse. Geometry live710,591,632/capacity1,275,068,416/retired0; textures2,749,224,436 across2,488 records; backend allocation5,301,730,128.

| Screen | Frames | Mean ms | Usual median ms | Nearest-rank p95 ms | p99 ms | Max ms |
|---|---:|---:|---:|---:|---:|---:|
| Earlier92 default-path screen | 624 | 58.363009615 | 26.5195 | 185.105 | 511.312 | 1062.104 |
| Installed dbe prefix screen | 760 | 45.478965789 | 25.9735 | 144.786 | 490.696 | 904.133 |

Percentiles use sorted index `ceil(n*p)-1`; even-count medians average the middle pair. Both dropped0 and returned19,923/required0, but allocated texture/geometry work differs. Terrain/call CPU timing was enabled and the candidate added copy diagnostics; model PBO1/DDC0/20,000 budget is the controlled fixture, not every normal default. Ordered cache, source, asynchronous admission, profiling, and unequal work prohibit causal FPS/no-regression acceptance.

The untagged strict cold attempt `build/stream-residency/cold-owned-alpha-on-20260927-20260927-192150-eb8ebb` failed `NotExercised` after normal shutdown: peek0 and no source alpha FLAG. BC3 compression does not itself imply the engine should classify alpha. The fixture-only script changes `ac32771e`/`81c073b2` explicitly author the original PAA alpha tag, keeping the compiled installed pair unchanged.

Tagged ON `build/stream-residency/cold-owned-alpha-tagged-20260927-20260927-192902-f3d8b7` passed: coldAlphaPeeks1 versus baseline0, and one actual5,440-byte source-validated successful operation-local chain upload. Tagged OFF `build/stream-residency/cold-owned-alpha-off-20260927-20260927-193109-b231c6` passed with coldAlphaPeeks0 and no cold upload rows. Both admitted the two ordinary models with actual GPU vertex buffers and camera resident2/required0, exited0 with normal shutdown and no forced termination. Source capability is limited to this actual unique primary PAA and selected four-level5,440-byte chain; the complete authored five-level chain remains5,456 bytes. No full material-stage, proxy, source-family, or general model readiness proof follows.

Root inspected both tagged ON/OFF opaque red-box fixtures with the same apparent opaque RGB, and both earlier92/candidate DayZ settled local bus/kiosk/building/tree scenes without obvious new missing geometry. This is narrow local still-image scope, not all-world/all-pass/material equivalence or performance acceptance. The earlier untagged failure remains recorded.

The installed pair recorded by the actual provenance is EXE `9924A3E0B621A9D7FBAD70385774244B02D0E5987AA93DA166640FEED3CEAC9C` and DLL `00498E5AE4E71026C3E391415911AC0284D994D7AB20A22959134AD76B2A6A8E`, clean stamp `dbe68b03`19:21. Immutable compiled source snapshots use gitshow dbe; script snapshots use gitshow81c073b2. Later source work is excluded.

## Remaining gates

The whole streaming plan remains OPEN, `performanceAccepted=false` and `defaultReady=false`. Pool prefix savings are small in the observed screen. Camera/frontier selection, general ownership/reclaim, all-pass pixels, full source/material coverage, and equivalent-work performance acceptance remain independent gates. Cold handoff remains explicitly off; no workload threshold or acceptance criterion was relaxed.

## Completed artifact manifest

| Artifact | Bytes | SHA256 |
|---|---:|---|
| build/build-conventional-copy-alpha-20260927.log | 90512 | `12EDC48F752BFE7C84E4CCABEEF9574FCF2F3CD704647F7EA15EB6E24A657DF5` |
| build/test-occupied-prefix-copy-20260927.log | 5854 | `FD4C5C7A6E4BC9035392E059B68DE88FFD103051BFC9220D396CDCF22685D1A2` |
| build/test-occupied-prefix-copy-repaired-20260927.log | 13585 | `D16700B7738268D8389C1140946558B7AB671B3AD792C5E73C5E7AE07329B1D3` |
| build/test-cold-alpha-peek-on-20260927.log | 1730 | `0682301D1B4AC8A645A9174938655FA4A9F1453AFF79BF600DDA12CE8310874B` |
| build/test-cold-alpha-peek-on-workers1-20260927.log | 106 | `77B3ACFE2847705844A062EE63F4F0E5A21CDC760ED0A423BB00B4D5C1610726` |
| build/test-cold-alpha-peek-off-20260927.log | 103 | `8943FC49B14E67CF9958506920C8D1A9A6BAF181F79A125EA40B0F81B49C284D` |
| build/occupied-prefix-dayz-proof-20260927.json | 311 | `4816E65C85A6DB0016397C8948B7D6167D3D132562B7CBF31C296B4AF5B2B237` |
| build/stream-residency/camera-prefetch-dayz-default-20260927-disabled-20260927-190408/result.json | 79550 | `F526F58D4265AEABD66197AF3BDCECA5E039135E437454E5D2453D5F8C1BBE57` |
| build/stream-residency/camera-prefetch-dayz-default-20260927-disabled-20260927-190408/provenance.json | 3317 | `60E35079AA90A7361583C2B068E5BAECBE359358981FE7810FF9E9828F174302` |
| build/stream-residency/camera-prefetch-dayz-default-20260927-disabled-20260927-190408/engine.log | 3232766 | `ECA9949E024C096159E43F9D80B84B569A38A0D48BB37B6E3FE08F69BE5CD2E0` |
| build/stream-residency/camera-prefetch-dayz-default-20260927-disabled-20260927-190408/route-frame-ms.json | 6879 | `B838B1A6F51B4466D860890CF56250FA53413901E5560BF5B76247C60175CA94` |
| build/stream-residency/camera-prefetch-dayz-default-20260927-disabled-20260927-190408/returned-settled.png | 2162961 | `60BC5A92994B69CBCE526D0906E1343C5337518255A1E8C9DFCE15D2565ABBC6` |
| build/stream-residency/occupied-prefix-dayz-20260927-disabled-20260927-192612/result.json | 78825 | `FF8DDD574CBB8E4AEDCD6173823B6D24E96F1430A1CA8E6AAAB6D68C2CD85AF0` |
| build/stream-residency/occupied-prefix-dayz-20260927-disabled-20260927-192612/provenance.json | 3352 | `D90FA731D041A59D56216F3085A5C204E097B8202D83275552C4B88120E227E0` |
| build/stream-residency/occupied-prefix-dayz-20260927-disabled-20260927-192612/engine.log | 3216351 | `463332D9B6B43E92ACE059109119A9811DBBD5EB19AAC663C6AA728A6C9859AA` |
| build/stream-residency/occupied-prefix-dayz-20260927-disabled-20260927-192612/route-frame-ms.json | 8303 | `859ED2209178FD33C20945FEF332F2DAA8B336A4C39224AF162B704894B4DDE7` |
| build/stream-residency/occupied-prefix-dayz-20260927-disabled-20260927-192612/returned-settled.png | 2162396 | `81AA33BDBB6A08CFCB1F9394FBC7FAEE335ED281EE26AE6CF1044038D9D70B57` |
| build/stream-residency/cold-owned-alpha-on-20260927-20260927-192150-eb8ebb/provenance.json | 5940 | `3CFBA858EB8FBC841DAB9E5F4D74D705DEFC0C50529C527984C4C96828A08F1F` |
| build/stream-residency/cold-owned-alpha-on-20260927-20260927-192150-eb8ebb/engine.log | 81374 | `1C100A58FF795476105268D3D08D318735F5AEB6B93FBBBF82AE43809AAE8D83` |
| build/stream-residency/cold-owned-alpha-on-20260927-20260927-192150-eb8ebb/failure.json | 182 | `4222AE564CFE5102A657D0C8B26A37E832500B6DD03FA795FC63767E858544C4` |
| build/stream-residency/cold-owned-alpha-tagged-20260927-20260927-192902-f3d8b7/result.json | 9023 | `423D059BD5503625ABC706CF39EDFCD6FF8F01F93B481F7062DEC1C244FFCA70` |
| build/stream-residency/cold-owned-alpha-tagged-20260927-20260927-192902-f3d8b7/provenance.json | 6164 | `4863CD16FFB111802E70DC77532FD4BB6773F9E5967FCBB61BB890D7259AD8B3` |
| build/stream-residency/cold-owned-alpha-tagged-20260927-20260927-192902-f3d8b7/engine.log | 95021 | `9B8A2B14E911E321B06C1368A1C3162D20D2680BA90028E3278A90BF037D3227` |
| build/stream-residency/cold-owned-alpha-tagged-20260927-20260927-192902-f3d8b7/fixture.png | 1767999 | `236A9BED3B3EE4988B287B18654E325A76736F4C28E477DAAAEE378E46AAB4F1` |
| build/stream-residency/cold-owned-alpha-off-20260927-20260927-193109-b231c6/result.json | 8547 | `A828CE68EA5FE0591449A93C18A3BC3DA918B9663B52DA7A40FE2720DAF0035A` |
| build/stream-residency/cold-owned-alpha-off-20260927-20260927-193109-b231c6/provenance.json | 6155 | `92F2DFA27AA6A79333BCB9E17BE01A96005290768EFBA690ED22C7278B2DA028` |
| build/stream-residency/cold-owned-alpha-off-20260927-20260927-193109-b231c6/engine.log | 92505 | `EFB91EFBC73D5CB6E2421CABBF1055111A795CDDC6830EE99AF4F11C1C2C5D75` |
| build/stream-residency/cold-owned-alpha-off-20260927-20260927-193109-b231c6/fixture.png | 1767837 | `38CE96630FC6AE27A7E29D703426328985BE6E635563DE5B6B3E59B5626A3767` |
| build/conventional-source-dbe68b03/engine/WgpuRenderer/rust/src/gfx3d/pool.rs | 52220 | `99D82BEF8EFB232077FBEA584EFB77950583A746E4C2EB32E219B876D5DF8FA3` |
| build/conventional-source-dbe68b03/engine/Poseidon/Graphics/Textures/ColdPaaHandoff.hpp | 7770 | `FE2D169AE0E5173062F944247719600C587CB5B5AE2B9AD90020D2D92DC10408` |
| build/conventional-source-dbe68b03/engine/WgpuRenderer/TextureWgpu.cpp | 116521 | `1F6085E772135A2FD7F1A1E4BC678DE7F8DA185B546E713DAF4283FA9066399E` |
| build/conventional-source-81c073b2/scripts/Test-ColdPaaHandoff.ps1 | 12734 | `6074248796B969913395FD76EE3FD2AEC9EF81EC3FF5A56BA522E207BBF61522` |
| build/conventional-source-81c073b2/scripts/streaming/build_cold_paa_handoff_fixture.py | 6521 | `85B5059D91A42C9C21312CCB3D8C56697D9277FCBA95AB55AC0CBA79CB49BA90` |
| build/conventional-source-81c073b2/scripts/Test-DayZStreamingLatency.ps1 | 36400 | `966061188CB6CF3D758C70FE179D3CFE8138AB8F01DE0B316A3EDA7CF1B94CF3` |
