// SIM-811 -- roadmap Phase 8, critical-path step 10S: the SERIAL task graph.
//
// Two things are under test and they are not the same thing.
//
//   1. THE GRAPH IS THE ENGINE'S ORDER. `kSimStageOrder` and the resource declarations
//      beside it are asserted against SIM-810's recorded audit of `World::StepSimulation`
//      -- including the two places the engine deliberately disagrees with Phase 8's
//      diagram. If somebody "fixes" the order to match the picture, these fail.
//
//   2. SERIAL EQUIVALENCE. Phase 8's gate is that the explicit graph produces the same
//      results as the implicit order. That is proved here on a MODEL of the step, not on
//      the live `World`: a model with the same stage set, the same resource dependencies
//      and the same read-of-a-later-stage lag, run twice -- once as a hand-written call
//      sequence shaped exactly like the old `StepSimulation` body, once through
//      `RunSimStagesSerial` -- and compared with `Determinism::CompareTimelines`, which
//      reports the FIRST divergence as a tick, a record, a field and two bit patterns.
//
//      What that buys: the runner's semantics are proved (it walks the declared order,
//      it skips exactly the masked stages, it does not reorder or repeat). What it does
//      NOT buy: it is not a measurement of the real `World`, which needs game data and a
//      running engine. The real step's equivalence rests on the two runs sharing one
//      order constant plus inspection of the diff -- said plainly in SIM-811 rather than
//      dressed up here.
//
// Every equivalence case has an ablation beside it. A green test that would also be
// green with the graph wrong proves nothing; SIM-806 and SIM-808 both carry this
// discipline and this file copies it.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Core/StateTimeline.hpp>
#include <Poseidon/World/SimStageGraph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace Poseidon::Sim;
using Poseidon::Determinism::CompareTimelines;
using Poseidon::Determinism::DivergenceKind;
using Poseidon::Determinism::StateTimeline;

namespace
{

// ---------------------------------------------------------------------------
// A model of the step.
//
// Deliberately not a toy that would agree with itself whatever the order. Every stage
// reads something an adjacent stage wrote, and the AI reads `visible`, which the
// Visibility stage writes LATER in the tick -- the one-tick lag SIM-810 found in the
// real engine. Reproducing that lag is the point: an equivalence harness over a model
// without it could not tell a correct order from a reordered one.
// ---------------------------------------------------------------------------

constexpr int kBodies = 8;
constexpr int kTicks = 64;
constexpr float kDt = 1.0f / 60.0f;

struct ModelWorld
{
    float clock = 0.0f;
    float wind = 0.0f;
    float scriptPush = 0.0f;

