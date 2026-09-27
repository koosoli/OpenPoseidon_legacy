# Tidewater W6g default, camera controls and strict experimental failures

The completed subset below supersedes the initial source-only pending state. Compiled and installed source is feef4140b0b57c75b7496525bc3621f377aad58a, supported Deploy.ps1 stamp2026-09-27_23:36. EXE SHA2562119C58BEE81719F27B08A9671432C0E3B74B255626C91E25B536EFB10C9BFCD; renderer DLL DC232FC79D22B48FB49AF4FEBA1579108138705A9B8C7D783F063BEFF87F61C9. Later diagnostic/test changes are not installed under this checkpoint.

## Source integration and automated checks

Exact requested source tip e10f5e746a565b88c74a3c65a38c1f0c5ab9730e was merged64fd11ba. Three missing commits are preserved: a28b3f03292f6bc6175073cf542917ab3f62cea3(W6e),09adb3ef1bc899457aed384ed3a23ecb5e11b1a8(W6f),e10f5e746a565b88c74a3c65a38c1f0c5ab9730e(W6g). W6e adds actual afloat surface checks, W6f per-hull local wave contacts, W6g canFloat-gated water path planning. These impose real car/hull CPU work; no actual BRDM/path/grounding/network or performance acceptance is established here.

Default adaptation e4d32224eb146159c2f51c0cbb5ee4261c8b2495 makes fresh/missing/unknown selectors choose Tidewater1. Saved explicit/UI0 and exact env0 preserve CurrentOP. Startup constructs only the chosen backend; unsupported storage falls back to legacy. A saved0 without environment is not yet known by Rust at construction and may initially allocate Tidewater before producer profile0 switches. ABI18/WaterParams1216 and camera/CPU72/pass592 layouts stay matched; no zero-overhead claim.

Combined all-five build completed scheduled253. Fresh C++ projected/camera854 assertions7 cases, Water/ship4119/13, weak ON62/2 and weak OFF18/1 all PASS without skips. Rust water_backend4 and ABI21 PASS/0ignored, including the new startup selector. Counts identify exact executed filters, not all vehicle/runtime behaviours. Earlier cdef Harness/slice compile failures remain historical in the preceding camera checkpoint.

## Actual requested default and explicit rollback

build/tidewater-w6g-default-Default-20260927-233617-7d913a records backend environment absent/null and initial Tidewater Native23:36:22.950. Explicit0 control build/tidewater-w6g-default-0-20260927-233654-97d78e records environment0 and initial CurrentOP23:36:57.724. Both passed=true, exit0, normalShutdown=true, forcedTermination=false and unchanged installed pair. Shutdown completes23:36:34.985 and23:37:08.282. Root saw local coast/LST/water present in the default capture. This establishes startup and scoped visual presence, not water fidelity, underwater, wreck, amphibious paths, network, all-view or FPS acceptance.

## Actual camera controls

Live scaled repair passed build/stream-residency/camera-tuple-scaled-live-20260927-20260927-233803-b3b86a. Actual v1/192 render960x540/output1280x720/status5/rawvalidfalse/idealfalse occurs at setup generation93, poses103/114/125 and refill194; final14 Absent/zero jobs/debt. Normal shutdown/exit0/no force. The earlier failed startup-only scaled arm is retained; this passing live pin does not rewrite it.

OFF manual control build/stream-residency/camera-tuple-off-control-live-20260927-20260927-234152-8e599b passes the existing full cycle with tuple enabledfalse/rawvalidfalse/idealfalse, final14 Absent/jobs0/debt0 and clean exit0. Earlier cdef native positive/move/refusal/refill evidence remains historical and unchanged. These are copied frame and logical record/lifecycle proofs, not GPU completion, pixel equivalence or physical memory reclaim.

## Projected preparation: strict failure before activation

build/stream-residency/projected-prefetch-native-20260927-20260927-233900-398b07 contains failure.json, no result.json: passed=false, exit0/normalShutdown=true/forcedfalse, reason far pose lacks low ideal pair discrepancy. Actual conservative upper4.588616 at the10km pose exceeds unchanged leave rail1, so followProjected never activates. No request/preparation/cancellation runtime proof is claimed.

The source policy remains preparation-only, coarse selection unchanged, joined completed requests consumed once, fresh authenticated current camera generation/time required, eight lifetime attempts, Stop/restart fenced, worker cancellation debt retained until exit. Source-reviewed implementation is not a substitute for runtime exercise. Proposed100km pose was rejected during source review because camera-relative model100000 exceeds the helper's certified10000 matrix domain. Any further test/camera-source repair remains unbuilt/unexecuted here; no rails or mathematical domain have been relaxed to obtain a pass.

## Weak-cache runtime: two honest failures

Both DayZ arms use WarmMaterialPreflight1 and the same source/jobs/hot/profile/terrain prerequisites; solely weak1 versus weak0 differs. Runs are ordered, with unequal work/cache/timing, and cannot imply causal FPS or no-regression acceptance.

ON build/stream-residency/weak-cache-proof-on-20260927-disabled-20260927-234238 failed early NotExercised because no retained weak cell was observed despite cacheHandoffEnabled=true. Retained Harness/source/job/log observations show256 live strong bindings,134232 known bytes,367 wrapped reads,366 Init retains,3861 capacity refusals, weakCells0/weakBytes0/refusals0. The original runner produced neither result.json nor failure.json and no raw frame summary. Root supplemental screening-failure.json now records passed=false and the exact post-route source-state refusal; ownedExitCode and forcedTermination are null, shutdownCompleteObserved=true. This is separately recorded historical tool/log evidence, not an original runner result. Do not invent passing jobs, route metrics or cleanup fields. Cumulative transient-cell instrumentation is a future experiment and cannot retroactively pass this live-retention protocol.

OFF build/stream-residency/weak-cache-proof-off-20260927-disabled-20260927-234637 has result.passed=false. Strict hot owner-drop plus later-upload proof remains false: four observed logical debt drops, one completed/prepared member and Put1 but Take0/Upload0. Job queue/active/metadata/scratch return0; optional queue1056 bytes and prepared store1 entry1573776 bytes remain. Source strong256/known134232/wrapped379/Init371/capacityRefused4065, weak0. Returned resident/desired19923, required0,pendingfalse, live geometry710357640/capacity1275068416/retired0, textures2749573876/count2491/backend5302088752. The Shutdown complete marker is present. The historical original result did not persist owned exit code or forced-cleanup state, so both remain unknown; neither a shutdown marker nor a strict failed result establishes the missing cleanup fields.

OFF raw778 frames/drop0: mean45.843365038560414ms, usual median26.223ms, nearest-rank p95153.540ms,p99347.374ms,max1147.953ms. Percentile indexceil(n*p)-1. No ON route summary exists for an equivalent-work comparison. Diagnostic/profiling and resource/cache differences forbid performance claims.

## Evidence and remaining gates

build/feef-projected-water-weak-artifact-manifest-20260927.json records exact feef source snapshots, completed automated logs and actual runtime files without dumping arrays. Source snapshots are under build/feef-projected-water-weak-source. NEW cumulative weak counters, runner failure-retention repairs and projected test corrections remain separate pending work, excluded from feef installed evidence. Existing warm-material and useful-upload failures remain preserved. Full plan OPEN, defaultReady=false,performanceAccepted=false; no all-pass, visibility, pixel, full ownership, physical-free or causal performance acceptance.
