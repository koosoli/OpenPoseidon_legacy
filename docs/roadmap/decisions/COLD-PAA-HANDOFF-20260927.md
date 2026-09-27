# Optional operation-local cold PAA handoff

## Verified checkpoint: 27 September, source 5b57f097

The controlled ON run demonstrated one actual prepared primary-texture upload during normal object admission. The OFF run loaded the same original two-box fixture using ordinary reads. Both finished normally with two visually resident objects and one normal vertex buffer per object. This closes the narrow useful-upload correctness gate. It does not establish a performance improvement or whole-model texture readiness. `WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF=1` remains an exact startup opt-in; its default is OFF. The streaming plan remains OPEN, with `performanceAccepted` and `defaultReady` false.

### Ownership and limits

The joined owner snapshots the existing initialized PAA header and retained physical archive-member witness. For a cold conversion it visits at most64 LOD entries within a shared256-visit bound, selects at most one eligible nonresident visual primary texture, and requires no more than two configured workers. The snapshot contains immutable numeric metadata, a normalized key and an archive read lease; workers receive no live Texture, file-server MRU or config pointer.

The converted result owns the decoded block chain. It remains available until the admission that takes that result, independently of global prepared-store TTL or an uploaded-key exclusion. A lexical owner scope permits one claim after current mounted member, exact selected-header layout and publication-token checks. A mismatch or optional allocation/read/refusal leaves ordinary conversion and upload fallback available. Reset and DropStale invalidate publication and retire optional payloads outside the preparer lock without allocating a retirement list; real active-worker debt is retained until that worker exits. One indivisible LZO call cannot be interrupted.

Source bytes and retained decoded chain capacity are each limited to16MiB; the snapshot admits up to seven selected mip levels. Optional owned ready chains have a128MiB cap and are charged within normal ready payload accounting. The existing64MiB scheduling scratch estimate is not an allocator, kernel-handle, peak-RSS or hard execution-time bound. Default OFF does not start this source capture or worker handoff, but added pointer fields, null branches and TLS plumbing have a nonzero footprint.

The parsed original MLOD format preserves quads before adaptation. The narrow dependency gate now accepts vertices plus triangles **or quads**. It does not alter global `Mesh::HasGeometry`, eligibility of arbitrary models, class admission or gameplay query readiness. The focused unit uses a real parsed MLOD quad and its actual added material index, rather than assuming the parser's null material occupies the desired texture slot.

### Build and failed attempts

The source series is fadc37fc (owned handoff),1ee51084 (typed mip byte snapshot),cc5e51bd (test capability namespace),27f06b28 (actual Init source retention),34fcbadc (valid legacy mip fixtures),e74970c8 (ordinary-reader destination parity), and5b57f097 (parsed-quad dependency eligibility). The initial build failed at narrowing int to size_t; the repaired build then failed at an incorrect test namespace. Those logs remain. Subsequent supported builds passed; e749 built all five targets in19 steps and5b57 in202 steps.

Two initial ON unit runs each failed three cases, six of nine assertions passing, because the authored first4×4 mip is not counted by the existing Init policy and no binding was retained. Switching the cold-only fixture to a real8×8 BC2 chain advanced to two passing cases but one failing ordinary-read comparison (107/108 assertions): its destination format remained the default ARGB1555. Setting the same block-preserving destination used by TextureWgpu::LoadHeaders yielded122 assertions in three passing cases. Final5b57 ON passed187 assertions in three cases with no skips, including real parsed quad input, independent ordinary-reader byte parity, one-shot/source-affinity refusal, cancellation debt and optional-ready refusal. OFF source contracts passed208 assertions in nine cases without skips. These are focused tests, not the entire test suite.

Actual e749 run `cold-paa-owned-on-20260927-20260927-163324-71bc60` failed the strict useful-upload gate: **NotExercised**, with no upload row. It still admitted both boxes and shut down normally at16:33:37.267 without forced termination. Its failed result is retained as failure.json; no successful result.json is invented. The real MLOD boxes contained quads only, while the former cold dependency check required triangles. The55,072-byte peak conversion reservation supported the absence of the additional cold read; it is not a memory-performance result.

### Actual ON/OFF evidence

Runs `cold-paa-quad-on-20260927-20260927-165226-3e2f46` and `cold-paa-quad-off-20260927-20260927-165339-100156` used the same installed clean5b57f097 pair, script and generator hashes, under the game lock. Their normal shutdowns were16:52:40.595 and16:53:50.654. Both started with identities1001/1002 absent and camera resident/desired0; after camera demand both had resident/desired2, required0, pendingfalse, and one normal vertex buffer each. Simulation remained disabled. Warm workers, global PBO texture preparation, water and other experimental gates were disabled.

