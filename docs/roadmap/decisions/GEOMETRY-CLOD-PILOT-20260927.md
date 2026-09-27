# Controlled resident CPU CLOD pilot

This checkpoint implements source for an offline bake and resident full-DAG-cut
reference. It is pending build, automated checks and runtime integration. It
changes no defaults, game geometry, streaming workers or renderer submission.

The dependency is upstream meshoptimizer at
[`9e1f07b159d3cb777f1c67ed31fc11fd117986f4`](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4),
including its `demo/clusterlod.h` example. The minimal CPU dependency retains its
MIT notices. It imports no NVIDIA renderer, RTX SDK, Vulkan sample or assets.

`GeometryPageClodBake.hpp` accepts the immutable fine representation of a
controlled rigid, single-material `ShapeExport`. This caller certificate excludes
animation, skinning, forest/conformance, mutable proxies, destruction and unknown
material semantics. It is not arbitrary retail-model eligibility proof. Positions
must match the exact packed `SVertex` records. Normal and both UV attributes are
passed to the simplifier; all coincident original vertex IDs are locked. Output
indices reference the original unchanged attribute buffer. Permissive, fallback
permissive/sloppy, dilation, regularization and error-clamping policies are disabled.

The fixed policy, weights, locks, supported numeric domain and validation rules
belong to `AdapterVersion=1`; changes require a version bump before reuse. A future
cache key must also identify the exact library revision, immutable source witness,
selected representation, vertex layout and material mapping. No disk cache format
or serialized-data parser is implemented by this checkpoint.

Input caps are 4096 vertices and 8192 triangles, with finite local coordinates and
simplification attributes bounded to absolute value 1,000,000. Captured output is
limited to 4096 groups, 16384 clusters, 1,048,576 index entries and 8MiB logical stored
contents. Each cluster is limited to 64 unique vertices and 128 triangles. These
limits exclude vector capacity, allocator metadata and external library scratch.
One upstream call can allocate its own vectors and run without an interruptible
budget. The callback returns group IDs; refusal stops output copying and discards
the transaction after the call, and does not cancel the bake. This is unsuitable
for a live worker/owner-frame integration until separate resource and lifecycle
gates are implemented.

The adapter stores paired containing-group/refined-group IDs, group bounds and
simplification errors. It validates finite bounds/errors, prior-group references,
monotonic group errors, terminal roots and exact winding-preserving original-leaf
triangle coverage before publishing. Cluster-bound errors may be nonmonotonic and
are not selection criteria. The resident reference cut uses both group predicates
and replaces entire refinement groups. `SelectClodCut` requires an immutable,
successful `BakeClodPilot` result; its topology checks do not independently certify
mutated index payloads or externally constructed geometry.

Focused `[geometry-clod-bake]` source tests exercise a real indexed planar grid,
original attributes and seam IDs, refinement output, exact leaf coverage,
transactional input/output refusals, malformed topology and a hand-constructed
paired-error cut. They have not yet been executed at this pending checkpoint.

The next renderer gate is a resident conventional indexed-draw pilot through the
existing resource queue, with original authored/query geometry retained. It must
prove complete cuts, material/attribute equivalence and consistent required passes
before disk backing, asynchronous requests, eviction/refill or pressure handling.
Draw counts, actual allocation/staging/in-flight bytes, loading and frame tails
need matched quality/work measurements. No performance acceptance, physical
reclamation, Nanite equivalence or completion of the streaming plan is claimed.
