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
};
inline constexpr std::size_t kStageCount = 6;

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

[[nodiscard]] const char* StageName(Stage stage);
[[nodiscard]] const char* KindName(Kind kind);

struct Accum
{
    std::array<double, kStageCount>        stageMs{};
    std::array<double, kKindCount>         kindMs{};
    std::array<std::uint64_t, kKindCount>  kindCalls{};
    std::uint64_t                          ticks = 0; // SimulateAllVehicles entries
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
