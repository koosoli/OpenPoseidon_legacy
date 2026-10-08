# Optional authored UV0 tangent transport

`WGR_REFORGER_TANGENT_PREVIEW=1` adds decoded authored UV0 tangent axes to the
existing retained ordered-normal preview. It requires the PBR, ordered-layer
and normal preview switches to equal 1. All remain off by default. The existing
normal material suffix filter also controls admission. A fresh process is required.

The importer keeps the render-corner mapping, carries both axes through an
in-memory MLOD sidecar, and includes them in vertex identity before sorting.
Tangent-only seams therefore survive. MLOD disk bytes are unchanged. Ordinary
unskinned two-direction packing, complete source mapping, finite nonzero
independent axes and a complete LOD sidecar are required. Unsupported experimental
packing, malformed/partial frames and excessive sidecar size decline to previous
geometry processing. Aggregate sidecars are capped at 32 MiB per model; the temporary
four-corner record is 112 bytes on the current layout. Existing canonical vertices
and model blobs already contain the axes, so their layout does not grow.

A versioned producer metadata entry plus full-frame validation qualifies the runtime
shape. Old blobs lack this producer mark. The optional producer schema also changes
the derived-cache key, preserving the default key when disabled. A property found
in arbitrary source data cannot qualify an older producer. No blob format bump is
needed because the serialized fields already exist.
Container identity remains `MLOD`, preserving modern adapter, preparation and
simulation admission. Producer version 2 invalidates earlier optional blobs whose
producer marker replaced that container identity. The default cache key is unchanged.

Admission additionally requires the existing four-layer opaque ordered-normal
contract. Retained rendering reads the existing tangent/binormal fields at 36/48
of the 68-byte vertex. It independently transforms and normalizes both axes and the
normal at vertices, then interpolates them without reconstructing the binormal
or making the axes perpendicular in the fragment shader. The normal uses the same
linear transform as the axes. Mirrored transforms keep the independently authored
binormal. Backfaces flip the normal axis only. Singular transforms, terrain-conform
and vegetation instances use the previous normal path. Interpolated axes retain
their lengths through global/detail composition before final normalization.

ABI 24 and its material offsets remain unchanged. No texture samples, descriptors
or backing allocations are added. The default shader has no extra attributes or
varyings. The enabled shader adds two axes, packs existing crown/snow scalars
and combines two flat integer owner proofs. It uses 15 user varyings, leaving
one of the baseline 16 fragment input variables for `FrontFacing`. Colour/prepass and retained
weather-depth builders use matching optional vertex attributes. Direct drawing
keeps its existing material path; this preview does not qualify a direct PBR draw.
Legacy OFP imports and shaders remain on their previous path.

Validation: owned synthetic corner seam, reversed winding, sort, model-blob byte
roundtrip, old-producer decline, invalid mapping, unsupported packing/skinning,
parallel axes and partial sidecars; optional cache-key invalidation; actual C++
production/unit syntax; whole Rust test metadata; actual composed retained/direct/
shadow shaders with PBR off, PBR on and authored on, including the fragment
input limit with builtin deductions. A real device-pipeline test uses the
production varying declaration and fragment signature at 1x/4x MSAA; its extra
varying negative control must fail the 16-variable limit.
A separate production-helper GPU readback test covers nonuniform transforms,
mirroring, independent nonorthogonal axes, singular decline, interpolation and
backface normal orientation. The integrator runs this test and installed fixed-camera
A/B. Native numeric identity of the packed source axes and complete renderer
fidelity are separate acceptance limits, not claims of this preview.

For an A/B, keep PBR, ordered-layer, normal and their material filter fixed; change
only the tangent switch between 0 and 1. Check `Wgpu authored UV0 tangent preview`
reports admission 1 for the visible material, then compare identical camera,
lighting and exposure. An admission 0 run exercises fallback rather than the new
composition. Keep the option off until visual acceptance.
