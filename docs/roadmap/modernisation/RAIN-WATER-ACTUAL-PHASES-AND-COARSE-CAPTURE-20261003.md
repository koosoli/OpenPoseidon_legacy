# Actual runoff attribution and exact scene capture, 3 October 2026

## Installed phase campaign

Root's `occupied-coarse-runoff-phases-20261003-184521-a4680c` ran installed
`572b838a` with both coarse diagnostics enabled and fine water explicitly OFF.
Both actual occupied stock UH60 arms passed normal exit zero, stable installed
EXE/DLL, source/weather/route guards and drop-free completed frame capture.
The physical routes displaced 103.35 m dry and 101.24 m rain. Both used ready
513x513 fields at 25 m spacing, generation 3. The rainy retained volume was
44,394.86 -> 55,985.21 m3 and automatic liquid rain 0.53960 -> 0.43632.
These are actual scene observations, not a synthetic performance input.

Use whole reporting windows whose previous AND current report endpoints are
inside the first/last actual poll clocks. The first eligible complete report
therefore follows the first eligible report endpoint. This avoids including a
partly preceding window, while retaining the original completed frame summary.

| Actual contained reports | Dry | Rain |
| --- | ---: | ---: |
| Report windows | 23 | 22 |
| Local first/last report | 18:46:29.097 / 18:46:51.232 | 18:47:59.284 / 18:48:20.454 |
| Advances / actual steps | 1,388 / 93 | 1,330 / 89 |
| Empty steps / multiple-step Advances | 93 / 0 | 0 / 0 |
| Elapsed Advance total, ms | 131.4601 | 1,567.1306 |
| Elapsed ms per Advance | 0.094712 | 1.178294 |
| Elapsed ms per actual step, including non-step calls | 1.413549 | 17.608209 |
| Maximum Advance, ms | 2.5208 | 42.5882 |
| >=50 ms Advances | 0 | 0 |
| Valid calling-thread CPU samples | 1,388 | 1,330 |
| Summed sampled thread CPU, ms | 156.25 | 1,281.25 |

Rain's nested phase totals/maxima are source/sink 132.2911 / 10.6763 ms,
clears 22.6402 / 1.6930 ms, raw transfer 800.5081 / 28.3075 ms,
limited flux 529.4307 / 15.0247 ms, final cells 81.4946 / 3.1222 ms.
All five have 89 calls. Raw transfer plus limited flux account for about 84.9%
of elapsed Advance time. There is no solver backlog: every stepping Advance
ran exactly one step. The dry empty skip is active and admits no raw/flux passes.

For comparison with prior endpoint-bounded reporting, the same current campaign
has dry 24 windows / 1,449 Advances, 137.1412 ms (0.094645 ms/Advance), and
rain 23 windows / 1,391 Advances, 1,614.8997 ms (1.160963 ms/Advance).
These include the first partially preceding report window; do not silently
mix this convention with the contained-window totals above.

Thread CPU samples are quantized to 15.625 ms in this campaign. The dry summed
CPU exceeds elapsed time, so these measurements cannot give an exact off-CPU
fraction. The maximum rainy Advance at 18:48:09.350 had one step, 42.5882 ms
elapsed and 15.625 ms sampled CPU. Its report contained several large phase
maxima; those maxima need not belong to that same step. Quantization and
possible descheduling remain unresolved. No >=50 ms call appeared anywhere
in either mission (including warmup). Whole-mission maxima were 13.7658 ms
dry and 42.5882 ms rain. The earlier 154.6936 ms episode was not reproduced.

Completed frames averaged 9.909981 ms dry (p95 11.675, maximum 40.122) and
13.644339 ms rain (p95 27.721, maximum 68.817). Instrumentation overhead,
changed water histories and scene/system variance prevent a guard-only or
unmodified-FPS comparison with earlier installed94 campaigns. The quiet94
repeat had dry 13.794825 / rain 12.129721 ms average frames, with Advance
0.173813 / 0.875189 ms per tick; the preceding94 campaign had 10.121966 /
12.260348 ms frames and 0.098284 / 0.993250 ms Advance, including its 154 ms
outlier. Performance remains open.

## Next decision requires actual arrays

The exact sqrt cap shortcut was previously bitwise-equivalent but regressed
mixed synthetic terrain by about 34.8%, so it remains unmerged. New phase
evidence identifies the actual edge passes as the largest target; it does not
prove that this particular branch will help. Existing harness state queries
do not expose exact coarse bed/depth arrays. Renderer heads cannot be inverted
reliably to recover raw bed bits. Capture actual arrays before revisiting the
shortcut or other edge-loop changes. Do not invent a sparse fixture or change
solver rates, budgets, precision, weather coverage or grid quality to improve a
timing number.

