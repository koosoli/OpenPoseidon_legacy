# Actual sand geometry GPU acceptance fixtures

`sand_geometry_tests.rs` is a new isolated test module. Root must register it
under `#[cfg(test)] mod sand_geometry_tests;` in the shader module and supply the
production Sand helpers before running it. No production shader or registration
file is changed in this candidate. Rust formatting/source review is complete;
composition and GPU results remain pending root execution.

The tests call actual production `sand_height_offset` and
`sand_height_gradient`, rather than a mirrored model. Conform is imported through
the real Naga composer; its resource group is remapped from four to one only to
fit the default headless device limit. The terrain entry shader cannot be
imported, so the tests extract its complete actual declarations verbatim and
also require its full production composition. Exact shared helper parity and
the actual vertex, colour/prepass normal and conformed support call sites are
checked.

The third packed view starts at `SnowData.deficits[524292]`, followed by four
header floats and a 512-square signed grid starting at 524296. A complete buffer
has at least 786440 deficit floats. The fixture header uses origin 1000/2000,
cell spacing 0.125 and depression bound 0.15. Existing Snow and Mud views hold
nonzero data independently, so incorrect view routing cannot pass as an empty
buffer.

Twelve buffers run through both actual paths: complete legacy Snow/Mud-only,
asymmetric depression averaging -0.10 metres, a +0.02-metre rim, a mixed signed
interpolation, disabled Sand, unsupported cell spacing, an incomplete third
view, a Snow-only change, a Mud-only change, oversized negative and positive
samples testing both clamp bounds, and a negative disabled limit. Every buffer
also probes both sides outside the Sand window. Tests require finite unit
normals, actual support-height movement down and up, nonzero relief normals,
unchanged independent Snow/Mud observables and matching terrain/conform outputs.

The GPU test requires a real headless device and fails if one cannot be acquired;
it has no early success or skip path. Suggested focused root command:
`cargo test -p wgpu_renderer sand_geometry -- --test-threads=1` (use the actual
workspace package name if different). Passing these tests proves bounded
geometry sampling and integration, not stock sand source admission, boot
production, physical Landscape support, wet/dry appearance or installed-game
acceptance. Root owns those independent checks and the authoritative ledger.
