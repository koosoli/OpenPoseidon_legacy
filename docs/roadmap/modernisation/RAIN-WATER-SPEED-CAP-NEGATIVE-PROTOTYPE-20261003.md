# Exact coarse runoff speed-cap shortcut: rejected for production

Keep the current coarse solver unchanged. A prototype skipping square roots on already capped speeds passed exact-output checks but regressed the explicit irregular-bed CPU benchmark. Gains on some regular steep beds were small or confounded by outliers. This is a negative CPU optimisation result, not installed-game, map coverage or FPS acceptance.

## Actual source and equivalence

The benchmark generated complete classes from the pinned pre-cache `e017e028812dcd7ce050497fd40bf81253a1bcf1` header, cached `f29050a5` header, and actual root header at `4077009f74607026c007858afb5456a7d25b34fc`. The actual current header SHA256 was `DDFEDC79ECC4B9726E0226FDA6B27078939DEE5B1491242DEF746C2707800D29`. Only class/singleton names changed for coexistence. The prototype was generated from current source by this one replacement:

```cpp
// Current:
const float speed = std::min(5.0f, std::sqrt(9.81f * std::abs(head)));
// Prototype:
const float gravityHead = 9.81f * std::abs(head);
const float speed = gravityHead >= 25.0f ? 5.0f
    : std::min(5.0f, std::sqrt(gravityHead));
```

The comparison uses the exact already-multiplied float value, with no head-threshold approximation or arithmetic reordering. Retaining `std::min` in the fallback matters: finite accepted bed/depth can overflow free-surface sums, yielding `Inf−Inf = NaN`; the original ordinary `std::min(5,sqrt(NaN))` comparator returns 5. A bare `else sqrt` would change that case. No timestep, precision, donor limiter, source/sink budget, flow order or fine solver changed.

Clang `-O0` and `-O2` proofs passed against actual current and pinned old classes: bitwise public four-lane snapshots, water budgets, pending time, generation/revision, and readiness across flat/sloped/capped/checker/random/extreme/sea beds, finite-max water, rain/drying/wind, bed edits, sea changes, reset/regrid, partitions and retained backlog. A million raw IEEE speed inputs (including subnormals, signed zero, infinities and NaNs), plus adjacent representable positive/negative inputs around the cap, passed exact speed checks. This establishes the tested numerical equivalence; it does not establish runtime benefit.

## Bounded CPU measurements

The isolated `-O2` benchmark used 513×513 complete coarse fields at 25 m spacing, eight warmup steps followed by 24 measured quarter-second `Advance` calls. Fields began dry and received ordinary fixed .85 rain, .4 sun, .1 wind during these explicit synthetic tests. Configure and public Snapshot inspection were outside the timing window. Four replicas rotated the four variant orders. All reported units are milliseconds per actual Advance call, not per rendered frame. The beds are explicit synthetic inputs, not Nogova source substitutes. The separate public-state edge census is after warmup, not a per-step instrumented counter.

| Synthetic case | Current mean [replica range] ms | Prototype mean [replica range] ms | Eligible / already capped edges after warmup |
| --- | --- | --- | --- |
| Dry | 1.0082 [0.9884, 1.0342] | 1.0486 [1.0058, 1.0888] | 0 / 0 |
| Flat rain | 3.6126 [3.5698, 3.6666] | 3.6797 [3.5735, 3.8026] | 0 / 0 |
| Gentle .2/−.1 m per cell | 7.0032 [5.7082, 10.4399] | 8.3385 [5.7308, 10.0266] | 525,312 / 0 |
| 3/1 m slope | 5.2807 [5.0694, 5.5083] | 5.2337 [5.0782, 5.4887] | 525,312 / 262,656 |
| 5/3 m slope | 6.3659 [5.0824, 9.8333] | 5.9049 [4.9788, 8.6350] | 525,312 / 525,312 |
| Alternating ±3 m | 6.7454 [5.3934, 9.8639] | 5.3483 [5.2262, 5.4921] | 525,312 / 525,312 |
| Irregular 0–6 m relief | 5.5621 [5.2640, 6.0697] | 7.4972 [7.0489, 7.8402] | 525,238 / 173,830 |
| Sea/hollows | 6.6876 [5.1305, 8.3803] | 5.3228 [5.1483, 5.5058] | 460,225 / 513 |

The irregular test regressed about 34.8%, with nonoverlapping observed ranges. The 50%-capped regular slope differs less than 1%, with overlapping ranges. The all-capped slope's best replicas differ about 2%; its mean improvement includes large outliers. The apparent sea/hollow mean improvement cannot credibly be attributed to skipping square roots on only 0.11% of eligible edges; its best replicas are essentially equal. Dry and flat cases show no useful work removal. The first run was also inconsistent (candidate/current irregular ratio 1.073 and 50%-capped slope 1.201), supporting caution rather than a selected favourable result.

Old/cache/current/prototype means for the second run are preserved in the isolated raw output `build/rain-water-speed-cap-prototype/benchmark.txt`; respectively dry 3.3458/3.6220/1.0082/1.0486, gentle 8.3601/7.5781/7.0032/8.3385, and irregular 9.4949/5.4104/5.5621/7.4972. This broad comparison is contextual, not new acceptance of the earlier cache/empty optimisations. Their installed attribution remains authoritative.

Skipping actual capped square roots is a genuine work reduction, but these measurements do not justify the extra condition for the default solver. Branch outcome/layout costs are a possible explanation for the mixed-terrain regression; no hardware branch counters were collected, so that mechanism is an inference. Background scheduling/thermal/compiler layout effects were not fully isolated, and observed outliers prevent generalising favourable means.

## Preserved isolated prototype

The opt-in script `Test-RainWaterSpeedCapPrototype.ps1` and complete-class test are preserved on `codex/coarse-transfer-cache`; they are not production kernel changes or default build tests. Run with explicit `-SourceRoot` for the desired actual checkout, `-Compiler` for Clang and `-Benchmark` only when collecting CPU timings. Without `-Benchmark`, only the two optimisation-level equivalence checks run. Root merges this audit document alone. No production solver change, full build, GPU/device test, game launch or deployment was performed by this prototype task.
