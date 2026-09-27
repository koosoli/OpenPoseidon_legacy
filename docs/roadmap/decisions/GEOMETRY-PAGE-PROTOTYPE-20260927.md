# Bounded offline geometry-page prototype

The pending resident-decoder integration uses builder algorithm2: pages roll over
at256 clusters even when byte capacity remains. Format1 control encoding remains
unchanged, while the cache identity rejects algorithm1 packages. New rollover and
resident-decoding tests are source-only pending the root's next build/check; the
earlier passing checkpoint below does not validate this later change.

The owner explicitly requested implementation of a Nanite-like prototype while
normal streaming work continues. This is the first independent CPU foundation,
not Nanite parity, runtime paging, a simplification hierarchy or acceptance.
It changes no renderer, simulation, default or source asset. Focused tests are
registered in the existing unit-test target.

`GeometryPagePrototype.hpp` accepts borrowed original interleaved vertex records,
positions, indexed triangles and one material ID per triangle for two authored
representations: complete coarse fallback and fine detail. The caller must select
one static opaque pilot and certify those inputs originate from its mounted
source and materials. A caller boolean is not an engine eligibility certificate.
The caller must prove position[i] is the exact rendered vertex position in raw
vertex record i in the same coordinate basis. There is no opaque vertex decoder
here; computed bounds apply to the supplied position table only, and cannot
certify a mismatched shader/layout position. The focused fixture stores real
float positions in its records and distinct extra attributes at coincident seams.
Animated/skinned objects, doors, vehicles, destruction, cutouts, translucent
surfaces and terrain are outside this prototype. No copyrighted assets are copied.

The builder preserves complete vertex bytes, original vertex IDs and triangle
order. Clusters never cross material runs; coincident vertices with different UVs
or normals remain distinct. Bounds expand outwards by one float step and refuse
nonfinite results. Default clusters cap64 vertices/124 triangles. Separate coarse
and fine pages hold multiple clusters up to64KiB/page; hard maxima are4096pages,
65536clusters,128MiB serialized payload,1Mvertices/2Mtriangles per representation
and128MiB borrowed vertex bytes per representation. Configured limits cannot
raise those hard maxima. One failed input/allocation/cap leaves output unchanged.

Page format v1 control fields are explicit little endian, uncompressed and disposable. Its header
and cluster records include lengths, stride, material, original triangle range,
bounds, original vertex IDs, unchanged vertex bytes and local indices. Opaque
vertex payloads retain their original encoding, not an imposed endian conversion.
Package cache identity includes full caller-supplied source SHA256, producer,
format and builder algorithm versions, geometry/material options, explicit
coarse/fine representation selection, vertex layout/material mapping and effective
cluster vertex/triangle limits plus page byte size. The source/options contract
must identify the complete two inputs. Refusal ceilings do not affect successful
packing and are not cache content keys. There is no disk-cache reader/writer or corrupt-input parser yet;
do not feed untrusted serialized pages directly to rendering. Allocation capacity,
metadata and caller memory are distinct from actual serialized `payloadBytes`.
No CPU collision/fire/view/path/memory LOD is modified or owned by this data.

The first safe frontier is deliberately whole authored representation selection:
all coarse pages must remain resident, and fine replaces coarse only when every
fine page is resident. Otherwise draw coarse alone; never partial fine pages,
parent plus child overlap or holes. This is a useful fallback contract, but it
does not provide hierarchical error refinement. The residency snapshot carries
the exact package cache identity and checks it before selection; bitmap size and
source/options/format/algorithm/packing mismatches return Unavailable. It must
belong to this immutable Build-produced package; asynchronous runtime request
epochs and page leases are not provided here. Caller-supplied coarse geometry
has no automatically certified silhouette/error correspondence with fine geometry.

Focused `[geometry-page-prototype]` tests reconstruct every triangle, material and
vertex attribute byte; check bounds, seams, deterministic packing, degenerate
indices, page/cluster/payload limits, invalid indices/stride/materials/nonfinite
positions and transactional failure/frontier behavior. The first integrated build
failed because the test helper name collided with the engine's `Verify` macro;
00220cd0 renames that helper. The repaired five-target build passed, and the
combined geometry-page/lifetime/ledger suite passed 15,266 assertions in 35 cases
without skips. This proves the CPU foundation contracts; no runtime page-rendering
evidence exists.

Next bounded integration: identify a genuinely static opaque authored coarse/fine
pilot through the actual adapter, preserve mounted source/options identity, add a
validated versioned derived-cache reader, then resident-only conventional WGPU
page rendering with identical depth/shadow/material behavior. Only afterward add
asynchronous requests/cancellation and fence-safe release with independent CPU
simulation. A hierarchy requires validated simplification/error boundaries;
independent meshlet decimation is not a substitute. Performance remains gated by
matched conventional baseline quality, cold loading, frame tails, all-pass demand
and actual allocation/staging/in-flight accounting. Defaults remain unchanged.
