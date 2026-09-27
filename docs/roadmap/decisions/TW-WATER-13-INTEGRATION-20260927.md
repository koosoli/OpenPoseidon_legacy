# Tidewater W6 integration checkpoint

All requested newer Tidewater source commits are integrated. The matched build and two controlled showcase startup/capture checks passed. Current OP remains the default water backend; Tidewater Native requires explicit selection. This checkpoint does not accept water quality, general gameplay, network equivalence or performance.

## Exact source coverage and integration

Source refs/remotes/safety/dec/tidewater-w6d-amphib-dev ends at3e99fee6f120dc0be9ba943d5f51356dc889524f. Independent git cherry against88cd7110 confirms nine already patch-equivalent commits and27 missing commits. The integration merge14f323cf3f05b2e94ef5df66fd09402abe77c953 retains full source-tip ancestry; original commit mapping is preserved in build/tidewater-w6-integration-commits-20260927.json. The equivalent first-nine snapshot a2b9c135 was the explicit three-way base used to avoid false add/add conflicts, with water-only hunks retaining the newer streaming work.

The27 new commits cover Tidewater sea/weather conditions and CPU sea approximation, underwater lighting/compositing and caustics, breaker spray/reactive coverage, shoreline attenuation/foam, underwater camera travel, vehicle flooding/sinking/fire and submerged crater/cloud behavior, plus development controls. Source-authored decisions07–12 remain historical source-branch evidence; their test and owner statements are not automatically evidence for this merged binary.

Matched C++ and Rust ABI18 changes WgrWaterParams1184 to1216 bytes by appending sea/effect lanes at1184 and1200. Size/hash/version checks and old17-with-current-hash refusal prevent silently accepting the older packet. Existing render-call CPU snapshot72 and geometry-pass facts592 are unchanged. The geometry worker, certificate/hot-proof changes, current timing and CLOD ownership paths remain preserved. Later camera-tuple and weak-cache candidates are outside this installed checkpoint.

## Default behavior and costs

WaterSettings.waterBackend remains0, Rust starts CurrentOp, and exact WGR_WATER_BACKEND=0|1 remains the explicit override. Tidewater's resources are constructed only when that backend activates; normal Current OP does not acquire a second full heightfield copy, build shoreline tiles or run Tidewater caustic/spray/underwater pipelines. Existing lazy owner shore capture is retained.

Default0 does not disable all requested gameplay commits. Car CancelStop now keeps afloat/airborne vehicles simulating; wreck flooding reduces buoyancy; sunk ships stay on the seabed instead of being deleted. Scripted/free cameras can descend below the surface, submerged crater/fire behavior changes, and underwater cloud compositing is excluded. TankOrCar gains flood state and the renderer gains camera-slot/submerged scalars. These are active behavior/lifetime changes that can add long-mission work; no default gameplay or performance equivalence is claimed.

New sea-preset interpolation still occurs in the existing unconditional C++ Tidewater parameter-packing block even under backend0; the appended32 bytes also enlarge the shared uniform packet. This is bounded scalar work, not zero overhead. Terrain shoreline derivatives remain gated to backend1. The CPU predictor is a six-band approximation of the selected sea; headless GEngine-absent execution retains the reference branch. Client/server sea equality, full FFT identity and multiplayer determinism are not established. Development WGR_AMPHIB_WAVE positive atof and WGR_CAR_TRACE leading1 parsing preserve source behavior; they are not strict bounded production tuning certificates.

## Actual build and tests

build/build-tidewater-w6-integration-20260927.log records all five targets passing254 scheduled steps at build HEAD8153881b, after the integration source merge. Supported deployment stamped8153881b at22:26; the source code is14f323cf, with8153881b adding the prior surface/hot ledger checkpoint.

- Rust water_tw:44 passed, zero failures, one intentionally ignored slow debug lace_512 comparison.
- ABI:21 passed, zero ignored/failures.
- C++ vehicle/water:4119 assertions in13 cases passed without skips.

Actual installed EXE SHA256 A1D3587C1B9151BB0C9E0E85851A9002CB87AAF693B4729D5AE2B745DFCEDC0D; renderer DLL3B8402E4ACE5A7A9C61726C7C723BA94A2A50B096BC4C02727DE18C2639EEFF9. The exact48 integration source snapshots under build/tidewater-w6-source-14f323cf match committed git show.

## Controlled installed startup/capture checks

build/tidewater-w6-smoke-0-20260927-222805/result.json and build/tidewater-w6-smoke-1-20260927-222851/result.json both record passed=true, exitCode0, normalShutdown=true and forcedTermination=false. Each post-exit EXE/DLL hash remains the matched pair above. Explicit1 records CurrentOP to TidewaterNative activation and native shader creation without errors. Explicit0 has no Tidewater initialization, independently checked by root in the completed log; the original helper did not itself assert that negative condition.

Root viewed both actual images: the local coastline and LST remain visible, while water appearance differs between backends. This is a narrow startup/capture and local-image observation. No direction/wave/underwater/wreck gameplay, whole-map quality, pixel parity, network behavior, frame percentiles or default no-regression acceptance is derived from it. No benchmark or extra runtime was executed by the reviewing agent.

## Immutable evidence and remaining gates

The64-file artifact manifest build/tidewater-w6-artifact-manifest-20260927.json includes the original patch/mapping/review, actual build/tests, both result/provenance/log/image sets and48 exact source snapshots. All64 sizes and SHA256 values were independently recomputed, and source snapshots independently compared with the integration commit. Manifest SHA256 A10A0BA51519B308886EF029CE8B9D6640B719AA56337B47B43DD3187E140AAD. Original integration patch SHA2563031D914BBB49865AA48BC26C8FBA83EFE36FDD423FF8C06BD1E4A33C8B38F74.

The complete streaming and modernization plan remains OPEN. defaultReady=false and performanceAccepted=false. Actual surface-admission successes and hot/warm useful-upload failures remain unchanged. Complete ownership, physical reclaim, general runtime paging/quality, retail/MP compatibility and causal performance gates stay open. Further checks should target unresolved behavior or performance evidence, rather than treating this successful startup checkpoint as whole-plan completion.
