# Bounded parked geometry attribution

The optional `662130e3` report now has actual original DayZ evidence for a bounded selected set of earlier zero-reference retained models. It distinguishes CPU-borrowed, unborrowed, outside-linked, absent-retired and uncertain allocation records at one drained renderer-owner cut. The result identifies a small potential policy target; it does not certify exclusive ownership, safe eviction, physical reclamation or performance benefit. The report remains off by default.

## Source and validation contract

Exact startup `WGR_GEOMETRY_PARKED_REPORT=1` additionally requires the existing owner ledger. The producer request captures at most256 parked model records and2,048 metadata visits using existing model/ref/mesh maps. It carries copied monotonic producer IDs rather than Shape pointers. It describes zero-reference selection at the earlier request, not a assertion that a model remains parked later.

The existing `SnapshotHandles` performs one mailbox drain, then copies records, aggregate owner facts and indexed selected model/LOD/allocation tuples from the same maps. Tuple collection stops at2,048 model/LOD/link visits; it does not scan the entire model registry. The existing single Rust facts call supplies full-generation mesh handles and actual span bytes. Truncation, incomplete ledger, missing links/facts, stale epoch, malformed values and arithmetic overflow never produce reassuring partial classifications. Outside-selected-model links take precedence over CPU borrowing, so classes and known byte totals are disjoint. A known byte count excludes missing or uninspected facts. Diagnostic uncertainty does not clear or overwrite sticky owner-ledger errors.

Default requests allocate no parked capture or selected-link vector, add no indexes or frame scans, and retain original lifecycle/rendering. Fixed report fields, optional request pointer and empty-vector metadata are additional footprint; this is not a literal zero-cost claim. Reported2,020 request bytes and3,376 tuple-capacity bytes count only known object/vector capacities, excluding allocator overhead, transient maps/joins, native handles and RSS.

All-five build252 passed. Focused `[parked-geometry-report],[geometry-owner-ledger]` checks passed9,892 assertions/24 cases without skips, including disjoint/shared-owner partitions, same-drain detach, stale/missing/duplicate/overflow refusal, bounded tuple prefixes, and unchanged lifecycle completeness on diagnostic truncation. These are source/unit correctness checks, not physical reclaim or all-pass drawing tests.

## Actual three-return evidence

`build/stream-residency/dayz-parked-candidates-20260927-20260927-201145` passed/rootexit0 with normal shutdown complete20:14:30.262. Result status is `Converged`, with convergenceMeasured/lateQuiet/retiredClear and genuine captured candidate usage true. The original strict ResourceGrowth gate is unchanged: return2→return3 geometry live729,236,116→729,519,052 grew282,936 bytes within8MiB; capacity stayed1,275,068,416 and retired0. Texture bytes2,767,529,972→2,759,316,628 fell8,213,344, while count2,495→2,517 grew22 within the allowed25. Backend allocation5,324,550,992→5,317,181,328 fell7,369,664. This narrow repeated-return pass does not erase the earlier +15,338,464 ResourceGrowth failure or establish global memory convergence.

| Cut | Captured models | Unique allocations | Known selected bytes | CPU-borrowed allocations/bytes | Unborrowed allocations/bytes |
|---|---:|---:|---:|---:|---:|
| Request1, quiet return2 |46|144|36,975,596|71 /18,532,708|73 /18,442,888|
| Request2, quiet return3 |54|164|41,568,568|71 /18,532,708|93 /23,035,860|
| Request3 final and request4 bounded |54|164|41,568,568|71 /18,532,708|93 /23,035,860|

All four joins/captures were complete, with outside-linked/Unknown/absent-retired counts0 and no truncation. Metadata visits190→218 and link visits334→382 stayed within their independent caps. Every cut satisfies count/known-byte conservation. Request4's bounded ordinary allocation report does not truncate this independently bounded parked selection. Final return3 also reports persistentAttributionComplete=true/reasons0 and rendererMeshFactsComplete=true:4,122 live records, complete record closure, valid residual and unattributed0. Cumulative metadata pruning reports6,742 allocation records and871 borrower records; these are diagnostic-history removals, not physical GPU release.

CPU-borrowed bytes are44.58% of the final selected parked set. This percentage is not the earlier whole-CPU sharing figure and does not describe every world allocation. The23,035,860 unborrowed bytes are about3.16% of the final live pool, only a candidate category. Absence of recorded CPU borrowers/outside selected model links does not prove no cached view, uploader, command, device or other owner exists. `ownershipCoverageComplete=false` and `persistentReclaimableBytes=0` remain unchanged.

## OFF control and installed provenance

`build/stream-residency/dayz-parked-report-off-20260927-20260927-201522` passed with normal shutdown complete20:16:09.073, status `AllocationDiagnosticOnly`, convergenceMeasured=false and zero traversal cycles. Parked enabled/captureComplete/joinComplete werefalse; models, visits, tuple capacities, candidates and bytes werezero. The normal owner/allocation report still operates. This proves optional gating and absence of this capture work, not an equivalent three-cycle performance comparison or a default-FPS verdict.

Both completed processes used clean installed `662130e3`20:11: EXE `B2D1D52324C8B5ABAE59270B2E87B0FA8FF574377EFBC131AF1B11E9ABE50376`, DLL `2F7267F21CC7BC655AE90A5506CD59A68EEBFC3C73F5A2B62408AE23F70E6FC5`. Rust FFI/ABI is unchanged. Exact checkpoint source snapshots are retained under `build/parked-geometry-source-662130e3`. Root inspected both returned images, with the same local bus/kiosk/building/tree appearance and no obvious new missing geometry; this is not pixel identity, all-map parity, all-pass quality or performance acceptance.

## Decision and remaining gates