## Opt-in exact coarse capture contract

This source candidate starts from exact root572. `-CoarseCapture` sets/restores
`POSEIDON_RAIN_WATER_CAPTURE=1` and a per-arm designated campaign directory.
After `triPerfCapture 0`, releasing forward input and `setAccTime 0`, the runner
queries `dev_rain_water action=coarse_capture`. The handler requires actual zero
time acceleration, ready coarse field/fine OFF, an aligned current domain and
matching sea datum. It bit-compares EVERY raw bed cell against current native
`ClippedData` at the aligned source stride before exporting. World name/opaque
owner token, source revision, exact generation/revision strings, frozen clock,
native count, current liquid-rain/solar/wind inputs and forcing constants are
returned as receipts. This witnesses current native bed bits; it does not invent
a historical coarse source-owner token that the existing solver never stored.

The readonly accessor returns borrowed raw spans; it does not reconstruct
anything from rounded bed+depth heads. Invalid/nonfinite/negative depth,
oversized dimensions and fine fields are refused. There are no normal-step
copies, allocations or writes. Export is disabled by default and successful
export is allowed once per process. The handler accepts no caller-selected
file path. It creates `coarse-runoff.rwcap` under the canonical
`build/rain-helicopter/<campaign>/<Default|FarCoverOff>-<Dry|Rain>` directory,
refuses existing files, and reports failure instead of truncating a capture.

Binary format `RWCAP001`, little endian, has an 88-byte header:
8-byte ASCII magic; uint32 width,height; binary32 spacing,originX,originZ,sea;
binary64 pending seconds; uint64 generation,revision; binary64 rain,infiltration,
evaporation,outlet budgets. Raw binary32 bed array follows, then raw binary32
depth array, both in z-major/x-minor order. These are exact native IEEE bits.
Maximum size is 88 + 2*513*513*4 = 2,105,440 bytes, approximately 2.01 MiB.
The runner validates header/receipt sizes, exact identities and budgets,
before/after frozen clock/state, source/forcing receipts and designated path,
then records the file SHA256. It captures after the measurement, so the dump
does not contribute to the completed frame window or require a GPU readback.
The runner also asks for a second export and requires the actual one-time-gate
refusal, unchanged file hash, clock and field state.

## Actual capture precision failure and source repair

Root's `occupied-exact-coarse-scene-20261003-193540-c355bb` ran installed
`16d296c6` and exited normally with code zero. The campaign remains **FAILED**:
the first dry capture validator rejected `pendingSeconds` before subsequent
duplicate-refusal and rainy-arm acceptance. The retained binary is the full
2,105,440 bytes and its handler witnessed all 263,169 native bed cells.
These positive observations do not make the capture campaign pass.

The binary pending double is `0.16667026095092297`, IEEE bits
`3FC555737C000000`. cJSON emitted `0.166670260950923`, whose parsed double
is `3FC555737C000001`, exactly one ULP higher. This is JSON decimal precision
loss, not evidence that the field changed during its paused capture.

The source repair adds canonical 16-digit uppercase IEEE binary64 hex strings
for pending time and all four budgets to the capture receipt and to state
responses only when the capture flag is exactly `1`. The validator compares
each native header double's bits against the capture, before-state and
after-state strings exactly. Missing, mistyped, malformed or one-ULP changed
strings fail. Existing numeric before/after equality, paused clock, source,
path, generation/revision and duplicate-export guards stay strict. No numeric
tolerance, binary format change or solver change is introduced. The regression
fixture uses this observed non-round-tripping pending value and rejects altered
bits for each budget and pending field. Root must rebuild and rerun actual
capture acceptance; the repaired source tests are not an installed pass.

## Validation and root-owned next run

`scripts/Test-RainWaterCoarseCapture.ps1` passed native O0/O2 IEEE-bit,
readonly budget/generation/revision/pending and bounded-path/dimension checks.
Its PowerShell fixtures check the binary header, unavailable/changed source,
clock, budget, generation, path, forcing and truncated-file rejection. These
small unit inputs are explicitly synthetic and are not performance evidence.
The existing occupied-flight self-test also passes. Root owns full engine build,
dual-binary deployment and actual installed positive/refusal smoke verification.

After that integration, root can run under the existing game lock:

```bash
LOCK_OWNER="occupied exact coarse scene capture" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-RainHelicopterPerformance.ps1 \
  -ExpectedCommit INSTALLED_COMMIT -CloudletCosts -CoarseCosts -CoarseCapture \
  -Label occupied-exact-coarse-scene
```

Preserve both raw files and result receipts. Any later CPU benchmark must name
their actual hashes, preserve exact initial state/forcing, compare identical
kernel operations bitwise first, and be serialized against root builds and
performance measurements. No production math change is included here.
