# Optional ordered detail normals

This preview gives an opaque four-layer material separate global-normal UV and
detail-normal intensity parameters. A small global intensity no longer flattens
the entire detail mixture. Colour, roughness, metalness and AO retain their
existing preview paths.

`WGR_REFORGER_NORMAL_PREVIEW=1` opts in. It requires
`WGR_REFORGER_PBR_PREVIEW=1` and `WGR_REFORGER_LAYER_ORDER_PREVIEW=1`.
`WGR_REFORGER_NORMAL_MATERIAL` optionally restricts admission to a case-insensitive
material-path suffix; use the same suffix for the ordered-layer filter. All
switches remain off by default. A fresh process is required when changing them.

Admission requires the existing standard ordered-mask contract, four enabled
colour/normal pairs, original RG normal bindings, explicit finite detail
strengths in [0,16], explicit global intensity in [0,1], and a separate positive
diagonal global UV transform on UV0. Missing strengths/maps, malformed or rotated
transforms, alternate UV sources, fine masks and unknown author switches decline
the preview. The admission log distinguishes source qualification from ordered
blend availability; failed admission leaves the previous shading path active.

Each detail RG sample is decoded at its effective intensity (authored value plus
0.001) before the ordered interpolation. The blended vector retains its length
until world-space composition. The independently sampled global normal rotates
the detail vector by the shortest rotation from the geometric normal to the
global normal, followed by normalization. This uses the existing derivative
cotangent frame. Authored tangent streams, weather layers, BRDF and lighting
remain separate work; this preview does not establish full material parity.

The append-only public ABI is 24: material size 440 bytes, new UV/intensity fields
at offsets 408/424. The GPU material stride is 416 bytes, fields at 384/400.
Earlier offsets are unchanged. Matching EXE and DLL must be deployed together.
The cost is 32 bytes per section material and vector arithmetic for admitted
fragments; no new texture allocations, descriptors or texture samples.

Validation covers source admission/decline, C++ syntax, full Rust type checking,
public/internal offsets and actual composed retained/direct/shadow shaders with
the PBR preview on and off. The synthetic production-helper readback covers flat
global with visible detail, flat detail with global tilt, strong detail before
mixing, overlapping ordered masks, and a changed frame orientation. Visual
acceptance still requires a fixed-camera comparison with only the normal switch
changed and an admitted material visible.
