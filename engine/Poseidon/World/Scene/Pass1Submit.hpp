#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// ---------------------------------------------------------------------------------------
// Pass1 SUBMISSION sub-attribution.
//
// Pass1Stats (SceneDraw.cpp) splits `land:obj` six ways and answers the question it was
// built for: submission is 94-95% of the phase on every world measured, and the other five
// buckets together cannot repay more than ~1 ms. What it does NOT say is which line of the
// submission walk that is -- `submit` is one number covering the instanced-run scan, the
// per-object animation, the light selection, the proxy recursion, the texture prepare and
// the section queueing, and any one of them could be all of it.
//
// This splits `submit` again, across three translation units, because the work is spread
// across three: the run scan is in Scene::DrawObjectsAndShadowsPass1 (SceneDraw.cpp), the
// per-object blocks are in Object::Draw (Object.cpp), and the section walk is in
// Shape::Draw (ShapeDraw.cpp).
//
// EVERY bucket carries a count, so a millisecond total can be divided into a per-item cost.
// A phase that is expensive because it visits many cheap items and a phase that is expensive
// because each item is dear need opposite fixes, and a bare millisecond number cannot tell
// them apart -- this project has spent rounds on that distinction before.
//
// Gated on `gActive`, set ONLY inside the Pass1 submission loop. Object::Draw and Shape::Draw
// are also called from Pass2, from the shadow pass and recursively from DrawProxies; without
// the gate these counters would silently mix all four and report a number whose denominator
// nobody could name.
//
// NESTING: Object::Draw re-enters itself through DrawProxies. `gDepth` keeps the top-level
// blocks exclusive -- a proxy's own animate/lights/shape time is billed to the PARENT's
// `proxies` bucket and NOT double-counted into the top-level ones. `proxyDraws` counts the
// nested calls so the recursion has a denominator of its own.
//
// NANOSECONDS, not microseconds like Pass1Stats. These blocks are per-object and several are
// well under 1 us; microsecond truncation would round them to zero and report real work as
// free. This repository has a documented history of diagnostics that reported 0 where they
// meant something else.
//
// COST WHEN ON: ~12 clock reads per drawn object. At ~2,700 objects/frame that is ~30k QPC
// reads, order 0.5-0.8 ms/frame of probe overhead, which INFLATES the totals it reports. Read
// the SHARES, not the absolute milliseconds, and compare against the unprobed `submit` number
// from the same run. Off (the default) it costs one cached bool test per drawn object.
namespace Poseidon
{
namespace Pass1Submit
{

// Set true only for the duration of the Pass1 submission loop. Main thread only.
extern bool gActive;
// Object::Draw recursion depth (0 = top-level object, >0 = inside DrawProxies).
extern int gDepth;

extern std::atomic<uint64_t> gScanNs;      // run-formation scan, EXCLUDING the draws it triggers
extern std::atomic<uint64_t> gAnimateNs;   // Animate + Deanimate
extern std::atomic<uint64_t> gClipFogNs;   // bounding-sphere clip + fog constant
extern std::atomic<uint64_t> gLightsNs;    // SelectLights
extern std::atomic<uint64_t> gProxiesNs;   // DrawProxies, including everything it recurses into
extern std::atomic<uint64_t> gTexturesNs;  // PrepareTextures
extern std::atomic<uint64_t> gShapeDrawNs; // Shape::Draw -- the section walk and queueing

extern std::atomic<uint64_t> gRuns;         // instanced runs formed (length >= 4)
extern std::atomic<uint64_t> gRunObjs;      // objects covered by those runs
extern std::atomic<uint64_t> gScalarObjs;   // objects drawn one at a time
extern std::atomic<uint64_t> gSkipped;      // mergers the submit loop stepped over without drawing
extern std::atomic<uint64_t> gObjDraws;     // top-level Object::Draw bodies that reached the shape
extern std::atomic<uint64_t> gProxyDraws;   // nested (proxy) Object::Draw bodies
extern std::atomic<uint64_t> gSectionsSeen; // ShapeSections walked in Shape::Draw
extern std::atomic<uint64_t> gSectionsDrawn;// ShapeSections that survived every skip test

// WHY a candidate head was not batchable. `runs=0` on a world is a fact with several
// possible causes and opposite fixes: a scene-wide veto (one local light anywhere disables
// batching for every object in the frame), a per-object property (not Static, carries
// proxies, routed OnSurface/IsColored), or a set that simply has no four adjacent copies of
// one shape at one LOD in one fog band. These separate the three.
extern std::atomic<uint64_t> gHeads;         // loop iterations that reached the head test
extern std::atomic<uint64_t> gVetoLights;    // rejected by the scene-wide local-light veto
extern std::atomic<uint64_t> gVetoNotStatic; // rejected: object is not Static()
extern std::atomic<uint64_t> gVetoProxies;   // rejected: head shape carries proxies
extern std::atomic<uint64_t> gVetoRouting;   // rejected: OnSurface / IsColored routing
extern std::atomic<uint64_t> gRunsShort;     // batchable head, but the run was shorter than 4
extern std::atomic<uint64_t> gRunShortLen;   // summed length of those short runs

// THE PRIZE, if anything ever batches here. A pure predicate scan over the shape-sorted
// list, run beside the submission loop and touching no engine state: how many runs of >= 4
// adjacent identical draws the frame ACTUALLY contains. It is deliberately independent of
// whether the current batcher could arm on them -- on the wgpu backend it cannot, because
// EngineWgpu overrides none of the instanced-run methods and the base InstancedRunAdd
// refuses, so `runs` is structurally 0 no matter what the object set looks like. Without
// this, "batching never fires" carries no magnitude and cannot be told apart from "there
// was nothing to batch".
extern std::atomic<uint64_t> gWouldRuns;   // runs of >= 4 the sorted list contains
extern std::atomic<uint64_t> gWouldObjs;   // objects inside those runs
extern std::atomic<uint64_t> gWouldMaxLen; // longest such run seen in any frame

// PERF-015. The per-section material/texture classification cache in the wgpu backend, whose
// A/B lever is WGR_SECTION_CLASS_CACHE. These exist to answer the question a bare before/after
// cannot: whether the thing being measured was ON. `calls` counts classification questions the
// cache fielded and `misses` the ones it had to recompute from the path text; with the lever
// OFF both stay at 0 because the cache is never entered, which is how a run that measured
// nothing is told apart from a change that did nothing.
//
// Unlike every counter above these are NOT gated on the submission loop: the classifiers also
// run during model registration, and a denominator that silently excluded that would misstate
// the miss rate on exactly the frames where loading is happening.
extern std::atomic<uint64_t> gSecClassCalls;
extern std::atomic<uint64_t> gSecClassMisses;

// PERF-016. Subdivision of DrawSectionTL itself -- the term PERF-015 left unattributed.
// `shapeDraw` covers Shape::Draw (the walk, PrepareTL, BeginMeshTL) plus every
// DrawSectionTL it flushes; these split the DrawSectionTL half so the remaining
// per-section cost has names. Billed through the same gActive gate as everything
// above, so Pass2 and shadow calls stay out; unlike gShapeDrawNs they are NOT
// depth-gated, so proxy sections land here too (their own denominator, gSecTLCalls,
// counts the same population).
extern std::atomic<uint64_t> gSecTLNs;    // whole DrawSectionTL body
extern std::atomic<uint64_t> gSecTLCalls; // DrawSectionTL entries during Pass1 (top-level + proxy)
extern std::atomic<uint64_t> gSecBindNs;  // material-binding block: _materialBindings find + bank Loads
extern std::atomic<uint64_t> gSecBindCalls; // sections that entered the production/debug binding branch
extern std::atomic<uint64_t> gSecReflNs;  // the two MaterialIsReflective calls + DZ-006 guard
extern std::atomic<uint64_t> gSecDescNs;  // SplitLegacy + BuildRenderPassDescriptor
extern std::atomic<uint64_t> gSecAlphaNs; // blend/alpha-ref routing (leaf cards, bark, cutout, opaque-fix)
extern std::atomic<uint64_t> gSecLightNs; // sun/material constant fold

// PERF-016 lever self-count (WGR_SECTION_BIND_CACHE): fast-path sections served from the
// per-material binding cache vs entries that had to resolve. calls=0 <=> lever off, so a
// null A/B cannot be read as "caching does not help".
extern std::atomic<uint64_t> gSecBindFast;
extern std::atomic<uint64_t> gSecBindResolves;

bool Enabled();

inline uint64_t NowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Bills its lifetime to one counter; does nothing at all unless the stats are on AND the
// submission loop is the caller. Idempotent Close() for the blocks whose natural lifetime
// does not match a brace scope.
struct Scope
{
    std::atomic<uint64_t>* sink;
    uint64_t began;
    explicit Scope(std::atomic<uint64_t>& into) : sink((gActive && Enabled()) ? &into : nullptr), began(0)
    {
        if (sink)
        {
            began = NowNs();
        }
    }
    // Gated form: `on` is the caller's own additional condition (typically "this is a
    // top-level object, not a proxy re-entry"). Off, the scope reads no clock at all.
    Scope(std::atomic<uint64_t>& into, bool on) : sink(on ? &into : nullptr), began(0)
    {
        if (sink)
        {
            began = NowNs();
        }
    }
    void Close()
    {
        if (sink)
        {
            sink->fetch_add(NowNs() - began, std::memory_order_relaxed);
            sink = nullptr;
        }
    }
    ~Scope() { Close(); }
};

// Suspends billing of the enclosing Scope for a nested block that has its own bucket --
// used so `scan` excludes the DrawSortObject calls made from inside the scan loop.
inline bool Counting()
{
    return gActive && Enabled();
}

} // namespace Pass1Submit
} // namespace Poseidon
