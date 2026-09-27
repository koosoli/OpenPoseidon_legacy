# Optional bounded MODEL source-row uploads

## Verified checkpoint: source77b50518, 27 September17:04 deployment

The opt-in path now has actual GPU-storage parity tests, controlled disk-paging lifecycle proof, and a same-binary DayZ route screen. It writes a dirty source-row interval instead of the complete MODEL table when existing storage is suitable. This is a bounded copying optimization, not model-ID recycling, geometry relocation, pressure eviction or physical memory reclamation. `WGR_CULL_MODEL_ROW_UPLOAD=1` is an exact startup opt-in and remains OFF by default, independent of SECTION and LOD reuse. The streaming plan remains OPEN; `performanceAccepted` and `defaultReady` remain false.

### Mutation and retry contract

A single fixed minimum/maximum interval coalesces MODEL births, actual changed feedback tags, and actual LOD-sentinel retirement. Disjoint changes include their unchanged gap. There is no model-sized dirty map, row scan or sparse allocator. Missing or grown backing storage always receives the complete CPU mirror; an unknown, empty or out-of-bounds dirty interval also falls back to the complete mirror. Empty initialization allocates minimal backing storage without inventing a row write. Existing buffer-growth/rebind handling remains.

The MODEL dirty interval resets only after every selected table-upload body returns successfully. Failure in a later domain retains MODEL dirtiness and its interval for retry. It does not alter immutable model IDs, relative LOD offsets, current census/feedback identity or CPU geometry ownership. It adds no frame-global deadline; existing preparation and registration timing limits remain separate.

An ON-only fixed numeric notice is recorded after `queue.write_buffer` returns and drained through the existing renderer log queue. Only the first16 partial writes are logged, followed by one cap marker; further writes continue without notices. Full initialization/growth writes do not qualify as partial notices. These logs prove selected source writes, not driver execution, submission completion, surviving draw pixels or total route work. The actual ON runtime reports64 bytes of inline `Option<ModelRowUploads>` state; the source ceiling is80. This storage also exists when disabled, together with a startup lookup and two None getter branches. OFF makes no new notice strings or log allocations, but its footprint is not zero.

### Build and controlled lifecycle

`build-model-row-upload-20260927.log` passed all five targets in18 scheduled steps. `test-model-row-upload-20260927.log` passed27 Rust tests with zero ignored and no adapter skip, including real GPU neighbor readback, exact legacy storage parity, missing/growth full initialization and actual tombstone writes. The pure dirty-interval/caught-failure cases also passed. This is a focused campaign; no entire-suite or all-pass pixel claim follows.

The installed stamp is clean `31c80f9c 2026-09-27_17:04 deploy-ps1`. That commit contains only the preceding cold-PAA documentation; the compiled implementation is77b50518. Both disk and both DayZ runs identify the same installed pair. EXE SHA256: `78936B72004922FC7921432F824E7E558DE94322E08EF7FBDB724E0DD4626778`. DLL SHA256: `842101F91C234B24E7CD30E51E41CB6359D9E0A9D08D7629F200EEE6C2368EE8`.

`disk-model-row-on-20260927-20260927-170511` and OFF170654 passed with normal shutdowns17:05:24.767 and17:07:07.326. ON selected MODEL, SECTION and LOD reuse together. It recorded one MODEL activation and eight partial-write notices, with no cap marker: seven single-row16-byte writes, then first row3/count4/table7/64 bytes. SECTION and LOD each recorded one activation, three reuse commits and zero disable events. OFF selected none and emitted no activation, partial-write or capped notice in those domains. Both completed the original disk paging/cancel/refill fixture with14 historical handles absent, zero live jobs/reserved debt, fineGeneration2 and unchanged helpers. These are controlled record-lifetime and storage-write results, not whole ownership, GPU freeing or an isolated timing comparison between the three features.

### Same-binary DayZ screening

The two DayZ arms verified identical installed pair, source HEAD, runner SHA and route protocol. Their recorded environment differs only in MODEL0/1 and the private profile path. SECTION/LOD/cold-PAA/warm jobs are disabled in both. TerrainPageTimings and copied current-render CPU diagnostics are enabled, WarmProfile is OFF, and fixed detail/admission settings are shared. They are instrumented renderer screens, not untouched normal settings or a vanilla comparison.

