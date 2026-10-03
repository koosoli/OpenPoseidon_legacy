# Exact local coarse donor-limiter guard

Candidate only: skip the existing donor-ratio division when its local clamp result is already exactly one. Keep the original `_out[donor]>0` admission, multiplication and full division/clamp fallback. No rate, timestep, precision, sink budget, flow order, geometry, fine solver, scheduling or memory change is part of this optimisation. Installed rainy-flight performance acceptance remains with the root agent.

```cpp
if (_out[donor]>0)
    f *= _out[donor] <= _depth[donor] ? 1.0f
        : std::min(1.0f,_depth[donor]/_out[donor]);
```

For positive outgoing volume and finite depth at least as large, the original ratio cannot round below one. Inf/Inf also retains the original result: `std::min(1,NaN)` returns one under its ordinary comparator. Unordered depth, excessive outgoing volume, signed-zero depth and all other cases use the unchanged fallback. No global assumption about terrain spacing is made. Small-spacing partial donor limiting still operates.

## Exact source and compiled work

The generated prototypes read complete actual root headers through explicit `-SourceRoot`; their coarse header is source-equivalent to `fa855a12535b1d3d8596834edb579aac950bd5a3`, with CRLF file SHA256 `DDFEDC79ECC4B9726E0226FDA6B27078939DEE5B1491242DEF746C2707800D29`. This identifies the header, not a claim that the whole checkout remained on that commit: saved scratch/cache run receipts identify later root HEADs `1930ffd6` and `3fabb8bd` with that same file hash. The candidate replaced this one normalisation expression, retaining a copied current control and actual pinned pre-cache `e017` reference. Fine ownership was inactive in these coarse checks.

The isolated **production patch branch** was different: it still contained the earlier `6123f4f5` transfer-cache base, without root's later exact-empty `hasWater` wrapper. Keeping that stale patch base instead of rebasing to the requested current root caused the `f2d748ad` merge conflict. Its standalone production test is not proof of integration with the empty skip. Root preserved both the wrapper and guard in `84d9c7fd`, both regression tests, and the separate forcing calibration in combined `94f261c5`; root reports the combined native cache/field/fine/source/geometry/flux checks passing. This integration supersedes the stale patch base.

A subsequent **read-only byte audit** distinguishes the patch issue from measured prototype source. Reversing only class/singleton names in saved scratch/cache `Current.hpp` produces exactly the LF git `fa855` header: SHA256 `0704BB5A8927750DE5A1A569FC2572AAB2682EFB37D36AEF9D01D159D04E7D33`. Recreating CRLF reproduces the recorded `DDFEDC79…` hash. Both copies include post-sink `hasWater` and `if(hasWater)` around the edge passes. Later guard current/candidate copies, overwritten by the shared-forcing proof, also match `fa855` after explicitly undoing only the independent forcing input/include and candidate guard. Thus the copied benchmark controls retain the empty skip; the stale production branch does not. A complete repository/dependency equivalence claim is not made from this single-header audit.

Clang `-O0`/`-O2` comparisons passed bitwise public four-lane snapshots, budgets, pending time and generation/revision over flat, sloped, checker, irregular, sea, raw finite IEEE/extreme and signed-zero beds; rain/drying/wind; bed/sea changes; reset/regrid; frame partitions; backlog; and wet-to-exact-dry-to-new-rain lifecycle. Temporary direct checks included a million raw IEEE outgoing/depth pairs and focused signed-zero, denormal, NaN, Inf and finite-max cases. Configure allocation count/bytes were unchanged; no Advance allocation occurred.

The permanent focused existing coarse test adds actual 3×3 raised-centre donation at .125, 2 and 25 m spacing. Small-spacing water needs partial donor limiting and drains the centre; default coarse spacing retains most centre water. Both use the actual old-class comparison and retain existing budgets, finite guards, MaxCells/cache allocation checks and all prior field scenarios. The million-input prototype is not added to the default tests.

An `-O2` LLVM inspection of a wrapper calling actual candidate Advance confirms the second edge pass preserves the positive-outgoing branch and then branches on the local outgoing/depth comparison. `fdiv` occurs only in the fallback block. The factor-one path merges through a phi into the same `fmul`, delta and flow operations. This is compiled division removal, not a source-only conditional that still computes both operands. Floating flags/exceptions were not separately instrumented.

