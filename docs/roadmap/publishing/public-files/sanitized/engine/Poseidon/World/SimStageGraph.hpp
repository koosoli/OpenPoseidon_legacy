#pragma once

// Roadmap Phase 8, critical-path step 10S -- "First run the same task graph
// serially. Only after serial equivalence is proven should independent stages run
// concurrently." See design notes
//
// This is the SERIAL half and only the serial half. Nothing here schedules, nothing
// here is thread-aware, and no stage runs concurrently with any other. Phase 8 is
// explicit that ordering must not be forced at the cost of what immutable inputs and
// a deterministic commit can provide later, so this deliberately stops at making the
// order INSPECTABLE rather than building a scheduler that would have to be unbuilt.
//
// What it changes: `World::StepSimulation` used to express its order as a sequence of
// calls, so the only way to know what runs when was to read the function, and the only
// way to assert it was to not. Here the order is `kSimStageOrder` -- data, which can be
// walked, printed, and required by a test.
//
// The order encoded here is the order the engine ACTUALLY RUNS, recorded in
// design notes
// It differs from Phase 8's diagram in two places where the engine is right and the
// diagram is not -- most importantly scripts run BEFORE the AI, so an order a script
// issues is acted on in the same tick. Do not "fix" this to match the picture; that is
// a gameplay change and the roadmap requires behavioural and multiplayer tests for one.
//
// The resource declarations below are the point of the exercise. They are what makes
// the third difference SIM-810 found -- that the AI consumes visibility written LATER
// in the tick, i.e. the previous tick's -- a DECLARED edge that `AnalyseSerialOrder`
// reports, rather than a fact somebody has to notice while reading 80 lines of C++.
//
// Honest limit, as amended by SIM-814: the reads/writes are still hand-audited rather
// than derived by the compiler, but they are no longer unverified. `SimStageEvidence.hpp`
// carries the call path behind every bit, and `test_sim_stage_evidence.cpp` fails when a
// path stops matching the source, when a bit has no path and no written reason, or -- for
// the three resources whose access points can be enumerated -- when a new call appears
// inside a stage's reach. That audit found SIX bits missing, every one of them four or
// more layers of call below a stage's entry point.
//
// What is still not caught: a new touch of Clock, VehicleState or AIState, whose witnesses
// run to thousands of sites and are not censused. Making the declaration binding in full
// needs the "deterministic command and result commit" stage that SIM-810 names as the
// largest remaining piece of work, and that is deliberately not started here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Poseidon::Sim
{

enum class SimStage : std::uint8_t;

/// A read-only view over a stage order. `std::span` would say this in one word, but the
/// Poseidon library compiles as C++17.
struct SimStageSpan
{
    const SimStage* first = nullptr;
    std::size_t     count = 0;

    SimStageSpan() = default;
    SimStageSpan(const SimStage* data, std::size_t n) : first(data), count(n) {}

    template <std::size_t N>
    SimStageSpan(const std::array<SimStage, N>& a) : first(a.data()), count(N)
    {
    }

    SimStageSpan(const std::vector<SimStage>& v) : first(v.data()), count(v.size()) {}

    [[nodiscard]] const SimStage* begin() const { return first; }
    [[nodiscard]] const SimStage* end() const { return first + count; }
    [[nodiscard]] std::size_t     size() const { return count; }
    [[nodiscard]] bool            empty() const { return count == 0; }
    const SimStage&               operator[](std::size_t i) const { return first[i]; }
};

/// The stages of one fixed simulation tick, as `World::StepSimulation` runs them.
///
/// This covers stages 2-7 of SIM-810's table -- the body of `StepSimulation`. The
/// debug-input scancodes ahead of it and the network/chat/voice handling after it are
/// not simulation and are not stages.
enum class SimStage : std::uint8_t
{
    /// `Glob.time += deltaT` and `SimulateLandscape` -- world clock, weather, wind.
    Clock,
    /// `SimulateScripts`. Opt-in (`POSEIDON_FIXEDSTEP_SCRIPTS`), default OFF.
    Scripts,
    /// `PerformAI`, or `FinishSecondary` under `BACKGROUND_AI`.
    AI,
    /// `SimulateAllVehicles`.
    Vehicles,
    /// `GetSensorList()->SmartUpdateAll()`. Gated by a cheat toggle (Ctrl-Y), on in
    /// normal play.
    Visibility,
    /// The player proxy move plus exactly one `PhysicsWorld::Step` per tick.
    Physics,
};

inline constexpr std::size_t kSimStageCount = 6;

/// THE ORDER. This is the whole deliverable of the "make it data" half.
inline constexpr std::array<SimStage, kSimStageCount> kSimStageOrder = {
    SimStage::Clock, SimStage::Scripts, SimStage::AI,
    SimStage::Vehicles, SimStage::Visibility, SimStage::Physics,
};

[[nodiscard]] std::string_view SimStageName(SimStage stage);

[[nodiscard]] constexpr std::uint32_t SimStageMask(SimStage stage)
{
    return 1u << static_cast<std::uint32_t>(stage);
}

/// Every stage enabled. Callers clear the bits their tick does not run.
inline constexpr std::uint32_t kAllSimStages = (1u << kSimStageCount) - 1u;

/// The shared state stages read and write.
///
/// Coarse on purpose: the useful question at this granularity is "does stage B read
/// something stage A produced, and does A run first", and a finer partition would be
/// a guess dressed as a fact.
enum class SimResource : std::uint8_t
{
    /// `Glob.time`, weather, wind.
    Clock,
    /// Script VM state -- variables, running script set.
    ScriptState,
    /// Orders, plans, subgroup state.
    AIState,
    /// Entity transforms and velocities.
    VehicleState,
    /// Sensor-list results: who can see whom.
    Visibility,
    /// The rigid-body world inside the physics backend.
    PhysicsState,
};

inline constexpr std::size_t kSimResourceCount = 6;

[[nodiscard]] std::string_view SimResourceName(SimResource resource);

[[nodiscard]] constexpr std::uint32_t SimResourceMask(SimResource resource)
{
    return 1u << static_cast<std::uint32_t>(resource);
}

/// One stage's hand-audited declaration.
struct SimStageDecl
{
    SimStage      stage{};
    std::uint32_t reads = 0;
    std::uint32_t writes = 0;
};

/// Declarations for every stage, indexed by `SimStage`.
[[nodiscard]] const std::array<SimStageDecl, kSimStageCount>& SimStageDecls();

[[nodiscard]] const SimStageDecl& SimStageDeclFor(SimStage stage);

/// A dependency the serial order creates between two stages.
struct SimStageEdge
{
    SimStage    producer{};
    SimStage    consumer{};
    SimResource resource{};

    /// True when the producer runs EARLIER in the same tick, so the consumer sees this
    /// tick's value. False when the producer runs LATER, so the consumer necessarily
    /// reads the PREVIOUS tick's value -- a one-tick lag baked into the order.
    ///
    /// A false here is not automatically a defect. It is a question with a name, which
    /// is more than the call sequence offered.
    bool sameTick = true;
};

/// Walks an order and reports every producer/consumer edge the declarations imply.
///
/// For each stage, in order, and each resource it reads: the nearest EARLIER writer
/// wins and yields a same-tick edge; failing that the nearest LATER writer yields a
/// previous-tick edge. A stage reading only what it itself writes is carry-over state,
/// not an edge, and is not reported.
[[nodiscard]] std::vector<SimStageEdge> AnalyseSerialOrder(SimStageSpan order);

/// Edges whose producer runs after their consumer. The interesting subset.
[[nodiscard]] std::vector<SimStageEdge> PreviousTickEdges(SimStageSpan order);

/// Invoked once per enabled stage, in order. A plain function pointer, not a
/// `std::function`: this runs at the fixed-step tick rate and must not allocate.
using SimStageDispatch = void (*)(void* context, SimStage stage);

/// What one walk of the graph actually executed, in the order it executed it.
///
/// Returned rather than logged so a test can require it. "Inspected, logged and
/// asserted instead of inferred from reading a function" is the whole ask.
struct SimStageTrace
{
    std::array<SimStage, kSimStageCount> visited{};
    std::uint8_t                         count = 0;
    /// Wall time of each visited stage, same index as `visited`. PERF-012 measured the
    /// fixed-step loop at 30 ms -- 48% of a combat frame and the engine's largest single
    /// CPU cost -- and could say nothing about WHICH stage that is. The runner already
    /// walks the stages one at a time, so this is the one place where per-stage cost can
    /// be had without a timer at every call site.
    ///
    /// Wall time, not simulation time: this is diagnostic and must never reach a
    /// simulation decision. SIM-813 removed the last wall clock that did, and SIM-816's
    /// determinism mode would report a divergence if one came back.
    std::array<float, kSimStageCount> ms{};

    [[nodiscard]] SimStageSpan Visited() const { return {visited.data(), count}; }
};

/// Per-stage cost, accumulated across every tick since the last read. The runner adds to
/// this unconditionally -- one `steady_clock` pair per stage, six per tick, against stages
/// that cost milliseconds -- so there is no gate to forget to switch on and no "did not
/// run" ambiguity in a zero.
struct SimStageCostAccum
{
    std::array<double, kSimStageCount> ms{};
    std::uint64_t                      ticks = 0;
};

/// The process-wide accumulator. Read it, print it, and call `Reset()` to start a window.
SimStageCostAccum& SimStageCosts();
void               ResetSimStageCosts();

/// Runs `order` serially, skipping stages whose bit is clear in `enabledMask`.
///
/// Serial, single-threaded, no barriers, no scheduler. One `dispatch` call per enabled
/// stage, in `order`, on the calling thread.
SimStageTrace RunSimStagesSerial(SimStageSpan order, std::uint32_t enabledMask, void* context,
                                 SimStageDispatch dispatch);

/// The engine's order.
SimStageTrace RunSimStagesSerial(std::uint32_t enabledMask, void* context, SimStageDispatch dispatch);

} // namespace Poseidon::Sim
