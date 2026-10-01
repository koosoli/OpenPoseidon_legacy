# Physical shelter beyond the near weather map

Status: source candidate, far cascade **off by default**. GPU, installed visual,
device validation and performance gates remain pending. No all-map/all-forest
acceptance is implied.

The installed `50db754c` Eden forest campaign
`build/forest-snow/stronger-neutral-frost-20261001-022006-76513f` showed a white
audited target with a green rear forest band at its 500m camera. The camera was
`5025 3725 144.54546 0 -8.53076561`; the existing camera-centred 640m halfextent
physical map ends at Z4364.0625 including its tap border. Immutable stock
`Worlds/eden.wrp` inspection found already audited forest families beyond it:
owners4040/1015 at Z4377.1/4373.3,4039/1051 at Z4427.1/4423.3, and4038 at
`5025 100.2 4475` using the same triangle model as the target. Those fragments
cannot receive physical snow proof from the sole near map. Pixel identity of the
whole rear band was not asserted. All four visual resolutions of the selected
target registered their audited atlas categories; there was no demonstrated
generic far-LOD material omission.

`WGR_WEATHER_COVER_FAR=1` opts into a second 2048² Depth32Float map with 1600m
halfextent. The existing 640m map keeps its 0.625m texels; the far map has 1.5625m
texels. Both have the same 8cm physical comparison bias, current posed/wind/conform
geometry and authored cutout discard. Far is selected strictly outside the
planned near footprint. Pending/overflowed near proof does not borrow a coarser
far result inside that footprint. Far readiness, frame/image/instance identity,
cull recording, actual opaque/cutout/reservation counters, depth recording,
submission and abort are independent. Missing resources, skipped direct sources,
overflow, depth bounds and tap borders remain closed.

The private camera group adds binding20 and 96 private Frame bytes; the public
WgrCamera and C++ ABI are unchanged. Separate near/far depth shader overrides
replay the same current source slots without changing alpha, geometry, LOD or
atlas admission. Terrain rain, raised roof snow and admitted forest crowns use
the same physical selection; forest trunks/skeletons/ground/unknown tuples stay
outside their existing source crown policy. Artistic AO layers are unchanged.
`WGR_WEATHER_COVER=0` disables both physical cascades.

Cost when enabled: another 16MiB depth target, one actual retained cull view and
depth pass, associated indirect buffers, and replay of current direct sources.
There is no CPU readback or all-world dynamic geometry proof. A 1600m square
halfextent covers the evidenced rear forest rows, but not every axis-aligned
point at a configured 2000m draw distance. No cost/runtime improvement is claimed.

Source verification: Rust check with tests; composed actual direct/retained WGSL
and Naga Frame tail offsets; near/far absolute/relative view, audited rear-row,
8cm depth-bias, bounded footprint and exact opt-in tests; independent publication
and existing camera-tail tests. The real GPU weather fixture now includes far
800/1200m top/under/open/discard-hole cases, pending and per-cascade overflow,
near priority, far border/outside. Those added GPU cases are authored, not yet run.

Required installed gates before enabling: matched binary pair, no validation
errors, explicit far flag and ready/current VP traces at each camera; Eden500
rear-band and top views with normal exposure; retained/direct alpha holes and
covered/removed/thin/partial roof controls near200/500 and beyond640; lower trunks,
unknown atlas and terrain negatives; camera return and temporal stability; actual
memory/GPU cost. `WGR_WEATHER_COVER_TRACE=1` now reports each changed snapped
per-camera map matrix as well as initial publication rows, so startup readiness
cannot be mistaken for readiness at a later 500m camera.