## Bounded CPU evidence, before rainfall calibration

The serial paired benchmark used complete 513×513 coarse classes at 25 m spacing, eight warmup steps and 24 timed quarter-second Advance calls per sample, with eight alternating current/candidate replicas. Configure, snapshots and the public-state edge census were outside timing. Inputs were explicit synthetic beds (base 5,000 m for regular/irregular cases, with separate low sea/hollow beds), starting dry under the then-current maximum source `.00012 m/s`, with rain .85, sun .4, wind .1. These inputs are not actual map source or installed-game/FPS evidence.

| Explicit synthetic input | Current median ms/Advance | Guard median ms/Advance |
| --- | --- | --- |
| Dry | 1.0342 | 1.0461 |
| Flat rain | 3.6332 | 3.6395 |
| Gentle slope | 5.5175 | 5.5320 |
| 3/1 m slope | 6.9193 | 5.6296 |
| 5/3 m slope | 5.4372 | 5.3067 |
| Alternating ±3 m | 5.5647 | 5.1719 |
| Irregular 0–6 m relief | 6.7765 | 5.6334 |
| Sea/hollows | 6.1553 | 6.0233 |

Irregular median improves about 16.9%, checker about 7.1%; gentle, dry and flat show no useful improvement. Ranges overlap and contain outliers: irregular current 5.9676–10.9391 ms versus guard 5.5308–11.9858 ms. Irregular means are 7.6799/7.1902 ms; the smaller mean gain illustrates uncertainty. The apparent dry difference cannot be caused by this edge guard when no edge pass runs. Background scheduling/thermal effects were not fully controlled. An earlier four-variant timing overlapped a one-second diagnostic LLVM compile and is explicitly discarded; the reported paired run had no concurrent compilation by this agent. No uniform improvement or FPS claim is made.

The roughly 1 ms dry microbenchmark is **per `.25 s` Advance call**, each doing a fixed solver step. Installed attribution near .09 ms is **per simulation tick**, averaged across ticks including calls that do not yet reach the fixed-step interval. These values cannot be compared directly; at 60 ticks/s one quarter-second step is spread across about 15 ticks. The dry microbenchmark value is not evidence that its `hasWater` wrapper was absent. Final combined installed-game attribution remains authoritative; no additional compile or benchmark was run for this documentation audit.

## Independent forcing and reference policy

The soil agent's separate shared rainfall calibration changes maximum source `.00012` to `.000025 m/s`. It is neither introduced nor evaluated as part of this guard. Combined read-only prototype checks against its actual `rain-water-forcing-calibration-20261003` source tree passed `-O0` and `-O2` after normalising only the pinned old reference's rain input/include to the same shared `RainWaterMaximumRainMetresPerSecond`. All old reference edge/sink operations stayed pinned. No benchmark against the calibrated rate was run here.

`Test-RainWaterTransferCache.ps1` now records this distinction explicitly. It first verifies the original pinned header hash, then recognises either the original literal or the exact current shared input expression/include. Only that independent input/include is normalised, and `reference-policy.json` records whether it occurred. Unknown input policy is refused. This compares optimisation outputs under equal forcing; it does not accept the forcing calibration or require the old source rate to survive an intentional physics change. Root must repeat the native proof and installed wet-flight attribution on the final combined policy.

## Rejected alternatives

Reusing `_delta` as free-surface scratch preserved old/current outputs but added head-store/clear traffic. Its first bounded run regressed dry mean 11.6%, flat 2%, and regular steep means 4–6%; favourable checker/sea means had large outliers. Best regular steep current/candidate were 5.2055/5.3844 ms. Production scratch reuse was rejected.

A Configure-sized per-donor ratio cache also preserved outputs and zero per-step allocation, but added about 1 MiB at 513² (4 MiB at MaxCells) and an extra wet-cell pass. Best gentle times were 5.7869/5.7865 ms, best irregular 5.4885/5.9778 ms; no stable useful benefit justified the memory. That production cache was rejected. These results support the smaller local guard candidate, not a claim that the installed rain-flight bottleneck is resolved.
