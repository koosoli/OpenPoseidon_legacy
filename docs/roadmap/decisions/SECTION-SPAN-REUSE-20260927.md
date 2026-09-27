# Optional SECTION span reuse — 27 September 2026

## 27 September 15:28 — optional SECTION metadata reuse and real disk faults

Source `7d4316e1` built all five targets in18 scheduled steps; Rust19 tests passed, none ignored and no adapter SKIP. The deliberate caught panic in the partial-clear transactional test is expected, not a runtime error. Clean installed stamp15:28 and actual matched pair are recorded in SECTION-SPAN-REUSE-20260927.md.

Actual disk Missing151737 and Corrupt151836 used script86615d26 with the earlier installed ddda pair; both passed/cleanExit. Real main-returned fallback remained one Present mesh with unchanged epoch/instance/helper, Failed typed Missing or Invalid, no coarse/fine payload, and jobs/reservations0. Abort then established oneAbsent/jobs0. The fault script preserves the fresh original producer file; corruption affects one byte in a private copy and missing uses a new private path. This proves controlled failure retention, not arbitrary-source freshness or pixels.

The SECTION experiment is exact startup `WGR_CULL_SECTION_REUSE=1`, default OFF. Source, resolved and material rows clear together before a retired span becomes reusable. Model births and LOD rows still append. ON-only owner8192/range4096 containers report327680 logical capacity bytes, not RSS. Cleanup is capped4096 rows/32 ranges per preparation. Partial-clear prefixes deliberately remain holes; only the completed suffix can be reused. Invalid ownership, parallel-table mismatch, mutable escape or capacity saturation sticky-disable reuse for the renderer epoch and preserve append fallback. No table shrinking, whole-ownership or physical GPU free is claimed. OFF keeps a null optional-pointer field/branches rather than zero footprint.

Actual DiskON152925 and OFF153025 passed/cleanExit/root0, each ending14Absent/jobs-reservations0. ON produced one activation, three actual postcommit reuse rows, zero disabled rows; OFF emitted no activation or commit rows. The GPU tests exercise actual compute table and material readback, source/resolved clearing, bootstrap and generational address parity, not all-pass pixels or visual equivalence.

A single same-build default-feature screen153111 passed/root0, Shutdown15:32:33.732,681 frames/drop0. It explicitly uses reuse0/water0, TerrainPageTimings1/copiedCPU1/WarmProfileOFF and other experimental feature controls, so it is not an uninstrumented normal-settings measurement. Raw milliseconds: mean52.6898649, usual median24.773, nearest-rank p95148.261/p99511.423, maximum1181.802. Quantiles use sorted index ceil(n*p)-1. Returned19923/required0/pendingfalse, geometry718731600/cap1275068416/retired0, textures2643280788 bytes/2390 records/backend5194496768. Paired token391's805.793ms surface API CPU scope is not shader/GPU/compositor cause or completion proof. Historical095 had different allocations/cache/async work; center and tail changes are mixed, with no causal no-regression or performance acceptance.

Whole streaming/StageG remain OPEN, defaultReady/performanceAccepted false. SECTION stays OFF. ResourceGrowth, stage-family upload NotExercised, complete ownership/pressure and broader gameplay gates remain. The next LOD reuse feasibility study has no implementation or runtime acceptance at this checkpoint.