Keep the diagnostic OFF and the entire plan OPEN, `defaultReady=false`, `performanceAccepted=false`. No hard byte-pressure policy is chosen. Before production eviction, measure net rebuild/refill cost and certify remaining lifetime/command ownership and demand across passes. A selected logical category and repeated quiet return are neither physical free nor proof of useful page/mesh eviction. No new GPU paging or pressure acceptance is granted. Prior resource-growth, startup/route classification failures and performance limitations remain historical evidence.

## Completed artifact manifest

| Artifact | Bytes | SHA256 |
|---|---:|---|
| build/build-parked-geometry-report-20260927.log | 127991 | `E1AAB0CB42A4A462ED9DA8F5162AF2ACBB9D91F21B3ADE7140EE7CBF42DC4D48` |
| build/test-parked-geometry-report-20260927.log | 135 | `169E6D0905EEE8A191A330FDE2E9F263FC3365061E4A796CB73422BF6E1A97BD` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/result.json | 612010 | `E4FE9B1BE50A2A5C9E7A04B8BC02BA47DD560DA6307D55BC63C905582FAD5587` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/provenance.json | 2440 | `64BFB3D228065A598207AAB2028BFEC8F3DEB2B9CE18B5FF1BF5205DA9C3202E` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/engine.log | 3885506 | `184C6163743EE21F8B999934E0326E8724025FB20B104D348704AE56F086B0E7` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/samples.jsonl | 156670 | `4294F6620E6F251144CD586A087FCF930C2C32D3B630E54859ED67730C10FFF5` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/parked-candidate-proof.json | 3861 | `28DC385431AD0D712AC0FE347FB459E20B33FC817CD374323670525536C6DD4E` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/geometry-allocations.json | 202182 | `A958131DE61F2D07D7F52920A2D428CBB31BE4947388DBA64B0A3482F505A4D2` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/geometry-allocations-bounded.json | 7279 | `DB46D30B913834F6446B293A16D053FABA57941ECB82C94CAF657997DB2F19C0` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/geometry-allocations-return-2.json | 203580 | `416CE773C3F30BC40E0B90F103D2DC01AB197C1DD883E1F16408D0F3F18389B6` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/geometry-allocations-return-3.json | 202182 | `543C04873116F406D565340020ED13DA8CCD2462CFF871FA3AAD6E4FC8F015E1` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/geometry-allocations-return-comparison.json | 344511 | `301CC4875F5B4BE6C8266FE33CBFA15215F32E9F6FD91E7CD0D26414AF126AAE` |
| build/stream-residency/dayz-parked-candidates-20260927-20260927-201145/returned-settled.png | 2154067 | `0E7D168B2D367592F7128FA946320DBDDC8A061FC772877EF504FC8F5D65715F` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/result.json | 225046 | `71A7416BF518EB61A08ECF019D3A8695CAD46885FCFE3646CA35FB3703A43C70` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/provenance.json | 2442 | `C7098AEF5EDB3AD109885F39BB3B6021CEF58C25117AC4C9E7EF44FF53FE5464` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/engine.log | 2167229 | `300D00F5E7BEA6CC04C3332D09F0784B4E05B7C3DE70DE4C12816A8CD2CBC062` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/samples.jsonl | 38627 | `E4DBA936AD791CDE9F8AA661E9CBBF2B8F804E425368B8534623FCD9E1EA3AF7` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/parked-candidate-proof.json | 264 | `C03F8C51D8AB8AE8F171E45D07C468E064A4211E4DA48D04972BC395E2D42103` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/geometry-allocations.json | 203684 | `165F31CD3EECBC2E655425C53646ABEE3AC7674EC6EDF815A9743364C4BD55A3` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/geometry-allocations-bounded.json | 7246 | `CC42B0E47EBC5B68C9DB174A0FC7BB198074589BDCE060CC6F77C1E565F16805` |
| build/stream-residency/dayz-parked-report-off-20260927-20260927-201522/returned-settled.png | 2165279 | `4C6F6B9C51945FFD1E4D4C40FF428D9E4FA1C1A3DB87EF4C71A112DAA9A8DC3E` |
| build/parked-geometry-source-662130e3/engine/WgpuRenderer/ParkedGeometryReport.hpp | 3551 | `09D547A88B86F49F3FE74A72BD6E788CD52354CFC22FBCBD6E85B296EBF5156A` |
| build/parked-geometry-source-662130e3/engine/WgpuRenderer/GeometryOwnerLedger.hpp | 26526 | `DE98CAD441E1860760BF4AA945F9AB573ABF0002ABFA8BCA43CB54F1D967AE94` |
| build/parked-geometry-source-662130e3/engine/WgpuRenderer/EngineWgpu.cpp | 881949 | `32EE4E0B15D99A1EA4219A6A535DE57EFCEF810DC0131D58EC9D2263607252F9` |
| build/parked-geometry-source-662130e3/engine/WgpuRenderer/EngineWgpu.hpp | 102447 | `50A1FD5D10308EAF3754B50040C27A8165628B8E03654F1B408BB719A431DB72` |
| build/parked-geometry-source-662130e3/engine/Poseidon/Graphics/Core/Engine.hpp | 208428 | `DCCDE89064213D327183790CA1474B66D9DE798C5A76FE6A422AA3C10626241E` |
| build/parked-geometry-source-662130e3/scripts/Test-GeometryResidencyConvergence.ps1 | 39588 | `A20204D4239D643D0CC817F8BF71BB3FB558BBA8220FB24448DBF7BD36B5F599` |
| build/parked-geometry-source-662130e3/tests/unit/engine/Poseidon/Graphics/test_parked_geometry_report.cpp | 7109 | `96B57F075744C84D8192E9CE1F8810E7B14E36794B1DFA3048617585BC40ECA0` |
