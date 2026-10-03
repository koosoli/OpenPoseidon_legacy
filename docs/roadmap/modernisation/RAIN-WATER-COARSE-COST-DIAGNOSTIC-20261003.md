# Coarse runoff attribution, 3 October 2026

Source candidate based on exact root `94f261c5`; source only until the root
owner builds, deploys both binaries and runs the installed campaign. This is
instrumentation, not a performance fix or acceptance of the limiter guard.

## Actual episodes motivating the diagnostic

Root campaign `calibrated-guard-flight-20261003-170301-4137aa` passed normal
exit, stable installed pair, actual occupied route/weather/source guards and
drop-free completed frame capture. Rain reported 24 windows / 1,450 simulation
ticks: 1,440.2128 ms elapsed `Advance`, or 0.9932502 ms/tick. The report at
17:05:50.158 contained a 154.6936 ms maximum call and 198.8643 ms total over
60 ticks. The completed CPU ring first published the 165.3106 ms frame /
155.7478 ms simulation phase at the 17:05:49.542 poll. These rolling published
windows do not identify a single GPU frame or give exact event start/end.

The largest call alone contributes 0.1066852 ms/tick and 10.74% of measured
Advance time. Other report windows average 0.8930565 ms/tick (diagnostic
exclusion, not a replacement acceptance statistic). Nearby particle simulation
was 0.278 ms; source, natural and soft-surface maxima were <=0.002 ms.
An asynchronously published GPU total of 55.087 ms at 17:05:49.362 had 44.740
ms unexplained residual. It cannot establish same-frame causality.

The quiet repeat `quiet-calibrated-guard-repeat-20261003-172040-45e935` also
passed its functional/normal-exit controls. Root's actual weighted Advance
totals were dry 253.5933 ms / 1,459 ticks = 0.1738131 ms/tick (max 7.1419),
rain 1,267.2735 ms / 1,448 ticks = 0.8751889 ms/tick (max 23.0041).
The 154 ms spike did not recur. Dry completed frames averaged 13.794825 ms
(p95 26.934), while rain averaged 12.129721 ms (p95 18.861). This noisy reversal
does not close performance acceptance.

Current calibration, rainfall and stored water histories differ from the earlier
0a163 campaign. Several unrelated CPU phases increased, including setup,
draw preparation, HUD and sound. No own compilation/benchmark overlapped
either measurement. These logs do not distinguish expensive computation from
descheduling, page faults, CPU frequency changes or other external contention.

## Exact diagnostic contract

`POSEIDON_RAIN_WATER_COST=1` enables the header-only diagnostic. Any other or
missing value disables it. Default-disabled scopes perform no clock reads,
thread-time queries, accumulator writes or logs; the environment is read once.
The coarse solver retains all floating operations, pass order, empty-edge skip,
budgets, pending time and revision behavior. Fine dispatch is unchanged and
does not contribute to these counters. No allocation, wait or readback is added
to the solver.

Each valid coarse Advance records elapsed time, actual zero/one/multiple step
counts, and an entry/end `GetThreadTimes(GetCurrentThread())` CPU delta on
Windows. Failed or unsupported samples are unavailable (`cpu_at_wall_max_ms=-1`),
never zero-cost evidence. CPU sampling spans both API reads; wall timing spans
the solver between them, so boundaries are slightly different. Windows thread
time is coarse despite FILETIME's 100 ns storage unit: a short scope can report
zero or quantized CPU time. A slow elapsed call with little sampled thread CPU
supports investigating off-CPU time; it cannot prove a particular OS cause.
Total/max wall, total CPU, valid CPU sample count, >=50 ms call count, and the
CPU sample plus exact step count associated with the maximum wall call are
reported together.

Actual coarse Steps record five nested elapsed phases: source/sink pass,
four vector clears, raw-transfer/donor-outgoing pass, limited flux/delta/flow
pass, and final cell/outlet/revision pass. Raw/flux calls must equal steps minus
empty steps; source/clear/final calls must equal steps. Phase timings include
small diagnostic accounting overhead and are elapsed time, not exclusive CPU
time. They are nested within Advance, so must not be added to its total.

World.cpp reports one summary and five phase rows in the existing once per
frame-time second reporting block, outside the measured Advance/Step scopes.
No per-step log is emitted. Reports remain whole windows; report maxima need
not belong to the same step, and no GPU identity is claimed.

## Verification and installed measurement

`scripts/Test-RainWaterCost.ps1` passes accounting tests at O0/O2 for OFF,
exact `1`, and invalid flags. It checks actual empty and wet step admission,
invalid input, unavailable/reversed CPU samples, max association and reset.
The existing pinned actual reference test passes bitwise O0/O2 with diagnostics
OFF and ON across flat/slope/checker/hollow/sea/extreme beds, dry/rain/drying,
wind, editing, regrid, backlog, partitions and finite guards; it observes zero
per-Advance C++ allocations. Shared rain forcing is normalized identically in
the existing reference seam. This is not an OS allocation/page-fault proof.
Runner self-tests reject orphan, incomplete, misordered and impossible phase
receipts. The runner explicitly records/restores the new flag and validates
actual reports rather than relying on inherited environment state.

After integration and deployment, root owns this serialized invocation from its
checkout, substituting the verified installed commit:

```bash
LOCK_OWNER="occupied coarse runoff phase attribution" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-RainHelicopterPerformance.ps1 \
  -ExpectedCommit INSTALLED_COMMIT -CloudletCosts -CoarseCosts \
  -Label occupied-coarse-runoff-phases
```

Interpret this as an instrumented attribution run, not an uninstrumented FPS
comparison. Preserve complete logs and choose report windows inside the actual
poll/capture interval. If slow elapsed calls have low sampled CPU and no source
change, a serialized OS scheduling/page-fault trace is the next appropriate
investigation; neither source inspection nor this diagnostic replaces that trace.
