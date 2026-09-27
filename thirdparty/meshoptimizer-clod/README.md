# Pinned CPU CLOD dependency

Unmodified MIT-licensed upstream sources from
[`zeux/meshoptimizer@9e1f07b159d3cb777f1c67ed31fc11fd117986f4`](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4).
`PIN.json` records each original Git blob ID and downloaded SHA-256. Keep
`LICENSE.md` and the copyright/licence notices in `demo/clusterlod.h` when
redistributing. `clusterlod.cpp` is our single implementation TU, not an
upstream modification. There is no NVIDIA SDK or Vulkan sample code here.

`cmake/MeshoptimizerClod.cmake` defines the CPU-only static target
`OpenPoseidon::MeshoptimizerClod`, excluded from the default build unless a
consumer links it. It is not included by the existing project yet. The seven
upstream implementation files cover clustering, meshlet bounds/local index
optimization, partitioning, position remapping, simplification and spatial
ordering, plus their allocator. This is deliberately a CLOD subset, not the
complete public meshoptimizer library: unrelated header declarations for codecs,
remeshing, general cache optimization, etc. are not supplied by this target.
No external libraries, renderer objects, shaders, asset loaders or nested
worker pool are introduced. Compilation/link completeness remains for root's
controlled build to verify; this source preparation did not run one.

The example's `clodBuild` returns void. Its output callback returns a group ID
used by `clodCluster::refined`, **not** a cancellation signal. Callback indices
and cluster arrays are borrowed during the call; a bake consumer must copy its
bounded outputs. Temporary `std::vector` storage exists outside meshoptimizer's
allocator hook, so changing that hook does not enforce a whole-bake budget.
Keep input limits and transactional output refusal; do not claim prompt
cancellation or an enforced peak-memory ceiling until independently established.

The upstream defaults also support permissive/sloppy simplification and border
dilation; the latter mutates positions despite the input pointer's const type.
Our initial static-shell pilot must explicitly disable these paths. Preserve
material/UV/normal seams, use the paired group-error DAG selection conditions,
and retain original authored visual and query LOD fallback. This dependency
alone supplies no paging, Ready, source freshness, ownership or performance
proof.
