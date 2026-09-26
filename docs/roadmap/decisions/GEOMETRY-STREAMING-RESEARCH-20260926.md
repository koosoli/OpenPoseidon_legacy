# Asset geometry streaming: staged recommendation

Owner requested a subagent investigation while the streaming plan continues.
Local review baseline: `codex/streaming-residency`, `1e8f04da`; allocation
diagnostic implementation: `abfd4131`. This is research, not a paging or
performance acceptance claim. Tidewater water work remains parked separately.

Nanite combines hierarchical triangle clusters, view-dependent refinement,
compressed streaming and a specialized rendering path. Copying only its
distance-based selection would not provide that system.
[Epic overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine),
[public technical presentation](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf).

Our retained path already selects LODs and performs GPU visibility compaction.
`EngineWgpu::RegisterGpuModel` nevertheless registers drawable LOD geometry
up front; selecting a coarse LOD does not release unused detail. The first
useful experiment is therefore conventional per-LOD residency, preserving a
resident coarse fallback, before introducing cluster hierarchies or disk pages.

Use the bounded allocation report to identify expensive opaque models first.
Its rows describe allocated mesh ranges, not current visibility, reclaimable
bytes, a complete asset ranking or the complete corpus. Shared mesh handles
must remain deduplicated. Existing limited DayZ diagnostic and convergence
results do not yet justify enabling geometry paging by default.

Suggested sequence:

1. Measure allocation and selected LOD demand on the same route and coverage.
2. Prototype resident coarse LOD plus bounded asynchronous detail admission for
   a few high-cost opaque rocks or building shells.
3. Prove cancellation, model generations, submission lifetime and delayed GPU
   release before permitting eviction. Never free geometry referenced by an
   in-flight frame; keep the fallback valid during failed or superseded loads.
4. Only if these measurements justify it, build offline clusters, error-bounded
   simplification and a hierarchy, initially fully resident. Then add bounded
   page requests and compressed derived data with explicit dependency keys.

`meshoptimizer` offers meshlet construction, bounds, simplification and encoding
building blocks; it does not provide our residency scheduler or lifetime model.
[Project documentation](https://github.com/zeux/meshoptimizer).

The initial exclusions—alpha foliage, skinning, moving doors and destructible
topology—are our engineering scope, not claims about current Nanite limits.
CPU collision, AI, navigation, destruction and authoritative object identity
must remain independent of render-detail availability. Compare traversal
frame times, allocation convergence, reload churn and equivalent silhouettes;
an object-count reduction or lower instantaneous VRAM is insufficient.

No Unreal integration, Nanite parity or production geometry paging is reported.
Keep the experiment opt-in until its correctness and performance gates pass.