    std::array<float, kBodies>         aim{};
    std::array<float, kBodies>         pos{};
    std::array<float, kBodies>         vel{};
    std::array<std::uint32_t, kBodies> visible{};
    std::array<float, kBodies>         physPos{};
    std::uint32_t                      events = 0;
};

void StageClock(ModelWorld& w)
{
    w.clock += kDt;
    w.wind = std::sin(w.clock * 1.7f) * 0.5f;
}

void StageScripts(ModelWorld& w)
{
    // Reads the clock this tick (why it sits after it) and writes AI/vehicle state --
    // which is why running before the AI, not after physics, changes the answer.
    w.scriptPush = w.clock * 0.25f;
    for (int i = 0; i < kBodies; ++i)
    {
        w.aim[i] += w.scriptPush * 0.01f;
    }
}

void StageAI(ModelWorld& w)
{
    for (int i = 0; i < kBodies; ++i)
    {
        // THE LAGGED READ. `visible` is written by the Visibility stage, which runs
        // after this one, so this observes the previous tick.
        const float seen = (w.visible[i] != 0) ? 0.4f : -0.2f;
        w.aim[i] = w.aim[i] * 0.9f + seen + w.wind * 0.05f + static_cast<float>(i) * 0.003f;
    }
    ++w.events;
}

void StageVehicles(ModelWorld& w)
{
    for (int i = 0; i < kBodies; ++i)
    {
        w.vel[i] += w.aim[i] * kDt;
        w.pos[i] += w.vel[i] * kDt;
    }
}

void StageVisibility(ModelWorld& w)
{
    for (int i = 0; i < kBodies; ++i)
    {
        w.visible[i] = (w.pos[i] > 0.0f) ? 1u : 0u;
    }
}

void StagePhysics(ModelWorld& w)
{
    for (int i = 0; i < kBodies; ++i)
    {
        w.physPos[i] = w.physPos[i] * 0.98f + w.pos[i] * 0.02f;
    }
}

void RunOneStage(ModelWorld& w, SimStage stage)
{
    switch (stage)
    {
    case SimStage::Clock:
        StageClock(w);
        break;
    case SimStage::Scripts:
        StageScripts(w);
        break;
    case SimStage::AI:
        StageAI(w);
        break;
    case SimStage::Vehicles:
        StageVehicles(w);
        break;
    case SimStage::Visibility:
        StageVisibility(w);
        break;
    case SimStage::Physics:
        StagePhysics(w);
        break;
    }
}

/// Record one tick's state. `trace` is the executed stage sequence; recording it makes
/// ORDER part of the compared state, so a reordered graph is reported by the same
/// comparison that reports a wrong number.
void RecordTick(StateTimeline& t, const ModelWorld& w, SimStageSpan trace, bool recordOrder)
{
    t.BeginTick();

    if (recordOrder)
    {
        for (std::size_t i = 0; i < trace.size(); ++i)
        {
            t.U32("stage", static_cast<std::uint32_t>(i), "id", static_cast<std::uint32_t>(trace[i]));
        }
    }

    t.F32("world", 0, "clock", w.clock);
    t.F32("world", 0, "wind", w.wind);
    t.F32("world", 0, "scriptPush", w.scriptPush);
    t.U32("world", 0, "events", w.events);

    for (std::uint32_t i = 0; i < kBodies; ++i)
    {
        t.F32("body", i, "aim", w.aim[i]);
        t.F32("body", i, "pos", w.pos[i]);
        t.F32("body", i, "vel", w.vel[i]);
        t.U32("body", i, "visible", w.visible[i]);
        t.F32("body", i, "phys", w.physPos[i]);
    }

    t.EndTick();
}

/// The IMPLICIT order: a hand-written call sequence, shaped exactly like the body of
/// `World::StepSimulation` before SIM-811 -- the same conditionals in the same places.
/// This is the control the graph is measured against.
void RunImplicit(StateTimeline& t, bool scriptsEnabled, bool visibilityEnabled, bool physicsPresent,
                 bool recordOrder)
{
    ModelWorld w;
    for (int tick = 0; tick < kTicks; ++tick)
    {
        std::vector<SimStage> executed;

        StageClock(w);
        executed.push_back(SimStage::Clock);

        if (scriptsEnabled)
        {
            StageScripts(w);
            executed.push_back(SimStage::Scripts);
        }

        StageAI(w);
        executed.push_back(SimStage::AI);

        StageVehicles(w);
        executed.push_back(SimStage::Vehicles);

        if (visibilityEnabled)
        {
            StageVisibility(w);
            executed.push_back(SimStage::Visibility);
        }

        if (physicsPresent)
        {
            StagePhysics(w);
            executed.push_back(SimStage::Physics);
        }

        RecordTick(t, w, executed, recordOrder);
    }
}

std::uint32_t MaskFor(bool scripts, bool visibility, bool physics)
{
    std::uint32_t mask = kAllSimStages;
    if (!scripts)
    {
        mask &= ~SimStageMask(SimStage::Scripts);
    }
    if (!visibility)
    {
        mask &= ~SimStageMask(SimStage::Visibility);
    }
    if (!physics)
    {
        mask &= ~SimStageMask(SimStage::Physics);
    }
    return mask;
}

/// The EXPLICIT graph: the order comes from `kSimStageOrder` (or a caller-supplied
/// permutation, which is how the reordering ablation is applied).
void RunViaGraph(StateTimeline& t, SimStageSpan order, std::uint32_t mask, bool recordOrder,
                 int perturbTick = -1, int perturbBody = -1)
{
    ModelWorld w;
    for (int tick = 0; tick < kTicks; ++tick)
    {
        const SimStageTrace trace = RunSimStagesSerial(order, mask, &w,
                                                       [](void* ctx, SimStage stage)
                                                       { RunOneStage(*static_cast<ModelWorld*>(ctx), stage); });

        if (tick == perturbTick && perturbBody >= 0)
        {
            // One ulp on one field of one body at one tick.
            std::uint32_t bits = 0;
            std::memcpy(&bits, &w.pos[perturbBody], sizeof(bits));
            bits += 1;
            std::memcpy(&w.pos[perturbBody], &bits, sizeof(bits));
        }

        RecordTick(t, w, trace.Visited(), recordOrder);
    }
}

std::vector<std::string> StageNames(SimStageSpan order)
{
    std::vector<std::string> names;
    names.reserve(order.size());
    for (const SimStage s : order)
    {
        names.emplace_back(SimStageName(s));
    }
    return names;
}

std::size_t PositionOf(SimStage stage)
{
    const auto it = std::find(kSimStageOrder.begin(), kSimStageOrder.end(), stage);
    return static_cast<std::size_t>(std::distance(kSimStageOrder.begin(), it));
}

} // namespace

