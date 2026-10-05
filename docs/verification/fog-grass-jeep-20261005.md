# Fog, grass and bounded Jeep mud prototype

Installed paired client/renderer: `488a93e7`, `2026-10-05_11:44`, via Deploy.ps1 on `codex/kolgujev-fog-corpse-fixes-20261004`. The receipt records both SHA256 values and source stamps. Documentation-only follow-up does not change the installed code.

## Fog

At original Everon `[8131.43,4086.37,191.04]`, az229.1/el-11.3, fog0.78, the installed baseline reproduced sharply bounded bright ground patches. Rain shelter previously returned neutral sun transmission at canopy-covered receivers. Incoming solar transport now distinguishes air from rain interception; actual surface CSM retains solid shadowing. View/roof refusal remains. Filtered comparison coverage replaces abrupt near-complete-exposure rejection.

Incoming solar transport is encoded once, sampled in the separate view pass, and its world grid is snapped to stable cells. The view volume increases from32x32x32 to64x64x32. Separate layouts/passes prevent solar storage/sample aliasing. One layer is the default; the second remains selectable.

Twelve asynchronous GPU-region samples per run at1280x720,4xMSAA,grass off: baseline mean4.655ms (4.494..5.158); optimized old two-layer profile mean1.100ms (1.075..1.303); final default mean1.105ms (1.085..1.294). Roughly76% lower fog-region cost on this specific scene, including the doubled view-axis resolution. This is not a whole-game FPS guarantee or a universal visual-parity certificate.

Final Everon13 poses and original Kolgujev compact temporal26 poses pass camera/source/weather assertions, normal exit and renderer health. Earlier fixed-ASL valley flights and Everon default-grass flight also passed. Sampled views reduce the reproduced ground discontinuities; finite screenshots do not prove every possible flight is flicker-free.

Valley preset uses fixed ASL and nearby low ground instead of a layer climbing mountains. Lowering Top below Bottom now lowers Bottom rather than undoing the user's Top change. UI distinguishes AGL from ASL and explains that legacy Fog amount also shortens visibility. For a thick valley bank: low Fog amount, ASL ceiling below hilltops, increase Fog density. The second default layer is disabled. Actual fog scattering already adds sun/local-light shafts as participating optical depth grows; the existing clear-weather atmosphere/shafts are preserved. Very dense fog eventually obscures shafts rather than increasing them without limit.

## Grass

Master coverage0.5 remains. Mid coverage4 and radius500m are default; mid control permits50. Fixed2440-square candidate and1,048,576-instance budgets remain. At the limit extra requested coverage broadens clumps rather than silently shrinking reach or allocating50x buffers. Daylight backlight sky fill is partly neutral/warm at unchanged luminance; night and direct sun remain unchanged. Actual default-grass Everon flight completed. High50 CPU/WGSL placement bounds and actual GPU pipelines passed; high50 whole-map visual/performance acceptance remains owner tuning.

## Jeep

Stock class Jeep only. Actual identified wheel ground collisions create distance-spaced, rain-softened ruts capped12cm. Shared admission preserves soil/dry/road/roof/sea/snow refusal; stationary, unsupported and teleport gaps break trails. Installed original Nogova AI Jeep moved8.49m on `o/pole2.paa` after real rain and produced about10cm negative offsets. Close capture is descriptive; no authored tread texture/parallax or generic traction/softbody system was added. Far vehicles retain their existing support-plane approximation. Native/A3 mixed surfaces are still refused.

Tests:15 layered-fog GPU/settings tests,36 grass tests, focused wheel schedule/contact tests and existing MudField tests pass. Final source built and installed; owned games exited normally. Private screenshots/full receipts stay in build directories; compact source/camera/image hashes are tracked in the accompanying JSON.