| Artifact | SHA256 |
| --- | --- |
| build/build-section-span-reuse-20260927.log | 32b579949ee1511f58e20e38c0656c41c4f6deab767e5467b9b6117b281c97e7 |
| build/test-section-span-reuse-20260927.log | 5ab65e2c936a8b56ed44a288271e898f13ff6a65a3964ab1546f398da8097acd |
| engine/WgpuRenderer/rust/src/gfx3d/section_span_pool.rs | 05482b5f46838c537433e85f7b9ef5842fb2426b99ae4034f6e20cb6672d194f |
| build/stream-residency/disk-missing-20260927-20260927-151737/result.json | 791d5fb6c31b174bed38dd5f026ef46b6b120743bca68ced5ce0934acd078e2a |
| build/stream-residency/disk-missing-20260927-20260927-151737/provenance.json | f4f123df300f5ba4de9d8d1cf5a59fb9fdf3a0b3f49f884bff6928335c1aeee4 |
| build/stream-residency/disk-missing-20260927-20260927-151737/disk-fault-proof.json | a838a4699532c37c6acb85510526b5c6b9159b4fa244e36c4f77d3c3c27c5c08 |
| build/stream-residency/disk-missing-20260927-20260927-151737/engine.log | 8a760dacf59ae2c11eb16f9c704cc199a41c7603696c547c4013adc9955f081b |
| build/stream-residency/disk-missing-20260927-20260927-151737/harness.jsonl | a50b924e098e0dfb7809bd84a07b0dc5740036ce0e8351e9ad2d3501930dc42b |
| build/stream-residency/disk-missing-20260927-20260927-151737/fault-retained-fallback.png | fdea4edbece4774494c3110d5fece3462a8544ee878e1f8b71612e793954292e |
| build/stream-residency/disk-corrupt-20260927-20260927-151836/result.json | 3a93ba78e0643a4f6a16af50651ca5500fbe2102c6e0351819a982292d5e4e5e |
| build/stream-residency/disk-corrupt-20260927-20260927-151836/provenance.json | 0176d87b6e20f051897bf1849248f51505d2ae4a900f3863c0bb7d84a9b34620 |
| build/stream-residency/disk-corrupt-20260927-20260927-151836/disk-fault-proof.json | 3535020f10659689d04c7fc3492c47ff59cbc894194e1a0dada18daf338418e7 |
| build/stream-residency/disk-corrupt-20260927-20260927-151836/engine.log | 6cf1f2808fabcc412e64e7b3152a2e95f399877a14a4f3c75277275c1b20a547 |
| build/stream-residency/disk-corrupt-20260927-20260927-151836/harness.jsonl | 27e5871cbf9910d91e40a33427463ee0ea20733ceb4a675d593f12921fc66e6d |
| build/stream-residency/disk-corrupt-20260927-20260927-151836/fault-retained-fallback.png | 60687c72406baba41cb2a9a3f29f85a1bf07c84ebc631719549cf6a67761cfab |
| build/stream-residency/disk-section-reuse-on-20260927-20260927-152925/result.json | 77e54c1112b52d8283c4960aa352ab7eb81c0a59e79a5d3bcc1da0f120982a71 |
| build/stream-residency/disk-section-reuse-on-20260927-20260927-152925/provenance.json | a2c4310c6607e33415b1ba70af25ae3b12acc4e0dead3fdef17837b3d0a0fc26 |
| build/stream-residency/disk-section-reuse-on-20260927-20260927-152925/engine.log | 172e1dd178806e592827cfffd8af6df2234fe936a73660d25cec371f386b1a0e |
| build/stream-residency/disk-section-reuse-on-20260927-20260927-152925/harness.jsonl | 981836a62730b3e033807f87211551769a4eec7d78f5130459f2d5b037196252 |
| build/stream-residency/disk-section-reuse-off-20260927-20260927-153025/result.json | 947625d1a81a20feedf6af6d954ea48b72700948bfbf4383cc21ca1630ac0a1f |
| build/stream-residency/disk-section-reuse-off-20260927-20260927-153025/provenance.json | 5acdc993e738474d4adee9f107b96ca7458fc230ce879971e20c0936812ee267 |
| build/stream-residency/disk-section-reuse-off-20260927-20260927-153025/engine.log | 04b0efd8b70a2717e283f6deb1627ad6826d4f9c392b53c97063e59f8d18dedd |
| build/stream-residency/disk-section-reuse-off-20260927-20260927-153025/harness.jsonl | 866d13860bff881fba7e425d3c49b5f9adcf56c6816191905b9a786bbb09bc7a |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/result.json | d730870910d0ad34ab57ee0d9b767ed76da6d437611751c669416771d1e7c0ec |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/provenance.json | 96ce28cbf1d44b038a840ee1a4aabcb9686c63e70f4dfba923867bfa3ec33e9b |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/engine.log | b8a172b19091d64d6b33089773e9b49d84d8c96b0fec8f82bbe58bfff225d49c |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/harness.jsonl | ed641243cf71163c961d19461c2251bc5837d3b29dc00536d44708f7db356eee |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/route-frame-ms.json | d0417f433df65fdcb48bdcb408eccf208b13aa3a2d765aeb2a4466e4da65ed28 |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/render-call-cpu-proof.json | 4f4aa25447dd1b04d623173d225cd9e10d3d1ebe2f70f01d057657df021866c5 |
| build/stream-residency/section-reuse-default-screen-20260927-disabled-20260927-153111/returned-settled.png | f236024c3c070eaa4f960d15ca9d55490fcf088a48abc401f3e1823517695e26 |

Actual7d installed pair: EXE `17FD44063015CD778C5B59632690C7A6EBB9C4326C85820C14F7BE1E02A7BE37`, DLL `6B9E7B529AA1346F9E2D16C38B540424FA8031D098DE69DFD3775B28E4C25B7E`; ON/OFF fixture scriptSHA `7E9860E2B1C83FA0284E3ECEE70F5A217635100B3E109AF7816BBEE8F0191725`. Earlier fault runs retain their actual ddda pair and script866 hashes in provenance rather than being relabelled7d.