// ===========================================================================
// 1. The graph is the engine's order
// ===========================================================================

TEST_CASE("the stage order is SIM-810's audited order, not Phase 8's diagram", "[determinism][taskgraph]")
{
    const std::vector<std::string> expected = {"clock", "scripts", "ai", "vehicles", "visibility", "physics"};
    REQUIRE(StageNames(kSimStageOrder) == expected);
}

TEST_CASE("scripts run before the AI, deliberately", "[determinism][taskgraph]")
{
    // Phase 8's diagram puts scripts second from last. The engine puts them third from
    // first, and says why at the call site: an order a script issues is acted on in the
    // same tick rather than the next one. Moving them would add a tick of latency to
    // every scripted order -- a gameplay change, and the roadmap requires behavioural
    // and multiplayer tests for one. This case exists so the change cannot be made by
    // accident while "tidying up to match the roadmap".
    REQUIRE(PositionOf(SimStage::Scripts) < PositionOf(SimStage::AI));
    REQUIRE(PositionOf(SimStage::Scripts) > PositionOf(SimStage::Clock));
}

TEST_CASE("the AI's visibility read is a declared previous-tick edge", "[determinism][taskgraph]")
{
    // SIM-810's third finding, promoted from "something you notice while reading
    // World.cpp" to a fact the graph reports. NOT fixed here: whether the one-tick lag
    // is intended or accidental is unsettled, and changing it is an AI-ordering change.
    REQUIRE(PositionOf(SimStage::AI) < PositionOf(SimStage::Visibility));

    const std::vector<SimStageEdge> lagged = PreviousTickEdges(kSimStageOrder);

    const auto has = [&](SimStage producer, SimStage consumer, SimResource resource)
    {
        return std::any_of(lagged.begin(), lagged.end(),
                           [&](const SimStageEdge& e) {
                               return e.producer == producer && e.consumer == consumer && e.resource == resource;
                           });
    };

    REQUIRE(has(SimStage::Visibility, SimStage::AI, SimResource::Visibility));

    // The other three are the cost of running scripts first: a script observes the
    // previous tick's AI, vehicle and visibility state. Real, and recorded here so the
    // set is pinned rather than the one interesting member cherry-picked out of it.
    REQUIRE(has(SimStage::AI, SimStage::Scripts, SimResource::AIState));
    REQUIRE(has(SimStage::Vehicles, SimStage::Scripts, SimResource::VehicleState));
    REQUIRE(has(SimStage::Visibility, SimStage::Scripts, SimResource::Visibility));

    // Was 4 when SIM-811 wrote this case. SIM-814 audited the declarations against the
    // call sites and found six more bits, which add four more previous-tick edges: the
    // wind tick reading last tick's helicopter positions, a flare's script run from
    // inside the vehicle stage, automatic fire reading the same stale sensor results the
    // AI reads, and loose objects taking a pose from a solver that has not stepped yet.
    // The whole set, in order, is pinned in test_sim_stage_evidence.cpp.
    REQUIRE(lagged.size() == 8);
}

