# Native forest checkpoint, not a completed optimisation

Installed bd1f889c; Measure-NativeReforger -Pose Forest -Repeats 1, output
build/native-reforger-perf/Forest-bd1f889c-forest. Clean shutdown and strict
lifecycle checks pass. Native 1920x1080, 4x MSAA, DLSS off, governor range 1,
90-second warmup. This is one static pose, not the required moving-route A/B.

The sampled 256-frame window reports 28.63 FPS, 34.92 ms mean, 35.12 ms P95,
597.32 ms maximum. The maximum is not isolated from capture overhead; do not
advertise it as a representative streaming hitch. Snapshot GPU total 33.32 ms,
far shadow cascade 9.04 ms, next cascade 3.39 ms. CPU simulation 0.105 ms mean;
producer waits 28.48 ms. This pose does not justify parallelising AI Think.

Loading totals are separate: 1220 texture uploads, 680.10 ms peak upload work
in one frame, 9.50 seconds cumulative cold model optimisation, 2.18 seconds
texture-header loading. Existing two-worker preparation settled with no pending
queue but seven failures. Residency reached 19999 of 20000 object slots.
Tracked GPU residency 1603.90 MiB is not total driver VRAM. No before/after
performance claim; neither satmap objects nor far-tier coverage is implemented
by collecting this evidence.

Inspected image still looks pale in distant foliage. Current direct/retained
wind shaders already share root phase and trunk bending, with leaf-only flutter
capped at 2.5 cm times the flutter setting; matching velocity formulas exist.
A still image cannot accept trunk/crown synchronisation. Do not repeat that cap
as a new fix; obtain a matched motion sequence before changing the formula.

## Motion checkpoint on installed 7329e2c4

Eight fixed-camera captures, 0.5 seconds apart after the same 90-second warmup:
build/native-reforger-perf/Forest-7329-motion. All eight PNGs exist; lifecycle,
native-resolution profile and settled-preparation checks pass. First and last
frames inspected: no obvious detached crown at this camera, but this is not
acceptance for other tree species, LODs or strong wind. Pale foliage persists.
No wind shader change justified by this sequence alone.

farfield-bench and Measure-NativeReforger now support bounded motion sequences.
Metadata marks them performance_comparable=false; farfield-compare rejects them
and three regression tests cover rejection and legacy/single-frame acceptance.
Readback-contaminated frame timings are not an optimisation result.

## Moving residency trace on installed 142b6d6a

Evidence: build/native-stream-cancellation/cancel-20260911-215738-slow-trace-bank-trace-texture-keys-1-lazy-bcr-default-bc7-default-driver-memory.
Test-NativeStreamCancellation with TraceSlowFrames, TraceBankTables and
SampleDriverMemory; same 100 m/s route, 1280x720 native/MSAA4/DLSS off,
Ultra profile, fixed LOD range 1. Lifecycle and route/coverage gates pass.
No rendering change or A/B treatment in this run.

Full FrameProfiler windows, milliseconds (not log timestamp gaps):
static n=288 median 33.884, P95 37.638, P99 39.815, max 112.463;
moving n=1286 median 30.649, P95 43.819, P99 89.410, max 160.093.
Five moving frames exceed 100 ms, none 250 ms. Late screenshot frame 4119
takes 269.984 ms and is outside the moving window; do not count as streaming.

Worst moving frame 1486: residency 156.476 ms, simulation 0.991 ms.
M923A1_wreck_static.xob cold admission takes 155.0 ms: optimisation/registration
154.9, mesh 1.6, section excluding textures 76.9, texture work 72.0 ms
(read 1.1, create 60.1, fallback 10.8). Header timer 69.5 ms overlaps the
section bucket: do not add it again. Bank preparation here is only 0.025 ms.
This particular tail is not evidence for parallel AI or a new scheduler.
Texture create currently includes producer-window acquisition; add a subset
timer before attributing all of it to staging or driver work.

Peak process working set 4116471808 bytes, private bytes 7860178944 (1 Hz).
Whole-GPU driver usage peaks at 4398 MiB, includes other applications.
Endpoint tracked GPU allocation 1675.057 MiB; object textures 1003.057 MiB /
1417 entries, live geometry 272.790 MiB (pool capacity 672 MiB).
Prepared queue drains; DDS preparation remains deliberately off after prior
unsuccessful experiments. No FPS/memory improvement inferred from this trace.

### Producer-window split and narrow experiment

Installed 4746f6b9, same route/options, cancel-20260911-222708-...-driver-memory:
lifecycle/coverage pass; moving n=1255 median 31.793, P95 43.589, P99 88.579,
max 153.309 ms (four >100 ms). M923A1 wreck admission 145.4 ms, normal/material
headers 67.5 ms, create bucket 55.9 ms. New subset texWindowWait=54.047 ms:
most of that create bucket is producer ownership waiting, not GPU staging CPU
cost. The subset is already included in existing buckets, not additive.

Try WGR_NATIVE_NORMAL_PREFETCH=1 (default OFF while under evaluation): before
a cold native model's first renderer operation, resolve its normal-map sources
on the main thread using the SAME material-binding and texture-bank caches.
No uploads, second store, scheduler, worker mutation or material substitution.
Only authored non-procedural NormalMap stages from native EMATs are eligible;
OFP/P3D paths, warm/parked models and procedural stages stay unchanged.
This reorders work to overlap the prior render frame; it does not eliminate
decoding or make a single model admission fully budgeted. Compare paired routes,
coverage, texture counts/bytes, memory and appearance before any default change.

### ABBA decision: keep the experiment off, streaming remains open

Same installed 804ad7d4, four complete routes with identical settings and driver
sampling. Run suffixes below refer to build/native-stream-cancellation/cancel-
20260911-HHMMSS-...; enabled runs additionally end in -normal-prefetch.

| Start | Prefetch | Moving n | Median | P95 | P99 | Max | >100 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| 223656 | off | 1208 | 32.458 | 45.102 | 88.606 | 154.832 | 3 |
| 223913 | on | 1167 | 33.626 | 46.244 | 90.699 | 141.097 | 4 |
| 224214 | on | 1198 | 32.587 | 50.164 | 91.623 | 133.175 | 5 |
| 224431 | off | 1145 | 33.706 | 51.135 | 96.748 | 161.249 | 10 |

Milliseconds from full FrameProfiler moving windows. Maximum improves twice,
but median/P95/P99 are mixed and the last control's static window also slows.
Do not claim steady FPS improvement or a conclusive broad tail reduction.
All four retain 22370 registered instances, 1417 object textures / 1003.057 MiB,
272.790 MiB live geometry and 1675.057 MiB tracked GPU allocation. Private byte
peaks in order: 7903981568, 7915106304, 7914373120, 7723655168. No memory saving.

Village-normal-prefetch-0/1 captures on the same build both pass strict native
1920x1080/MSAA4/no-DLSS, settled preparation and clean shutdown. Both have 22360
instances, 5362 main instances and 13499377 main triangles. Inspected paired
images agree on road, fence, roof and facade materials; moving foliage differs.
No shader or asset changed. Single endpoint timings are not an FPS comparison.

Keep WGR_NATIVE_NORMAL_PREFETCH default OFF. This small source-order experiment
does not close budgeted model admission, far coverage or native forest quality.
The ownership-wait attribution is retained as useful evidence for subsequent
work; do not replace it with an assumption that all create_ms is upload cost.
