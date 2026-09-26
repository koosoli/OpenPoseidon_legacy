#pragma once

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/SimulationResidency.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/World/Terrain/SimulationDemand.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyCapacity.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <optional>
#include <algorithm>
#include <memory>

namespace Poseidon::Streaming
{

// Allocated only by the default-off owner service. All weak records and shape
// references are created, read and destroyed on the owner, never by AI workers.
struct SimulationResidencyState
{
    struct RegionKey
    {
        const Object* actor = nullptr;
        uint8_t channel = 0; // 0 neighborhood, SimulationQueryPurpose otherwise
        bool operator==(const RegionKey&) const = default;
    };
    struct RegionKeyHash
    {
        size_t operator()(const RegionKey& key) const
        { return std::hash<const Object*>{}(key.actor) ^ (size_t(key.channel) * size_t(0x9e3779b9)); }
    };
    enum class ModelState { Waiting, Ready, Unknown, CapacityExceeded };
    enum class PositiveColdStage { Parse, Convert, Held };
    struct PositiveColdFacts
    {
        ObjectStreamPreparer::SourceEnvelopeSnapshot source;
        PositiveColdStage stage = PositiveColdStage::Parse;
        uint32_t selectedPlacement = 0;
        uint64_t shapeRevision = 0, factCharge = 0;
        uint32_t residentEntries = 0;
        bool retiring = false;
    };
    // Complete authored inventory metadata, no duplicate placement indices.
    // Group's full fixed record (including empty active fields) is byte charged.
    struct ModelMetadataRecord
    {
        uint64_t memberCount = 0;
        CoverageEnvelope origins{};
        bool validRows = true;
        double maximumScale = 0;
    };
    struct Group : ModelMetadataRecord
    {
        bool sourceValidated = false, charged = false, requestOutstanding = false;
        Ref<LODShapeWithShadow> shape;
        uint64_t revision = 0, payloadCharge = 0, shapeCharge = 0;
        float radius = 0;
        SimulationFactoryRoute route = SimulationFactoryRoute::Unsupported;
        ModelState status = ModelState::Waiting;
        std::unique_ptr<PositiveColdFacts> positiveCold;
        bool positiveColdRefused = false;
        bool CanEnlistPositiveCold() const
        {
            // A legacy handoff can own charges/request intent before a shape
            // exists. Cold admission must never replace that accounting.
            return status == ModelState::Waiting && !positiveCold && !positiveColdRefused &&
                !shape && !charged && !requestOutstanding && !sourceValidated &&
                payloadCharge == 0 && shapeCharge == 0;
        }
    };
    struct Entry
    {
        OLink<Object> object;
        Link<LODShape> shape;
        std::array<float, 12> frame{};
        float radius = 0;
        uint64_t positiveRevision = 0;
        int positiveId = -1;
        uint32_t positiveColdModel = std::numeric_limits<uint32_t>::max();
        bool MatchesPositiveResident(const Object& live) const
        {
            auto* current = live.GetShape();
            return static_cast<Object*>(object) == &live && current && current == shape.GetRef() &&
                positiveRevision != 0 && current->QueryPolicyRevision() == positiveRevision &&
                SimulationRoute(static_cast<const char*>(current->GetPropertyClass())) == SimulationFactoryRoute::Plain &&
                live.ID() == positiveId && live.Static() && live.GetType() == Primary && !live.MustBeSaved() && !live.IsDestroyed() &&
                live.GetDestroyed() == 0 && live.GetRawTotalDammage() == 0 &&
                SimulationFrameSample(live.Transform()) == frame && live.GetRadius() == radius;
        }
    };
    // Positive observation of an ALREADY resident Plain instance only. No group
    // shape, source certificate, factory, config lookup or query readiness.
    static std::optional<Entry> ObservePositiveResident(Object& object, int authoredId,
        const std::array<float, 12>& authoredRows)
    {
        if (!Foundation::IsMainThread()) return std::nullopt;
        auto* shape = object.GetShape();
        if (!shape || object.ID() != authoredId ||
            SimulationRoute(static_cast<const char*>(shape->GetPropertyClass())) != SimulationFactoryRoute::Plain ||
            !SimulationShapeAdmissible(*shape)) return std::nullopt;
        const auto canonical = RepairWorldObjectPlacementFrame(SimulationPlacementFrame(authoredRows), false, false,
            (shape->GetOrHints() & ClipLandKeep) != 0);
        if (!ValidSimulationFrame(canonical) || !std::isfinite(object.GetRadius()) ||
            object.GetRadius() != canonical.Scale() * shape->BoundingSphere()) return std::nullopt;
        Entry out; out.object = &object; out.shape = shape;
        out.frame = SimulationFrameSample(canonical); out.radius = object.GetRadius();
        out.positiveRevision = shape->QueryPolicyRevision();
        out.positiveId = authoredId;
        if (!out.MatchesPositiveResident(object)) return std::nullopt;
        return out;
    }
    struct Watch
    {
        OLink<Object> object;
        Link<LODShape> shape;
        std::array<float, 12> frame{};
        uint32_t model = 0;
        int id = -1, type = 0;
        float radius = 0;
        bool authored = false;
        // Only the owner may read weak witnesses. Shape policy/content scope is
        // validated separately; arbitrary mutable per-LOD APIs are excluded.
        bool MatchesImmutableObject(const LODShapeWithShadow* expectedShape, SimulationFactoryRoute expectedRoute) const
        {
            Object* live = static_cast<Object*>(object);
            if (!live) return true; // Joined cleanup may retain a harmless tombstone.
            return authored && live->ID() == id && live->Static() && live->GetType() == type &&
                live->GetType() == (expectedRoute == SimulationFactoryRoute::Road ? Network : Primary) &&
                live->GetShape() == shape.GetRef() && live->GetShape() == expectedShape &&
                SimulationFrameSample(live->Transform()) == frame && live->GetRadius() == radius &&
                !live->MustBeSaved() && !live->IsDestroyed() && live->GetDestroyed() == 0 && live->GetRawTotalDammage() == 0;
        }
    };
    struct Region
    {
        OLink<Object> actor;
        CoveragePoint from{}, to{};
        double radius = 0;
        std::array<float, 12> actorFrame{};
        std::array<float, 3> actorSpeed{};
        float actorRadius = 0;
        int actorId = -1;
        bool automatic = false;
        bool positiveOnly = false, positiveScanComplete = false;
        uint64_t positiveAdmissionGeneration = 0;
        size_t positiveValidationCursor = 0;
        uint64_t seen = 0, planGeneration = 0;
        SimulationResidencyStatus status = SimulationResidencyStatus::Pending;
        SimulationDemand plan;
        size_t cell = 0, offset = 0, pendingNew = 0;
        std::vector<uint32_t> active, pending;
        std::unordered_set<uint32_t> activeSet, pendingSet;
        bool NeedsPositiveRestart(uint64_t worldGeneration, uint64_t admissionGeneration) const
        {
            return !positiveOnly || planGeneration != worldGeneration ||
                (positiveScanComplete && positiveAdmissionGeneration != admissionGeneration);
        }
    };