The original fixture has two loose MLOD model paths and two placements sharing one unique archived BC3 PAA. Its source chain is64/32/16/8/4, totaling5,456 decoded bytes. Existing TextureSourcePac::Init breaks on the4×4 mip before counting it, so TextureWgpu::LoadHeaders selects four levels and the unbiased upload totals5,440 bytes. The strict gate checks this selected byte count, not all worker-decoded bytes. ON recorded exactly one unique-source row with `sourceValidated=true blockUploadSucceeded=true operationLocal=true`. OFF recorded no cold-handoff upload rows. Neither a prepared payload nor an archive counter alone can satisfy that ON gate.

ON archive-binding cuts changed118→119 live bindings and54,192→54,776 known C++ metadata bytes;119 wrapped reads and119 Init retains were observed after admission, with all three refusal counters0. Caps remain256 bindings/2MiB. OFF reported binding fields0. These are whole-process binding cuts, not per-fixture read throughput, all-source ownership or RSS. ON peak conversion reservation was134,291,264 bytes versus OFF55,072; both ended with parse/conversion/ready payload0. The greater optional reservation is retained as a cost, not presented as a speedup.

Root compared the two final captures: both show opaque red boxes without an obvious local appearance difference. This is a still-image judgement at the private fixture, not pixel equivalence, all-view rendering, retail/native asset compatibility or whole-map coverage. The upload witness records a successful renderer producer operation, not queue completion, physical allocation freeing or GPU timing. No frame-percentile/FPS benchmark was taken. Material-stage readiness, all primary textures, general asset admission, performance acceptance and default promotion remain open.

Clean installed stamp: `codex/streaming-residency 5b57f097 2026-09-27_16:52 deploy-ps1`. EXE SHA256 `BC00E5C1075F39E82210795C9831B9BD81373EEBF602F021A29267C975076B8C`; DLL SHA256 `F97C994BD5DB5C88BCDA87C0E67B27B43ECD96A20515B08A5A0B59B61F7F83BA`. Both run provenances identify this pair and verify hashes remained unchanged. Historical failed e749 used its own pair, recorded in its provenance; it is not relabelled as5b57.

### Artifact inventory

Each producer.json records original generated model, world and archive byte counts/hashes, its helper hashes and the unique member name. The provenance files preserve exact flags, installed stamp, script/generator hashes and command arguments. The following hashes were independently read from the actual completed files; no running log is treated as final.

