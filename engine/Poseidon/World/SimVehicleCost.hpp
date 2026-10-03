#pragma once
// PERF-023 -- the subdivision of the Vehicles simulation stage.
//
// PERF-013 measured the fixed-step tick's six stages and PERF-021 made the producer the
// frame's bound; the largest producer item is now `SimulateAllVehicles` at 3.7-3.9 ms/tick
// of a 4.9-5.0 ms tick on `perf_combat`, and nothing said what inside it costs that. This is
// the AI stage's instrument (AI/AICostAccum.hpp) one stage over: one process-wide
// accumulator, added to unconditionally, reported once a second with its denominators by the
// block in World.cpp that already reports the AI buckets, then reset.
//
// Two disjoint partitions of the stage, each summing to (nearly) the stage total:
//
//   by SUB-STAGE (SimulateAllVehicles' own structure):
//     importance  DistributeFar/NearImportances (periodic)
//     cloudlets   SimulateCloudlets
//     slow        SimulateVehicles: the _vehicles + _animals lists (1/15 s sub-steps + rest)
//     fast        SimulateFastVehicles: the _fastVehicles list (1 ms sub-steps + rest)
//     buildings   SimulateBuildings
//     attached    the _attached UpdatePosition loop
//
//   by ENTITY KIND (inside SimulateOnly, per entity, wall time of its Simulate call):
//     man, tank (incl. APC), car, air, ship, static, other -- each with a CALL count, so a
//     kind that ran and cost nothing is distinguishable from one that did not run.
//
// WALL TIME, DIAGNOSTIC ONLY: nothing in the simulation reads these fields.
#include <array>
#include <cstdint>

namespace Poseidon::SimVehCost
{
enum class Stage : std::uint8_t
{
    Importance,
    Cloudlets,
    Slow,
    Fast,
    Buildings,
    Attached,
    // PERF-027: `Slow` is four things, and by-kind accounting only ever covered
    // one of them (the per-entity Simulate). These three split the rest, because
    // the footprint ablation showed 2.2 ms/frame moving inside `Slow` with no
    // by-kind row moving with it.
    SlowMoveOut,   // the four MoveOutAndDelete list walks
    SlowVehicles,  // SimulateOnly over _vehicles
    SlowAnimals,   // SimulateOnly over _animals -- where footprint marks live
    // PERF-029: the two halves of a soldier's own tick, bracketed to bisect the
    // footprint effect PERF-028 could not locate by reading.
    ManPilot,      // KeyboardPilot / AIPilot / FakePilot / DisabledPilot
    ManBase,       // base::Simulate -- Person, Man, EntityAI, Entity
    // Nested within Cloudlets, never additive to its existing container.
    CloudletSoftSurfaces,
    CloudletRunoffSource,
    CloudletRunoffAdvance,
    CloudletRunoffNatural,
    CloudletRemainingEffects,
};
inline constexpr std::size_t kStageCount = 16;

enum class Kind : std::uint8_t
{
    Man,
    Tank,
    Car,
    Air,
    Ship,
    Static,
    Other,
};
inline constexpr std::size_t kKindCount = 7;

/// PERF-027: is the accounting on at all? OFF by default, from
/// `POSEIDON_SIM_VEHICLE_COST=1`, read once.
///
/// It has to be a gate rather than a cheap always-on counter because the
/// per-entity work is NOT the two clock reads -- it is the `IsKindOf` chain that
/// classifies every entity AFTER every Simulate call, which on a scene with a few
/// thousand footprint marks runs a few thousand times a tick and costs more than
/// several of the things it was built to measure.
[[nodiscard]] bool Enabled();

[[nodiscard]] const char* StageName(Stage stage);
[[nodiscard]] const char* KindName(Kind kind);

struct Accum
{
    std::array<double, kStageCount>        stageMs{};
    std::array<double, kKindCount>         kindMs{};
    std::array<std::uint64_t, kKindCount>  kindCalls{};
    std::array<std::uint64_t, kStageCount> substageCalls{};
    std::array<double, kStageCount>        substageMaxMs{};
    std::uint64_t                          ticks = 0; // SimulateAllVehicles entries
};

// Pure accounting seam; reject containers to prevent accidental double counting.
inline bool RecordCloudletStage(Accum& accum, Stage stage, std::uint64_t ns)
{
    const auto index = static_cast<std::size_t>(stage);
    if (index < static_cast<std::size_t>(Stage::CloudletSoftSurfaces) || index >= kStageCount)
        return false;
    const double ms = static_cast<double>(ns) * 1.0e-6;
    accum.stageMs[index] += ms;
    ++accum.substageCalls[index];
    if (ms > accum.substageMaxMs[index]) accum.substageMaxMs[index] = ms;
    return true;
}

// Default-off instrumentation: no clock reads or accounting when the existing
// vehicle-cost opt-in is disabled. Stop is idempotent, also used by destruction.
class ScopedCloudletStage
{
public:
    explicit ScopedCloudletStage(Stage stage);
    ~ScopedCloudletStage();
    void Stop();
    ScopedCloudletStage(const ScopedCloudletStage&) = delete;
    ScopedCloudletStage& operator=(const ScopedCloudletStage&) = delete;
private:
    Stage _stage;
    bool _enabled;
    std::uint64_t _startNs;
};

Accum& Costs();
void   Reset();

std::uint64_t NowNs();

class ScopedStage
{
public:
    explicit ScopedStage(Stage stage);
    ~ScopedStage();
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

private:
    Stage         _stage;
    std::uint64_t _startNs;
};

void AddKind(Kind kind, std::uint64_t ns);
} // namespace Poseidon::SimVehCost