    SimulationResidencyStats stats;
    std::vector<Group> groups;
    // Sparse, bounded live lease debt; zero-count rows are erased. There is no
    // allocation proportional to the landscape's millions of placement rows.
    std::unordered_map<uint32_t, uint32_t> leaseCounts;
    std::unordered_map<uint32_t, Entry> entries;
    std::deque<uint32_t> cleanupQueue;
    // Complete bounded registry of actual modern-placement admissions. Weak
    // watchers survive lease0; they neither instantiate nor retain an Object.
    std::unordered_map<uint32_t, Watch> watches;
    std::deque<uint32_t> watchCleanupQueue;
    bool watchCapacityExceeded = false;
    std::unordered_map<RegionKey, Region, RegionKeyHash> regions;
    size_t inventoryCursor = 0, modelCursor = 0, actorCursor = 0, regionCursor = 0;
    uint64_t tick = 0, generation = 1;
    uint64_t positiveAdmissionGeneration = 1;
    std::vector<uint32_t> positiveColdWork;
    size_t positiveColdCursor = 0, legacyShapeSlots = 0;
    bool inventoryComplete = false, inventoryInvalid = false;
    uint64_t referencedModels = 0;
    bool activeModelCapacityExceeded = false;
    double placementPadding = 0;

    void FinishInventory()
    {
        if (inventoryComplete) return;
        inventoryComplete = true;
        for (auto& g : groups) if (!g.memberCount) g.status = ModelState::Ready;
        if (referencedModels > SimulationMaxModels)
        { activeModelCapacityExceeded = true; ++stats.activeModelRefusals; }
    }

    void RetirePositiveCold(uint32_t index, bool refused = false)
    {
        auto& g = groups[index];
        if (!g.positiveCold) return;
        stats.positiveColdFactsBytes -= g.positiveCold->factCharge;
        if (g.charged)
        {
            stats.payloadCapacityCharge -= g.payloadCharge;
            stats.shapeSchedulingCharge -= g.shapeCharge;
        }
        g.shape = nullptr; g.positiveCold.reset(); g.positiveColdRefused = refused;
        g.requestOutstanding = g.charged = g.sourceValidated = false;
        g.payloadCharge = g.shapeCharge = g.revision = 0;
        auto found = std::find(positiveColdWork.begin(), positiveColdWork.end(), index);
        if (found != positiveColdWork.end()) positiveColdWork.erase(found);
        if (positiveColdWork.empty())
        { std::vector<uint32_t>().swap(positiveColdWork); positiveColdCursor = 0; }
    }

    bool HasLease(uint32_t index) const
    {
        return leaseCounts.contains(index);
    }
    bool CanAcquireLease(uint32_t index) const
    {
        return HasLease(index) || leaseCounts.size() < SimulationMaxLeasedPlacements;
    }
    bool AcquireLease(uint32_t index)
    {
        if (!CanAcquireLease(index)) return false;
        ++leaseCounts[index];
        return true;
    }
    void DropLease(uint32_t index)
    {
        const auto found = leaseCounts.find(index);
        if (found != leaseCounts.end())
        {
            if (found->second <= 1) leaseCounts.erase(found);
            else --found->second;
        }
    }
    void DropPending(Region& r)
    {
        for (auto i : r.pending) if (!r.activeSet.contains(i)) DropLease(i);
        r.pending.clear(); r.pendingSet.clear(); r.pendingNew = 0;
        r.plan = {}; r.cell = r.offset = 0; r.planGeneration = 0;
        r.positiveScanComplete = false; r.positiveValidationCursor = 0;
    }
    void DropRegion(Region& r)
    {
        DropPending(r);
        for (auto i : r.active) DropLease(i);
        r.active.clear(); r.activeSet.clear();
    }
    void CompleteReplacement(Region& r, bool completeCoverage = true)
    {
        for (auto i : r.active) if (!r.pendingSet.contains(i)) DropLease(i);
        r.active.swap(r.pending); r.activeSet.swap(r.pendingSet);
        r.pending.clear(); r.pendingSet.clear(); r.pendingNew = 0;
        r.positiveOnly = !completeCoverage;
        r.positiveScanComplete = !completeCoverage;
        r.status = completeCoverage ? SimulationResidencyStatus::Ready : SimulationResidencyStatus::Pending;
    }
};

} // namespace Poseidon::Streaming