| Arm | Frames | Mean ms | Usual median ms | Nearest p95 ms | Nearest p99 ms | Maximum ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| OFF171306 | 788 | 44.1476167513 | 23.450 | 137.387 | 443.227 | 1019.220 |
| MODEL-only ON171446 | 825 | 43.4457200000 | 24.465 | 126.106 | 335.488 | 1029.541 |

Both passed, dropped0 recorded frames and shut down normally. Values are calculated from the raw route-frame-ms arrays, sorted independently for each arm. The usual median averages the two middle observations for an even count; nearest-rank quantiles use index `ceil(n*p)-1`. This intentionally does not reuse the older floor-index percentile summary.

ON has a worse median and maximum despite lower mean/p95/p99. Transient coverage differs: first sampled pose resident17,279 ON versus16,248 OFF, with other early poses also different. Returned resident/desired19,923, required0 and pendingfalse do not certify equal detailed visual work. Returned allocated counts are:

| Returned allocation | OFF | MODEL-only ON |
| --- | ---: | ---: |
| Geometry live bytes | 717,813,696 | 710,379,372 |
| Geometry arena capacity bytes | 1,275,068,416 | 1,275,068,416 |
| Retired geometry bytes | 0 | 0 |
| Object texture bytes | 2,778,451,656 | 2,749,355,764 |
| Object texture count | 2,525 | 2,489 |
| Backend allocation bytes | 5,331,048,848 | 5,301,865,744 |

ON records one activation,16 actual partial-write notices and one cap marker; OFF records none. ON's first notice is13 rows/208 bytes within14 table rows, and its last logged notice is2 rows/32 bytes within42 rows. These startup-biased capped observations cannot quantify all route copying or upload throughput. The single ordered OFF/ON pair has different cache and asynchronous allocation/admission work. It does not establish causal FPS benefit, absence of performance regression, default promotion or whole-world visual acceptance. Both routes still contain large frame hitches.

Root inspected both returned-settled images at the same point of interest: buildings, bus and trees remain visible, with no obvious new missing geometry locally. This is not pixel equivalence, all-pass validation or whole-world coverage proof. Future controlled-original-MLOD export work is not part of this installed checkpoint. Earlier ResourceGrowth, useful-upload failures and all other open streaming gates remain in the authoritative history.

### Actual artifact hashes

The comparison JSON records the exact raw-frame method, environment differences and returned-work counts. Run provenance carries installed pair, source/runner identity and flags. The following files were read after completion; hashes do not imply a stronger scope than the evidence above.

