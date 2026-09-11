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
