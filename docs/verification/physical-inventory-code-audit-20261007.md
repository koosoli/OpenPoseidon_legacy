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

Implementation pending committed build and installed smoke results. The existing
native smoke script now also removes a magazine while its ghost is being dragged,
checks that it cannot be recreated, changes actual load during a ground pickup,
checks rejection and then recovers the refused item after unloading.

## Limits

The physical UI remains single-player; multiplayer uses classic Gear. No claim
of network-authoritative physical transfers, exhaustive addon coverage, save/load
coverage or a fault-injected allocation/partial-failure matrix. Grouped paths are
reviewed in source and compiled, not exposed by current classic shell semantics.
The optional 3D preview remains omitted because of the previously diagnosed
HDR/UI render-pass mismatch. Settings parsing and large-inventory performance
would be separate follow-up scopes, rather than an open-ended rewrite here.
