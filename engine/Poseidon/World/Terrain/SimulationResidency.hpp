#pragma once

#include <cstddef>
#include <cstdint>

namespace Poseidon { class World; }
namespace Poseidon::Streaming
{

enum class SimulationQueryPurpose : uint8_t { Fire = 1 };

enum class SimulationResidencyStatus : uint8_t
{
    Disabled, Pending, Ready, Unknown, Invalid, CapacityExceeded, WrongOwner
};

// Ready means the complete requested terrain-object region is installed under
// this prototype's validated static model contract. It NEVER means a clear
// collision, an AI decision, arbitrary posed geometry or background readiness.
struct SimulationResidencyStats
{
    SimulationResidencyStatus status = SimulationResidencyStatus::Disabled;
    uint64_t inventoryRows = 0, inventoryTotal = 0, coverageGeneration = 0;
    uint64_t models = 0, preparedModels = 0, unknownModels = 0;
    uint64_t regions = 0, leasedPlacements = 0, created = 0, released = 0;
    uint64_t readyRegions = 0, pendingRegions = 0, unknownRegions = 0, refusedRegions = 0;
    uint64_t watchedPlacements = 0, liveWatchedPlacements = 0;
    uint64_t queryRegions = 0, readyQueries = 0, pendingQueries = 0, unknownQueries = 0, refusedQueries = 0;
    uint64_t queueRefused = 0, payloadCapacityCharge = 0, shapeSchedulingCharge = 0;
    uint64_t shapeElementBytes = 0, lastPumpVisits = 0, ownerPumps = 0;
    uint64_t fireCollisionCalls = 0, fireCollisionCandidateTests = 0, fireCollisionWorkRefusals = 0;
    double lastFireCollisionMs = 0;
    double lastPumpMs = 0;
};

// The owner scheduler issues this ONLY after the existing background-AI join.
// A script/harness caller cannot invent a bool or epoch to perform admissions.
class SimulationResidencyOwnerBoundary
{
    friend class ::Poseidon::World;
    SimulationResidencyOwnerBoundary() = default;
public:
    SimulationResidencyOwnerBoundary(const SimulationResidencyOwnerBoundary&) = delete;
    SimulationResidencyOwnerBoundary& operator=(const SimulationResidencyOwnerBoundary&) = delete;
};

struct SimulationResidencyState;

} // namespace Poseidon::Streaming
