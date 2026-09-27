# Loose-PAA worker single capture

The default active loose-texture worker now constructs one native stream and decodes BC or fallback RGBA from the same captured bytes. This removes the previous second open/full read after a non-BC result. Actual native-file checks preserve supported bytes; the original DayZ screen exercises the negative loose-miss stage and ordinary owner fallback. It does not prove positive retail loose decoding or an FPS benefit.

## Compatibility and scope

Source `f9e87d41` uses the corrected reviewed0C587 patch. Older static drafts were never executed; missing include, OFFS cursor and cap issues were caught and repaired before the actual build/tests. The single stream obtains length, reads completely, checks read state and probes EOF for growth. This is not an atomic disk snapshot, immutable-file/source-freshness proof, cancellation change, or syscall count. `openAttempts` describes actual C++ stream construction and counters are local to preparation.

Production explicitly passes `SIZE_MAX`, retaining the legacy default worker's source/BC eligibility instead of imposing a new cap or mip-count refusal. BC parsing and the global RGBA decoder are unchanged. The helper's default16MiB bounded mode is not used by production; it returns Unavailable on the current parser's byte-cap or32-level refusal before inflating an unused RGBA fallback. Those reason-string comparisons match the current parser but remain maintenance-sensitive if messages change. No completed production source/output-memory bound, allocator/RSS limit or I/O-time budget is claimed.

Before optional raw fallback, bounded cursor checks reject malformed known TAG sizes, high/signed OFFS, a shifted top-level source header, the exact unsupported legacy marker, and physically truncated declared payload. Failure skips optional worker preparation and leaves the complete ordinary owner path. Successful BC parsing keeps its existing legacy-marker support. There is no new cache, feature flag, material/classification policy, or source-family whitelist.

All-five build22 passed. Actual native-file tests passed81 assertions/five cases without skips, covering accepted BC and raw RGBA byte parity with existing decoders, actual one-stream capture metadata, legacy mip eligibility, and malformed/missing/bounded source refusal. The successful tests establish these controlled files, not every caller buffer, archive/PBO file, retail texture, or full-world visual compatibility.

## Actual installed DayZ screen

`build/stream-residency/dayz-loose-single-capture-20260927-disabled-20260927-203013` passed/rootexit0 with791 frames/drop0 and normal shutdown complete20:31:24.806. Returned residency and desired count were19,923, required0, pendingfalse, coldAlphaPeeks0 and no cold upload witness. Geometry live702,520,576/capacity1,275,068,416/retired0; textures2,656,081,556 across2,394 records; backend allocation5,207,312,384.

The final cumulative worker row at20:31:09.014 reports0 chains/0MB/173ms, unreadable8,634, skipped3,137 and consumed0/2,764. This original DayZ virtual-source workload actually exercises the loose-file miss branch: it does not provide a successful readable BC/RGBA retail preparation/upload witness. Those accepted-file semantics are covered by the native-file unit checks. The old same-control dbe195215 row reports unreadable8,654/283ms, skipped3,139/consumed0/2,769. These are cumulative worker CPU counters, not syscall counts, unique assets, per-route wall time or GPU clocks. Their work differs, so the lower counter does not establish causal worker speedup.

| Screen | Frames | Mean ms | Usual median ms | Nearest-rank p95 ms | p99 ms | Max ms |
|---|---:|---:|---:|---:|---:|---:|
| New f9 screen | 791 | 43.551783818 | 23.118 | 135.818 | 438.351 | 1036.59 |
| Earlier dbe195215 OFF screen | 784 | 44.682094388 | 24.189 | 141.212 | 350.133 | 875.841 |

Percentiles use sorted `ceil(n*p)-1`; usual median averages the middle pair for even counts. Both screens use ModeDisabled/TerrainPageTimings and controlled model-PBO/DDC0/budget settings, so this is an instrumented default-path screen, not every normal default. The new screen has lower mean/median/p95 but worse p99 and maximum. Ordered cache, source, asynchronous admission and unequal allocation/preparation work prohibit causal FPS/no-regression acceptance. Root's settled bus/kiosk/building/tree view showed no obvious new missing local geometry; this is not all-world/pixel/material/all-pass proof.

