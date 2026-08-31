#pragma once

// Presentation-snapshot ring counters (roadmap Phase 5 step 5, and the 5.3 bullet
// "visible counters for snapshot age, queue depth and renderer wait time").
//
// Phase 5.3 forbids an unbounded producer/consumer queue: simulation must not be able
// to run ahead of presentation and silently add input latency. Today the ring is
// bounded at kPresentationSnapshotRingSize slots and the producer and consumer are the
// same thread, so the queue CANNOT grow -- but a publish that lands before the previous
// snapshot was retired is exactly the event that will mean "the producer outran the
// consumer" once step 6 splits the threads. `publishedWithoutConsume` counts it now, on
// the synchronous path, so the day it starts moving is visible rather than inferred.
//
// Storage follows the StreamingDiag / LightingDiag pattern: plain fields on the
// Poseidon::Dev side of the boundary, so the panel and the --capture-metrics sidecar can
// read them with no new link edge and no Engine vtable slot. Single-writer (the publish
// and retire calls, main thread).

#include <cstdint>

namespace Poseidon::Dev
{

struct SnapshotCounters
{
    uint64_t published = 0;  // total publishes since process start == newest generation
    uint64_t consumed = 0;   // total retires; `published - consumed` is the queue depth
    uint64_t fallback = 0;   // publishes that came from the renderer's capture-if-stale path,
                             // not from the world -- i.e. frames no world drove

    // The 5.3 bound, made observable. Incremented when a publish finds the previous
    // snapshot still un-retired. Zero on the synchronous path by construction; a nonzero
    // value after step 6 means simulation is producing faster than presentation consumes
    // and the ring is dropping frames rather than queueing them.
    uint64_t publishedWithoutConsume = 0;

    // WHICH publish dropped, not just how many. A traverse capture reporting `1` is
    // ambiguous between "one frame of the menu-to-world handover" and "the handoff is
    // wrong and it happened once by luck"; the generation says which, because the first
    // is always a small number and the second is not. Measured on perf_abel 2026-08-31:
    // exactly one drop, at a low generation, i.e. the load transition.
    uint64_t firstDropGeneration = 0; // 0 == never dropped

    // Age, in publishes, of the snapshot the last retire consumed. 0 = the renderer
    // consumed the snapshot published for that same frame (the only value the
    // synchronous path can produce); >0 after step 6 means presentation is running
    // behind simulation by that many frames.
    uint64_t consumedAge = 0;

    uint32_t ringSize = 0;      // kPresentationSnapshotRingSize, echoed so a capture sidecar
                                // records the bound alongside the counts
    uint32_t resourceEpoch = 0; // the epoch stamped on the newest published snapshot
};

SnapshotCounters& GSnapshotCounters();

} // namespace Poseidon::Dev
