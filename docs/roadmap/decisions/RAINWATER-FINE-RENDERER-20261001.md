# Native fine rainfall rendering

Source-only receiver/shader candidate. Requires the paired ABI19 producer
`35ad3083` and root's thin lib dispatch/accepted-heightmap receipt gate. Actual
device tests, combined compilation, deployment and installed natural-rain/crater
quality acceptance remain pending. No GPU, game or full build ran for this commit.
The existing coarse physical-water controls are retained; their visible angular
bands are not newly accepted as attractive water.

`POSEIDON_RAIN_WATER_FINE=1` opts into simulation ownership;
`WGR_RAIN_WATER_FINE=1` separately opts into this renderer. Both are needed.
A fine-backend publication received while its renderer is disabled clears the
whole draw pair rather than drawing unmasked coarse volume proxies. Ordinary
legacy grid calls continue their existing parameter-only coarse cache and clear
any prior fine/source lifetime.

`RainWater::set_source` receives the actual terrain source identity.
`invalidate_source` closes both draws immediately on a heightmap/source drop.
`set_publication(device,queue,publication,coarse,fine)` requires the exact world
token, ownership generation, accepted bare-height revision, native vertex range
and finite spacing, matching generation/layout, known ready flags and zero
reserved lanes. The root dispatcher must only deliver a valid source receipt
after its real matching heightmap resource is accepted. A C++ receipt alone
cannot make a failed GPU upload valid. Missing/stale/mismatched/partial pairs
clear both draw caches. Both empty slices reuse a complete prior pair only under
its exact source/layout/revision/flag/enabled identity; time/current rain may
update without replacing physical state.

Flags 3 with complete coarse data and zero fine owners are a healthy aligned
coarse-only state after conservative regrid. Flags 7 require 1024..8192 fine
records, including dry children, with at most eight complete tiles. A missing
fine list under FINE_READY, or any nonempty list without it, is refused.
Generation and height revision zero are legitimate when actually matched; no
invented nonzero counter rule substitutes for source identity.

For nonempty fine ownership, native spacing is 6.25m and coarse spacing 25m.
Actual range is a heightmap vertex count, so coarse dimensions are
`ceil((range-1)/4)+1`. Each old coarse node owns the square centred on its world
position; its lower bound is node minus 12.5m. Every admitted 200m tile contains
64 unique parents, each with all sixteen unique 6.25m children. CPU validation
checks finite geometry/flow, parent range, exact child alignment, complete
ownership, dry records, zero reserved values and native `[0,(range-1)*spacing]`
interior bounds. Negative-halo tile zero, incomplete borders, duplicate children,
sparse wet-only lists and over-cap tiles remain closed.

The receiver derives eight exact physical world rectangles. Wholly interior
coarse quads are omitted cheaply; partial boundary quads retain their geometry
and a bounded eight-rectangle fragment test. Coarse fragments inside any complete
owned tile are discarded, including dry areas. The old node-index/vertex domain
is not substituted for these physical bounds, so fine water is never an overlay
that doubles opacity over an admitted coarse body.

Fine data uses three RGBA32Float texels per cell: rect/head, native corners and
flow/average depth. A fixed 1024x24 texture holds the cap in 393,216 bytes, with a
144-byte eight-tile uniform and at most 32KiB of fine instance indices. This
private texture capacity is allocated even with fine rendering disabled; no
default-zero resource-cost claim is made. The renderer adds no fragment storage
buffer or ocean/SSR resource ABI. Camera selection visits at most 8192 retained
records and stays within the existing 600m presentation range. It does not alter
ownership, simulation volume or source lifetime.

The actual `vs_fine` submits horizontal head geometry. The real fragment samples
each native anti-diagonal triangle from the published `[00,X1,Z1,11]` corners,
computes `max(head-bed,0)` and clips strictly positive local depth. Average
volume/area is not used as fragment depth. The existing actual bare-map validity
and 15cm native support disagreement check, physical roof exposure, snow, sea,
underside, optics, filtered rain/flow and private same-frame reflections remain
in force. Fine base normals are horizontal; slope-derived coarse head normals
are not borrowed into a true horizontal pool. This repairs coarse slope/bed
interpolation as a water-geometry mechanism, without bypassing terrain guards
or promising smooth shoreline geometry beyond the authored native triangles.

Authored verification for root execution:

- CPU production-policy cases cover complete dry-inclusive tile union/origin,
  eight-tile cap, duplicate/partial/misaligned ownership, invalid border/domain/
  range/nonfinite data, healthy zero owners and native anti-diagonal saddle depth.
- A composed Naga check verifies actual 144-byte private uniform offsets and
  both real vertex entry points.
- The existing full production GPU raster fixture retains coarse flat/sloped/Cw
  controls and adds actual native fine flat, crater and sloping shoreline draws,
  dry-owned coarse exclusion, roof, snow, underside and sea negatives. Combined
  vs fine-only byte comparison rejects double blending; source/cache/drop and
  disabled fine-renderer cases reject stale fallback.
- An actual production-helper GPU probe checks lower-inclusive/upper-exclusive
  physical tile edges and the native saddle's wet/dry triangle depths under a
  constant head, independently of average depth.

Rust parsing and whitespace checks passed locally. These authored CPU/Naga/GPU
cases have not been executed by this source agent. Installed closure still
needs real matching terrain receipts, fine owner counts, source revision after
actual crater edit/restore, natural rain accumulation and draining, normal exit,
retained mask boundaries/negatives, ground-visible optics and serial performance
measurements. No all-map coverage, nice-water appearance or shipping status is
inferred from packing and shader source alone.
