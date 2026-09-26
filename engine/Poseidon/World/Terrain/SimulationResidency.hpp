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
    // Positive channel0 selection retains only observed resident Plain objects.
    // Completed scans prove neither complete coverage nor query readiness.
    uint64_t positiveScannedRegions = 0, positiveLeasedPlacements = 0;
    uint64_t positiveColdRequested = 0, positiveColdPrepared = 0, positiveColdCreated = 0;
    uint64_t positiveColdRefused = 0, positiveColdInterrupted = 0, positiveColdWorkingModels = 0;
    uint64_t positiveColdFactsBytes = 0, positiveColdTailCalls = 0;
    // Bank timing includes owner allocation/name normalization and table build;
    // final ShapeBank installation is recorded separately by TailMs.
    uint64_t positiveColdBankCalls = 0;
    double positiveColdLastBankMs = 0, positiveColdMaxBankMs = 0;
    double positiveColdLastTailMs = 0, positiveColdMaxTailMs = 0;
    uint64_t watchedPlacements = 0, liveWatchedPlacements = 0;
    uint64_t queryRegions = 0, readyQueries = 0, pendingQueries = 0, unknownQueries = 0, refusedQueries = 0;
    uint64_t queryBorrowedModels = 0, queryBorrowedEntries = 0;
    uint64_t registeredBorrowedModels = 0, registeredBorrowShapeCharge = 0;
    uint64_t registeredValidationBudgetRefusals = 0;
    // Historical observation only. Current coverage requires revalidating every
    // weak bank/config witness; this flag must never substitute for Ready.
    bool registeredMetadataEverComplete = false;
    uint64_t queueRefused = 0, payloadCapacityCharge = 0, shapeSchedulingCharge = 0;
    uint64_t shapeElementBytes = 0, lastPumpVisits = 0, ownerPumps = 0;
    uint64_t fireCollisionCalls = 0, fireCollisionCandidateTests = 0, fireCollisionWorkRefusals = 0;
    double lastFireCollisionMs = 0;
    bool metadataComplete = false;
    uint64_t metadataRecordBytes = 0, referencedModels = 0, activeModels = 0, sparseLeaseRecords = 0;
    uint64_t metadataPlacementRefusals = 0, metadataModelRefusals = 0, metadataByteRefusals = 0, activeModelRefusals = 0;
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
