# Fine rainwater paired renderer handoff

Status: C++/FFI producer candidate only, CPU checks passed. Matching Rust lib
dispatch/receiver/shader integration is required before a combined build or
deployment. The fine simulation remains opt-in (`POSEIDON_RAIN_WATER_FINE=1`);
the ordinary non-fine renderer path remains the existing grid setter. No GPU,
game, engine build or deployment was performed for this candidate.

## Exact ABI contract

The additive calls are `wgr_rain_water_set_source(renderer,key)` and
`wgr_rain_water_set_publication(renderer,publication,coarse,coarse_count,fine,fine_count)`.
ABI version is 19 for the new entry points; all three new type sizes enter both
layout hashes in matching order. The existing ocean and coarse-params layouts
remain unchanged.

| Type | Size | Fields |
| --- | --- | --- |
| WgrRainWaterSourceKey | 32 B | u64 world_token, generation, height_revision; u32 terrain_range; f32 terrain_spacing |
| WgrRainWaterFineCell | 64 B | vec4 rect, corners, flow_depth; u32 parent, reserved[3] |
| WgrRainWaterPublication | 96 B | old 48 B WgrRainWaterParams coarse; SourceKey source; u64 revision; u32 flags, reserved |

`world_token` is opaque actual Landscape identity, never dereferenced in Rust.
Generation is the globally reset-safe field generation and changes on each new
ownership tile. Source revision is the actual committed bare terrain revision.
Flags are FINE_BACKEND=1, SOURCE_READY=2, FINE_READY=4. Reserved lanes are zero.
The maximum is 8192 fine records, not an arbitrary renderer count.

Fine rect is `[lowerWorldX,lowerWorldZ,6.25,horizontalHead]`; corners are the
actual source heights `[00,X1,Z1,11]` with the antidiagonal triangles. Flow/depth
is `[flowX,flowZ,volume/area,0]`. Average depth is NOT fragment-local depth.
`parent` is the old coarse control-owner/node index `z*coarseWidth+x`, not a
native terrain cell. Its node is a centre: physical lower bound is node position
minus half coarse spacing (12.5 m). Each parent publishes all 16 children.
Each admitted complete 200 m tile publishes all 1024 cells, INCLUDING dry cells;
64 coarse parents x16 children, at most eight tiles. Records are in ascending
global parent order, then child `z*4+x`; they are not row-major across a tile.

The producer's packing helper rejects nonfinite float conversion, invalid
geometry, wrong size, negative volume/depth, bad parent index and over-cap data.
The consumer must additionally prove complete unique child coverage, source
coordinate alignment, real interior bounds and at most eight complete tiles.
Exclusion masks are exact physical world rectangles. A parent-index-only mask
applied to the old grid's vertex interpolation is insufficient.

## Actual terrain receipt and paired queue

TerrainWgpu issues the source receipt immediately AFTER its actual matching
bare-heightmap upload in the existing exclusive producer window. A new fine
lifetime/ownership generation forces a real upload even when ordinary cached
source numbers match. The receipt is not relabelled from an old upload. Normal
unchanged source/generation performs no repeated upload/receipt. Null source
height data publishes an invalid zero receipt. The C++ producer's readiness
check compares owner, generation, range, spacing and last uploaded revision.
The Rust receipt must separately require its actual accepted heightmap resource;
a C++ call alone does not override a failed GPU resource upload.

WaterWgpu prepares a paired publication only for FineActive. It additionally
requires simulation SourceReady and an exact matching TerrainWgpu upload
receipt. Stale/pending/incoherent source disables the ENTIRE pair. Healthy zero
fine tiles after conservative regrid can use the paired coarse backend with
FINE_READY clear; this is not a failed fine-renderer fallback.

EngineWgpu's overlap-safe pending settings keep coarse and fine arrays together.
An exact key covers source, generation, revision, domain origin/spacing,
dimensions, flags, enabled and reserved lanes. Time/current rain may change
without replacing physical state. A parameter-only second camera preserves
both queued arrays only for that exact key. A changed key, partial payload or
stale/disabled state never splices one fresh slice with another old slice.
After the drain, unchanged empty slices require a complete identical receiver
cache; otherwise the receiver must reject/clear the pair. Bad FFI slice counts
or null nonempty pointers invoke a disabled paired publication, without reading
the bad slices or leaving prior water drawable.

The existing non-fine setter must clear any prior fine receiver state. A
fineBackend publication with an unavailable/disabled fine renderer must fail
closed as a whole; forwarding its unmasked aggregate coarse proxies to the
legacy grid would misrepresent ownership. All existing ocean/wake/downwash
snapshot and draw sequences remain separate.

## Root consumer integration and remaining gates

The new FFI dispatch calls require these Rust renderer methods:
`rain_water_set_source(WgrRainWaterSourceKey)` and
`rain_water_set_publication(WgrRainWaterPublication, &[WgrVec4], &[WgrRainWaterFineCell])`.
This candidate deliberately supplies no stub or old-renderer fallback; lib.rs,
rain_water.rs and rain_water.wgsl remain assigned to root/consumer owner. Thus
standalone full cargo/engine compilation is pending their matched integration.
The new ABI must be built/deployed as a matched engine/DLL pair.

Receiver must atomically validate/retain coarse and fine bytes and the accepted
terrain receipt. Source changes invalidate the drawable pair. Fine raster uses
horizontal head with actual triangle clipping and strictly positive local depth;
coarse rendering excludes complete physical tile rectangles even where fine
cells are dry. Preserve raised support, roof exposure, sea, snow, physical
optics/reflections, near-camera bounds and the existing fragment resource budget.

`Test-RainWaterPublication.ps1` compiles the production packing/queue helper and
actual C++ ABI header with C++20 Wall/Wextra/Werror, testing actual source-cell
packing, corner order, physical rect/parent identity, exact-key camera retention,
changed source/lifetime/layout refusal, missing coarse slice, stale clear,
nonfinite/wrong-sized/bad-parent/overflow rejection. It also extracts the actual
Rust repr(C) definitions and their actual size/offset assertions, removes only
the external proc-macro derive, and compiles/runs them with rustc -Dwarnings.
Both pass. rustfmt parse of the actual FFI file passes; it was not reformatted.
These checks do not stand in for full Rust type-checking or engine compilation.

At cap the ABI fine payload is 8192x64=512 KiB, plus the existing coarse RGBA
payload and copies; source producer CPU surfaces remain 104 B each. Cold full
heightmap upload on new fine ownership is explicit cost. Root still needs real
device layout/raster/stale/resize/disable tests, full engine build, installed
actual crater edit/restore captures and cold/admitted/steady CPU-GPU timing.
No default activation or visible crater-water acceptance is claimed here.