TEST_CASE("same-tick edges are the ones the order was chosen to create", "[determinism][taskgraph]")
{
    const std::vector<SimStageEdge> edges = AnalyseSerialOrder(kSimStageOrder);

    const auto sameTick = [&](SimStage producer, SimStage consumer, SimResource resource)
    {
        return std::any_of(edges.begin(), edges.end(),
                           [&](const SimStageEdge& e) {
                               return e.sameTick && e.producer == producer && e.consumer == consumer &&
                                      e.resource == resource;
                           });
    };

    // The clock is consumed this tick by everything that needs it -- the reason it is
    // first, per the comment at the call site.
    REQUIRE(sameTick(SimStage::Clock, SimStage::Scripts, SimResource::Clock));
    REQUIRE(sameTick(SimStage::Clock, SimStage::AI, SimResource::Clock));
    REQUIRE(sameTick(SimStage::Clock, SimStage::Vehicles, SimResource::Clock));

    // The whole justification for scripts-before-AI, as an edge.
    REQUIRE(sameTick(SimStage::Scripts, SimStage::AI, SimResource::AIState));
    // And the AI's orders reach the vehicles in the same tick.
    REQUIRE(sameTick(SimStage::AI, SimStage::Vehicles, SimResource::AIState));
    // Visibility sees where things ended up, and physics places its proxy from it.
    REQUIRE(sameTick(SimStage::Vehicles, SimStage::Visibility, SimResource::VehicleState));
    REQUIRE(sameTick(SimStage::Vehicles, SimStage::Physics, SimResource::VehicleState));
}

// ===========================================================================
// 2. The runner
// ===========================================================================

TEST_CASE("the runner visits the declared order and nothing else", "[determinism][taskgraph]")
{
    std::vector<SimStage> seen;
    const SimStageTrace   trace = RunSimStagesSerial(kAllSimStages, &seen,
                                                     [](void* ctx, SimStage stage)
                                                     { static_cast<std::vector<SimStage>*>(ctx)->push_back(stage); });

    REQUIRE(trace.count == kSimStageCount);
    REQUIRE(seen.size() == kSimStageCount);
    REQUIRE(std::equal(seen.begin(), seen.end(), kSimStageOrder.begin()));
    REQUIRE(std::equal(trace.Visited().begin(), trace.Visited().end(), kSimStageOrder.begin()));
}

TEST_CASE("a masked stage is skipped and the rest keep their order", "[determinism][taskgraph]")
{
    // The three real gates: scripts off by default, visibility off under the Ctrl-Y
    // cheat, physics off when there is no physics world.
    const std::uint32_t mask = MaskFor(false, false, false);

    std::vector<SimStage> seen;
    RunSimStagesSerial(mask, &seen,
                       [](void* ctx, SimStage stage)
                       { static_cast<std::vector<SimStage>*>(ctx)->push_back(stage); });

    const std::vector<SimStage> expected = {SimStage::Clock, SimStage::AI, SimStage::Vehicles};
    REQUIRE(seen == expected);
}

TEST_CASE("an empty mask runs nothing", "[determinism][taskgraph]")
{
    int  calls = 0;
    const SimStageTrace trace =
        RunSimStagesSerial(0u, &calls, [](void* ctx, SimStage) { ++*static_cast<int*>(ctx); });
    REQUIRE(calls == 0);
    REQUIRE(trace.count == 0);
}

// ===========================================================================
// 3. Serial equivalence -- the gate
// ===========================================================================

