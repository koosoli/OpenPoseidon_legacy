# Coarse runoff transfer cache: measured source and equivalence

## Actual installed attribution

The `e017e028` two-arm campaign
`combat-audit/build/rain-helicopter/occupied-solver-staged-look-20261003-140313-8ffcd5`
passes with normal zero exits and an unchanged installed EXE/DLL pair. This is
the first accepted matched flight run containing the nested native attribution
from `RAIN-FLIGHT-CLOUDLET-ATTRIBUTION-20261003.md`. The earlier failed staging
campaign is not used as paired evidence.

The native log rows below fall between each arm's first and last actual sample
wall UTC timestamp. Engine log timestamps use local Luxembourg time (UTC+2 on
this date). Dry window: 12:04:16.967702–12:04:41.145554 UTC; rain window:
12:05:46.276724–12:06:10.486567 UTC. Each receipt is the existing approximately
one-second native report window; its boundary may include up to a report window
of preceding work. These physics ticks are not rendered frame IDs.

| Actual scope | Dry ms/physics tick | Rain ms/physics tick | Dry maximum call ms | Rain maximum call ms |
| --- | ---: | ---: | ---: | ---: |
| Existing Cloudlets container | 0.378272 | 0.861864 | not previously recorded | not previously recorded |
| Soft surfaces | 0.000987 | 0.000990 | 0.0132 | 0.0267 |
| Runoff source | 0.000517 | 0.000515 | 0.0018 | 0.0097 |
| Actual runoff Advance | 0.375442 | 0.859199 | 8.8046 | 16.0627 |
| Natural-source update | 0.000165 | 0.000155 | 0.0010 | 0.0009 |
| Remaining cloudlet effects | 0.000274 | 0.000205 | 0.0482 | 0.0076 |

There are 24 receipts per scope in each arm: dry 1447 physics ticks and 1447
actual Advance calls, with 543.2639 ms total Advance; rain 1448 ticks/calls,
with 1244.1201 ms total Advance. Weight totals by actual tick counts instead of
averaging the last log row. Advance accounts for about 99% of the Cloudlets
container, and essentially all its rain increase of about 0.4838 ms/tick. The
scope includes cheap no-solver-step calls and the periodic heavy solver calls.
The source domain remains the actual 513 by 513 coarse field, spacing 25 m;
fine support is off.

The genuine completed frame capture is dry 2631 frames, mean 9.4354 ms,
median 9.559, p95 11.441, maximum 30.288; rain 2304 frames, mean 10.7749 ms,
median 10.8385, p95 15.105, maximum 33.746. Actual rain is 0.777442–0.855593;
dry uses the earlier documented tiny positive ordinary native transition.
These measurements justify targeting actual coarse solver work. They do not
predict a cache improvement, make CPU/GPU containers additive, or establish an
improvement over a different campaign with different rainfall/history.

## Bounded production change

Only `RainWaterField.hpp` changes. In the actual coarse fixed step, the first
edge pass computes `Transfer(a,b)` and accumulates total outgoing demand. The
second edge pass previously recomputed the identical raw transfer; neither bed
nor depth is modified between the passes, and Transfer reads neither `_out`
nor any flow/delta values being written. The second pass now reads that first
raw float from two Configure-sized axis vectors.

The existing `Edges` traversal retains exactly the same row/cell order and
interleaves each X edge before that cell's Z edge. The axis counters follow
this traversal on both passes: X has `(width-1)*height` entries and Z has
`width*(height-1)`. They reset every fixed step; every actual edge overwrites
its own raw value before read. No value survives as authority across steps,
domain changes, bed edits, rain changes or flow writes. The donor selection,
limiter, signed deltas, flow calculations, final depth update, outlet behavior,
fixed-step scheduling, budgets and revision operations retain their order and
arithmetic. The fine solver and source/render admission are unchanged.

The 513-square domain adds 2101248 bytes (about 2.004 MiB) of float payload,
plus two vector objects and allocator alignment overhead. Each axis contains
fewer floats than the existing MaxCells limit. The tested 1024-square maximum
adds 8380416 payload bytes; observed Windows allocation requests include 78
additional alignment bytes and remain below 8388608 bytes. There are exactly
two additional Configure allocations and no new Advance/Step allocations.
Reset/domain Configure destroys the old scratch; copies own their vectors.
RainWaterField has no StorageBytes reporter. The separate RainWaterFine
StorageBytes reporter still measures its own unchanged standalone storage;
it is not a report of the containing field's legacy arrays or scratch.

## Verification and remaining acceptance

`scripts/Test-RainWaterTransferCache.ps1` generates a second actual class from
the pinned pre-change source at
`e017e028812dcd7ce050497fd40bf81253a1bcf1`. Its normalized-LF SHA256 is
`4BB54A8046F2D7E7C287EDC68F75F5F4582E097C744791CCBE188B43B77216A7`.
Only class/singleton identifiers change for compilation; the reference is not
a hand-written replacement solver and adds no production test toggle.

The actual candidate and actual reference have bitwise-identical snapshots
(bed+depth, depth, X/Z flow), budgets, volume, pending time, generation and
revision after every compared operation at both `-O0` and `-O2`. Coverage
includes flat, slope, checker, hollow, sea outlet and extreme finite beds; dry,
tiny dry-transition rain, rainfall, sunlight/wind drying; terrain edits;
rectangular regrids, Reset, 16-step backlog and 60 Hz partitions; invalid time,
rain, bed and domain guards; and the full actual 1024-square MaxCells limit.
An allocation observer checks Configure scratch size and zero Advance/Step
allocations without private access or a replaced production allocator.

The existing `Test-RainWaterField.ps1` also passes its actual hollow/spill,
flow, sea outlet, drainage, sunlight, pause, budget, reset and partition tests,
plus actual fine-source lifetime, queued-edit, corner/query, stale closure,
gap/overflow and mass-preserving regrid checks. `git diff --check` passes.

The full engine build and installed matched two-arm flight comparison remain
pending. Integration acceptance must retain actual rainfall/domain/budget and
route guards, inspect both solver mean and maximum calls, and check completed
frame timing. This source candidate is an equivalence-tested optimization;
no installed FPS gain is claimed yet.
