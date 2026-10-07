# Physical inventory code audit — 2026-10-07

Scope: donated inventory model, UI, item mass/footprint integration and current
single-player transfer paths. Bounded correctness review, not a full engine audit.
Two independent read-only reviews covered transfers and UI/lifetimes.

## Findings and changes

- **Magazine lifetime (high):** displayed/dragged cells held raw magazine pointers.
  A mission script can remove the last live reference while the UI still draws its
  snapshot. Cells now retain `Ref<const Magazine>` references, including grouped
  cells. Transfers still require live source ownership, so a retained stale tile
  cannot recreate removed gear.
- **Stale capacity (medium):** pickups used cached weight/volume while refresh is
  deliberately suspended during a drag. They now calculate current carried mass,
  volume and capacity at the actual transfer check.
- **Grouped transfers (medium, latent):** person transfers moved only the box's
  representative magazine. Cargo transfers could accept part of a box and then
  report failure without refreshing. Giving now processes the whole group, cargo
  checks capacity for the entire group before mutation, and failed additions roll
  back before the player loses anything. Classic OFP shell grouping is currently
  disabled (`IsShotgunShellMag` returns false); these are defensive fixes for the
  retained grouped-item paths, not a reproduced stock-game box bug.
- **Hotkey timing (medium):** throwable selection explicitly erased cooldown and
  reload timers. It now preserves loaded-item timers and uses the normal timed
  magazine swap when selecting a different carried throwable.
- **Frame-dependent UI timing (low):** hover, toast and periodic refresh advanced
  by a hard-coded 20 ms per frame; the refresh accumulator was also shared across
  display instances. They now use the UI clock and per-display state.
- **Preloader progress (low):** budget-boundary returns repeated the last icon on
  the next call; a budget of one could stall indefinitely. Indices now advance
  before yielding.
- Updated obsolete header descriptions that claimed weight-only storage and an
  active 3D preview. No unrelated gameplay systems were refactored.

## Verification

Source commits bc1862b5 and 83072732 built successfully. Matched EXE/DLL installed
from 83072732 at 19:45 local October 7. Script-only fixture refinements badd516d
and d3009a10 did not require rebuilding those unchanged binaries.

Installed native smoke `20261007-195124-710` passed:

- O/K/open/settings/close/reopen, grenade drop and pickup with item conservation.
- All 17 installed icon hashes match the versioned manifest.
- Remove the exact magazine while its drag ghost is being drawn; no crash,
  recreated magazine or duplicated ground item after release/reopen/pickup attempt.
- While dragging a ground grenade, use ordinary SQF to equip five AT mines,
  an M16 and a Carl Gustaf (65.1 kg, above the 60 kg limit). Release refuses the
  pickup, preserves the load, and leaves the grenade recoverable after unloading.
- Normal process exit; no unhandled exception, GPU validation error, device loss
  or renderer panic in the checked engine/stderr logs. Captures 07/08/09/10 were
  inspected. Displayed load remains a periodic snapshot; transfer validation is
  live even before that display refreshes.

Receipt: [installed SP audit](physical-inventory-code-audit-20261007-runtime.json).
The two earlier runs stopped at test-fixture assertions: ordinary OFP SQF accepts
five AT mines, and M60 occupies the secondary slot too. Those setup failures were
corrected in the test script; they were not product failures or hidden passes.
Live volume checking and grouped rollback are source-reviewed/compiled; this
particular runtime regression exercises the weight gate.

Installed private hosted-MP smoke `20261007-195316-618` also passed: after entering
the network world, developer harness evaluation is rejected, read-only ping and
clean exit remain available, and the checked logs contain no runtime/GPU failure.
Receipt: [MP guard](physical-inventory-code-audit-mp-20261007-runtime.json).
This checks the guard, not a complete multiplayer inventory/mission session.

## Limits

The physical UI remains single-player; multiplayer uses classic Gear. No claim
of network-authoritative physical transfers, exhaustive addon coverage, save/load
coverage or a fault-injected allocation/partial-failure matrix. Grouped paths are
reviewed in source and compiled, not exposed by current classic shell semantics.
The optional 3D preview remains omitted because of the previously diagnosed
HDR/UI render-pass mismatch. Settings parsing and large-inventory performance
would be separate follow-up scopes, rather than an open-ended rewrite here.