TEST_CASE("the explicit graph is bit-identical to the implicit call sequence", "[determinism][taskgraph]")
{
    // Scripts off, everything else on: the configuration the game actually ships.
    StateTimeline implicitRun;
    StateTimeline graphRun;

    RunImplicit(implicitRun, /*scripts*/ false, /*visibility*/ true, /*physics*/ true, /*recordOrder*/ true);
    RunViaGraph(graphRun, kSimStageOrder, MaskFor(false, true, true), /*recordOrder*/ true);

    // Anti-vacuity: a run whose ticks all hash alike is one where nothing moved, and it
    // would agree with anything. SIM-808's rule.
    REQUIRE(implicitRun.TickCount() == kTicks);
    REQUIRE(implicitRun.DistinctTickHashes() == static_cast<std::size_t>(kTicks));

    const auto d = CompareTimelines(implicitRun, graphRun);
    INFO(d.Describe());
    REQUIRE(d.kind == DivergenceKind::None);
    REQUIRE(implicitRun.RunHash() == graphRun.RunHash());
}

TEST_CASE("equivalence holds with every stage enabled, scripts included", "[determinism][taskgraph]")
{
    StateTimeline implicitRun;
    StateTimeline graphRun;

    RunImplicit(implicitRun, true, true, true, true);
    RunViaGraph(graphRun, kSimStageOrder, kAllSimStages, true);

    REQUIRE(implicitRun.DistinctTickHashes() == static_cast<std::size_t>(kTicks));

    const auto d = CompareTimelines(implicitRun, graphRun);
    INFO(d.Describe());
    REQUIRE(d.kind == DivergenceKind::None);
}

TEST_CASE("equivalence holds for every combination of the three real gates", "[determinism][taskgraph]")
{
    for (int combo = 0; combo < 8; ++combo)
    {
        const bool scripts = (combo & 1) != 0;
        const bool visibility = (combo & 2) != 0;
        const bool physics = (combo & 4) != 0;

        StateTimeline implicitRun;
        StateTimeline graphRun;
        RunImplicit(implicitRun, scripts, visibility, physics, true);
        RunViaGraph(graphRun, kSimStageOrder, MaskFor(scripts, visibility, physics), true);

        const auto d = CompareTimelines(implicitRun, graphRun);
        INFO("combo " << combo << ": " << d.Describe());
        REQUIRE(d.kind == DivergenceKind::None);
    }
}

// ===========================================================================
// 4. Ablations -- the harness can fail
// ===========================================================================

TEST_CASE("reordering two stages is reported, and reported at the order record", "[determinism][taskgraph]")
{
    // Swap AI and Visibility -- exactly the change SIM-810 says would need behavioural
    // tests, applied here only to a model, to show the harness would notice.
    std::array<SimStage, kSimStageCount> permuted = kSimStageOrder;
    std::swap(permuted[PositionOf(SimStage::AI)], permuted[PositionOf(SimStage::Visibility)]);

    StateTimeline reference;
    StateTimeline reordered;
    RunImplicit(reference, false, true, true, true);
    RunViaGraph(reordered, permuted, MaskFor(false, true, true), true);

    const auto d = CompareTimelines(reference, reordered);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
    // The order is recorded first in each tick, so the very first thing that differs is
    // the executed sequence itself, at tick 0.
    REQUIRE(d.tick == 0);
    REQUIRE(d.stream == "stage");
    REQUIRE(d.kind == DivergenceKind::FieldValue);
    REQUIRE(d.fieldKind == Poseidon::Determinism::FieldKind::Discrete);
    // Slot 1 is where AI used to be (Scripts is masked off, so: clock, ai, vehicles...).
    REQUIRE(d.lhsBits == static_cast<std::uint64_t>(SimStage::AI));
    REQUIRE(d.rhsBits == static_cast<std::uint64_t>(SimStage::Visibility));
}