| Artifact | SHA256 |
| --- | --- |
| build/build-cold-paa-owned-handoff-20260927.log | d28683a9942250b32a9cea44dfa7ea5f07b1dc2f064190831b4cbc610dfe9e3d |
| build/build-cold-paa-owned-handoff-repaired-20260927.log | 50e9ab8aff3c5749e2b5e0eba637b399d43fdb2266e478f67b3bc6f55c353cb6 |
| build/build-cold-paa-owned-handoff-final-20260927.log | c7d956e404e1847fc6638927c69e8c662772724cb20fe54aeab6c8c9877a1466 |
| build/build-cold-paa-source-retention-20260927.log | fdddd081f90ca5f265288d538ca60c19cde956201b700363fe2c8e02a60be92e |
| build/build-cold-paa-valid-mips-20260927.log | 14992b8e8a5bbd5c9b9e002e4b16d36e0db1558c32eec742b42d27b8def332b6 |
| build/build-cold-paa-destination-parity-20260927.log | 1d9a5e07eeeb57864316b77d5a6da43a6f8242b76557ce8dc4cd5d5a121a7bcf |
| build/build-cold-paa-quad-eligibility-20260927.log | dea3d24f850aa5c64d8cad69304b723f31d3de90ba06e6ceb592e62abd01ec07 |
| build/test-cold-paa-owned-handoff-on-20260927.log | 1b745c57c88c2e933c5bca25ec49ddc944dca652605dedaaafa33204a866cc58 |
| build/test-cold-paa-source-retention-on-20260927.log | 140082bf38fa1ee18bdabb83ca83ed36a9ae5e854bb52f5ef1d59d1926dafe77 |
| build/test-cold-paa-valid-mips-on-20260927.log | f0e003232ebb9cf557c98710451bd8c73adb022cec06583a0a9a5c832c03ee79 |
| build/test-cold-paa-default-source-20260927.log | 7065e11a9a4f4f22a9d73b47c8ebb0ff7b022bed68c10ee5d61830d9e9642b0f |
| build/test-cold-paa-destination-parity-on-20260927.log | 2304f692fd905021edbf72ad21567348ce4cac5c1dbc4871327be2617da9488d |
| build/test-cold-paa-repaired-default-source-20260927.log | a249c0e7ad2d35c31bed805507135f15851cbdc36eb0ff5cd51a17715ca5a08f |
| build/test-cold-paa-quad-eligibility-on-20260927.log | c85eff6270164cfa9a211bde7accc9879f74b58d0271fa48f0ab052ef2a1c54c |
| build/runtime-cold-paa-owned-on-20260927.log | d81779ed9a3b3282c55c0ea17c78878df3d4cba678bdeaf64253558c4cbedfd7 |
| build/runtime-cold-paa-quad-on-20260927.log | 41083f13aeccaae56ab666278e54008a5907e8e3a68b529693ef01b9643d7e65 |
| build/runtime-cold-paa-quad-off-20260927.log | 82c55632b5357b93254f028d9ac7ec0be9d815116abbf83037d5d04c20ac7b61 |
| engine/Poseidon/Graphics/Textures/ColdPaaHandoff.hpp | 6a1091d8ab2a4269a9c2784e618e134b61c93613c6772e973db707acbba861ce |
| scripts/Test-ColdPaaHandoff.ps1 | 007832fde17fa7faf46e20dfe1dd5d22fad982bac01e6f58a33b954929cda816 |
| scripts/streaming/build_cold_paa_handoff_fixture.py | ba6abdb1b54245a1113b59d24bcf08038d2cbe4ec8afc8ce846fbd9ecf936207 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/result.json | 8eb33bd089dc5af036bd83a0cac1882b9b39fd0304433ee33854d50094d3fd21 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/provenance.json | 65f3fcd27af9d5ab41c6b9edc69177c414f3a6e982c3c0e7d521ba57cbc1a662 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/producer.json | c9dfac5d892256957425e2b0a8b3a0598ff1dbb32ac5f9339137a5ba5dca95d6 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/engine.log | 29dea2cab884e2f913269afd964c43b848ca742b630f7f67f4605c945d810a84 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/harness.jsonl | 84e147bc7aec4ade923671e1711e3dd241bd3bf1413eec76919aff26944e7367 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/fixture.png | 5d040b810e63a9b376cf2b37123545fa94b16a75a52aa5a521d242ec3c250ed5 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/archive-before.json | 7f5174b0b649c1d1d9291212f49831301c5e078e87028e1a4240d1e43bfe10ad |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/archive-after.json | ac4faa6d3d39980e3265cf5d31613f856767ff6cb58d8e62520dd8e60756d7d3 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/baseline.json | 373b9ad8309c3dc117e08e8729ead2cc8f2a95ff3e014a056d77f78e82c2bba2 |
| build/stream-residency/cold-paa-quad-on-20260927-20260927-165226-3e2f46/coverage.json | 4c8a7875f6ccfdaefefc9e5e4f53a57242e51acc1b46a82d79703ed9e1e20ff4 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/result.json | 75ccb033109785c8fd061a8994b037370701c717000a253970e60f92fa1b1ab0 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/provenance.json | 36cd3fbeb6424d5aeccc9829df4dfa75efda1adccd458523828c6a8bf5445408 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/producer.json | c5c695da8020c911ca7fb94b4fac6f24a469fa18c7b7610f44db5027d112e3e2 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/engine.log | 1beabb9b45d1779f744e738b3a9d864a49e354bbe918b3fc41445d7fac119ec0 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/harness.jsonl | 2b95fddeab390f49e4fdb5e543bd6dd79845ca92a3abbce5770a965e714c86e6 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/fixture.png | 4a3384f989c1f73c8a22fec52316fb5bdb71f82f5449a574d8995877be3021ee |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/archive-before.json | 8fe3a4332e40730fde97bc5694eb6e10c793d43d85584ed81365ab122d387677 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/archive-after.json | 8fe3a4332e40730fde97bc5694eb6e10c793d43d85584ed81365ab122d387677 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/baseline.json | 6b58a0e4d1dd1a4b1bbb8b9b7ea39f6570540f520014da25261eabcd7d4d74c8 |
| build/stream-residency/cold-paa-quad-off-20260927-20260927-165339-100156/coverage.json | 50b5e0067182c5baa43b72d6e5136c5edae3c9f1ed8b57115347e59cac3f939b |
| build/stream-residency/cold-paa-owned-on-20260927-20260927-163324-71bc60/failure.json | d310487716e01601c85e58a7ce1974c83ef684f3c0e515431806c8ee3415ad1e |
| build/stream-residency/cold-paa-owned-on-20260927-20260927-163324-71bc60/provenance.json | 4fcc16fe0cc834896ccf179118c4894971e4f200262071d3e9187c7269d79ea6 |
| build/stream-residency/cold-paa-owned-on-20260927-20260927-163324-71bc60/producer.json | 33ca63eef2110b891127053185097302f5836d6eb415795aff4b8d027b414974 |
| build/stream-residency/cold-paa-owned-on-20260927-20260927-163324-71bc60/engine.log | 86d13a7cb0afbaf0d7971d0f1a8f79d738484579fc610489f9b220fec41bb205 |
| build/stream-residency/cold-paa-owned-on-20260927-20260927-163324-71bc60/harness.jsonl | 132048b5954cf6607bce1962f6e7074e24e3383a2e0e75a19d253ef747b380e4 |
| build/stream-residency/cold-paa-owned-on-20260927-20260927-163324-71bc60/fixture.png | 7f28b2092fc80b243d2b84ba75391257328e910f70ea10597d476ac17bfb02e8 |