| Artifact | SHA256 |
| --- | --- |
| build/build-model-row-upload-20260927.log | a79865fb54807ee182eacdc6f985c9811dd18ba5989c034eea9fc908d7082ae1 |
| build/test-model-row-upload-20260927.log | 8f922563a505de347a49913c003576bb79befbbcaa0a5cbffb368e0a5e072803 |
| build/model-row-comparison-20260927.json | 0889ada05c6ffe07aa29cb2e394512c7e5fadf6fc576c2fd0e9eb1cb6c8e200f |
| engine/WgpuRenderer/rust/src/gfx3d/cull.rs | ea8517a6cef474f2922a3eb9f8f530a7ee24c3e5a0263f0012a492290a0347f4 |
| engine/WgpuRenderer/rust/src/gfx3d/mod.rs | c3636f3aad269770b12deb72668022b4f5f1c674136a031f4925f40a2b4b3fb8 |
| scripts/Test-GeometryPageFixture.ps1 | 0aac11092bd9704a02f94ce011053bd93fcdeb7b003a2a6b949b64ce9b58bfb6 |
| scripts/Test-DayZStreamingLatency.ps1 | 144beff6eac98cd4acb7c49ce1b3a688972a8475c811910dc6a67b7206e949f4 |
| build/stream-residency/disk-model-row-on-20260927-20260927-170511/result.json | 81869e704420d9e71d081f45075854dabdcb00aa137884ff6eb25d2a34e19fd8 |
| build/stream-residency/disk-model-row-on-20260927-20260927-170511/provenance.json | 7b583f7c5b625848c421f9dd0eece2ac48f60e9a79e7e61e200873461a9ab7dd |
| build/stream-residency/disk-model-row-on-20260927-20260927-170511/engine.log | 3094a1fc169590b9e70f9d28336e60c0641f8273495ba08e237296086c551d0f |
| build/stream-residency/disk-model-row-on-20260927-20260927-170511/harness.jsonl | e0e3382d8fbe27661d786760e83bb396b421cd0adb32cdbde5997d7fcdd9d438 |
| build/stream-residency/disk-model-row-off-20260927-20260927-170654/result.json | 26025d6cd54db600e9677f8e7a2e78e16989912fc6d9d6779a2d14069027c80d |
| build/stream-residency/disk-model-row-off-20260927-20260927-170654/provenance.json | b89f74086b099931ff845439a66562ac6b500091ee38b349ea7b8aac8af11f74 |
| build/stream-residency/disk-model-row-off-20260927-20260927-170654/engine.log | d49399f512609e7cd2190de7099af32751d64c90e9f61b1158af649da4189151 |
| build/stream-residency/disk-model-row-off-20260927-20260927-170654/harness.jsonl | 0eae91a8ec209490a0440c6fb99d518cff70159c8349dd70b30a5d6c08733493 |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/result.json | fb66eeb715a0162b451041a4188ec49275dc49fe65e529d1b844f6be1a65aa87 |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/provenance.json | 722591fd8b51994eeb40614295b77b5169814cda65c9a547455baa31887a0330 |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/engine.log | 8635cef18e909562dffbcad525958b9f22cd2157b51f1f727b7007ad1dbd5418 |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/harness.jsonl | d7fb91031bd3447831f74b9dec48cd43a5050aadfb89a34bd1269b842ca2adeb |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/route-frame-ms.json | 234220b4072f2ed14e1debdf33fcc86d900402a67bde8a0364071a92533edcba |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/samples.jsonl | 68f34384366c68a6d759621d8514c3f3f10b48b36c72bb2352465ee3faa285d6 |
| build/stream-residency/model-row-default-off-20260927-disabled-20260927-171306/returned-settled.png | e9c213bbe869daca049576ae89518edfb8b25bdec9dedcbe078064d5920b23ac |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/result.json | 6dba049579c17ae01b8ba40427821d3c5c158137fc3de21da7e756b7c927cb36 |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/provenance.json | f8a8d7106792b9a632052814cf3f9fe03bd565e72f200b7eef30a5b62ba7f8e5 |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/engine.log | 6395b5d58cd98c1b0f72bcf9079763e45fcd25a6d691d7c77f18714448bfa85b |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/harness.jsonl | 5ac5c7a60ff771b39b45dea85566dfce3462042ecec01df186204244963c366e |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/route-frame-ms.json | dc8ba4a94b47bbf2333f48ac32c3cd9dd9f41c9e9508d313da9db340f8f7b101 |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/samples.jsonl | 24a2dc60a1b6aa2f96987b4c63e4615e69ad756b5a72e5fb94ce4a8ebbfb519e |
| build/stream-residency/model-row-dayz-on-20260927-disabled-20260927-171446/returned-settled.png | 232aaab3c63dd0f80f2f159a926cdb2520cd70a168425e6ec26e7139c30551c3 |
| build/runtime-disk-model-row-on-20260927.log | 6c528f73cf8c30bfc2c2e5d6dd91ac3999d2df07e79f2f23c2927336306a2443 |
| build/runtime-disk-model-row-off-20260927.log | 46d2a92d98d4522cb13bc76c9acc2a9dd47c1114324de247326931f3928a8855 |
| build/runtime-model-row-default-off-20260927.log | cd717e2159e0efc549a327fd53b7e586d8ad8bf69ea5a5fd64e193924158ee3d |
| build/runtime-model-row-dayz-on-20260927.log | 6fa4d0cdef270aeb70ce4f9ab548f8437304bcfa49fe8b1823cd2e81fdbdf069 |