TEST_CASE("reordering two stages changes the SIMULATED STATE, not just the label",
          "[determinism][taskgraph]")
{
    // The control on the case above. If the only thing a reorder changed were the
    // bookkeeping record of the order, the equivalence proof would be circular. Here
    // the order is NOT recorded, so the only thing left to diverge is the state -- and
    // it does, at the first tick the lagged visibility read can matter.
    std::array<SimStage, kSimStageCount> permuted = kSimStageOrder;
    std::swap(permuted[PositionOf(SimStage::AI)], permuted[PositionOf(SimStage::Visibility)]);

    StateTimeline reference;
    StateTimeline reordered;
    RunImplicit(reference, false, true, true, /*recordOrder*/ false);
    RunViaGraph(reordered, permuted, MaskFor(false, true, true), /*recordOrder*/ false);

    const auto d = CompareTimelines(reference, reordered);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
    REQUIRE(d.kind == DivergenceKind::FieldValue);
    // Measured, not assumed: this diverges at the FIRST tick, in `body[0].pos`. Moving
    // the AI after the vehicles means the vehicles integrate an aim of zero on tick 0,
    // so a position that should be -0.000055 is 0.000000 -- 946,352,407 ulp apart, and
    // reported as a divergence with no tolerance path to swallow it.
    //
    // The first draft of this case asserted `d.tick > 0` on the reasoning that the
    // lagged visibility read needs a tick to matter. It failed, which is the point of
    // writing ablations before believing them: the reorder moves more than the lagged
    // read. The "first means first" control lives in the two perturbation cases below,
    // where the divergence really is later than tick 0 and moves when the input moves.
    REQUIRE(d.tick == 0);
    REQUIRE(d.stream == "body");
    REQUIRE(d.field == "pos");
}

TEST_CASE("dropping a stage is reported as a shape change", "[determinism][taskgraph]")
{
    StateTimeline withPhysics;
    StateTimeline withoutPhysics;
    RunImplicit(withPhysics, false, true, true, true);
    RunViaGraph(withoutPhysics, kSimStageOrder, MaskFor(false, true, false), true);

    const auto d = CompareTimelines(withPhysics, withoutPhysics);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
    REQUIRE(d.tick == 0);
    // One fewer stage in the executed order, so the order record is a field shorter and
    // the slot that used to say "physics" now says something else or is missing.
    REQUIRE(d.stream == "stage");
}

TEST_CASE("perturbing one value at one tick names that tick, that body and that field",
          "[determinism][taskgraph]")
{
    constexpr int kAblationTick = 41;
    constexpr int kAblationBody = 5;

    StateTimeline clean;
    StateTimeline perturbed;
    RunViaGraph(clean, kSimStageOrder, MaskFor(false, true, true), true);
    RunViaGraph(perturbed, kSimStageOrder, MaskFor(false, true, true), true, kAblationTick, kAblationBody);

    const auto d = CompareTimelines(clean, perturbed);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
    REQUIRE(d.tick == kAblationTick);
    REQUIRE(d.stream == "body");
    REQUIRE(d.field == "pos");
    REQUIRE(d.key == static_cast<std::uint32_t>(kAblationBody));
    REQUIRE(d.fieldKind == Poseidon::Determinism::FieldKind::Continuous);
    // One ulp, and reported as a DIVERGENCE. No tolerance path exists to swallow it.
    REQUIRE(d.ulpDistance == 1);
}

TEST_CASE("an ablation at the last tick is reported at the last tick", "[determinism][taskgraph]")
{
    // The control on the case above: a comparison that always answered with an early
    // tick, or with the tick it was handed, would pass that one. Moving the perturbation
    // and requiring the answer to move with it is what makes "first" mean first.
    constexpr int kAblationTick = kTicks - 1;

    StateTimeline clean;
    StateTimeline perturbed;
    RunViaGraph(clean, kSimStageOrder, MaskFor(false, true, true), true);
    RunViaGraph(perturbed, kSimStageOrder, MaskFor(false, true, true), true, kAblationTick, 2);

    const auto d = CompareTimelines(clean, perturbed);
    INFO(d.Describe());
    REQUIRE(d.tick == kAblationTick);
    REQUIRE(d.key == 2u);
}