Clean installed `f9e87d41`20:30 used EXE `2C78A700B99EF70E7B8FA76CCA79A1052763E1788FECFCC83859673B7C927235`, DLL `BED1CB85C05E652AC9AB5226039A42560DFFDB1E3C417EEE5A180026D9593F2E`. Exact gitshow source/runner snapshots are retained under `build/loose-paa-source-f9e87d41`; later work is excluded.

## Decision

Keep the compatibility improvement while retaining the original owner fallback and unbounded production policy. No further smoke is required for this narrow phase. The next offline geometry-certificate prerequisite is unbuilt/pending and excluded. Production loose memory bounds and broad readable-asset coverage remain OPEN, alongside the whole plan; `defaultReady=false` and `performanceAccepted=false`.

## Completed artifact manifest

| Artifact | Bytes | SHA256 |
|---|---:|---|
| build/build-loose-paa-single-capture-20260927.log | 27656 | `F33C3C5B901231A055961138026AF807400FA72700B30781E2F1AC7376442673` |
| build/test-loose-paa-single-capture-20260927.log | 109 | `0836EADA5B71EC5316016F15F47EFB7C2997E38D5C657E1E8861915726DFCF24` |
| build/stream-residency/dayz-loose-single-capture-20260927-disabled-20260927-203013/result.json | 80425 | `540A96BC8342FF5758FB15411E8DFBB64C943D7419938F88056583CC95D7E7E1` |
| build/stream-residency/dayz-loose-single-capture-20260927-disabled-20260927-203013/provenance.json | 3549 | `1B6CECC4A5D7142DA3001E6CDA043E73E8D8D699D08827C8F6698A7782315714` |
| build/stream-residency/dayz-loose-single-capture-20260927-disabled-20260927-203013/engine.log | 3204248 | `69DEBC8A704D940EE5429B5561D3054E49AE0CF9F955985A65A2A2C8C5ED43A9` |
| build/stream-residency/dayz-loose-single-capture-20260927-disabled-20260927-203013/route-frame-ms.json | 8671 | `04EAE7A9502B7C385FCCC8861D84C138BC46B14CD2C4AE561050A2663E272BC7` |
| build/stream-residency/dayz-loose-single-capture-20260927-disabled-20260927-203013/returned-settled.png | 2163003 | `673652FA66D73A3695D84EBB50B81F812CAD2775813187527D30B5E37247658A` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/result.json | 78331 | `E1EE5CDC9A24DA9A1DE218466FC12A36CAE8043AE90B6AE69374EF68D0439A9D` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/provenance.json | 3510 | `1A4F42E4262339D1754815CD8759DB7F6389C10B7FB905922D10AB11AEF82B18` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/engine.log | 3209396 | `8FE0266551FE27575B782EBD0140C4E82D8698B697D55E663ADC8C20A8FC4E0A` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/route-frame-ms.json | 8586 | `B0DF783F494EFBA00A898CA014470B81FAF7667497D060A3BC3957C968AA838D` |
| build/stream-residency/dayz-cold-alpha-off-control-20260927-disabled-20260927-195215/returned-settled.png | 2163016 | `A83F6BA4D76B7E0079880A6A5FA2E1E8C046FB84A9D7DBD8694E13B43878048F` |
| build/loose-paa-source-f9e87d41/engine/Poseidon/World/Terrain/LoosePaaPreparation.hpp | 5040 | `E5E18C2D6E17F5E3B5AF0BEF7495C0EA38CD9E4CB49CB1D88D7F25FF2358241A` |
| build/loose-paa-source-f9e87d41/engine/Poseidon/World/Terrain/ObjectStreamPrepare.cpp | 97982 | `CA1157F416EA153FA09324374D3EF36B7E62414C4031928A4A45046314D96124` |
| build/loose-paa-source-f9e87d41/tests/unit/engine/Poseidon/World/Model/test_loose_paa_preparation.cpp | 8550 | `AE4EED1788D3C1B8D44D79BEBD061E269CA609AF961B368AF178E48B5D37E1BE` |
| build/loose-paa-source-f9e87d41/scripts/Test-DayZStreamingLatency.ps1 | 43230 | `7F7152BDB4804851B72DAC8FC54218291DEDD1A6D6BBFE47863CDDD921B8B8FA` |
