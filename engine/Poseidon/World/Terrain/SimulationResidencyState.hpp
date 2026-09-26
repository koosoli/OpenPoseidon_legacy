#pragma once

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/SimulationResidency.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/World/Terrain/SimulationDemand.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>

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
    struct Group
    {
        std::vector<uint32_t> members;
        CoverageEnvelope origins{};
        bool validRows = true, sourceValidated = false, charged = false;
        double maximumScale = 0;
        Ref<LODShapeWithShadow> shape;
        uint64_t revision = 0, payloadCharge = 0, shapeCharge = 0;
        float radius = 0;
        SimulationFactoryRoute route = SimulationFactoryRoute::Unsupported;
        ModelState status = ModelState::Waiting;
    };
    struct Entry
    {
        OLink<Object> object;
        Link<LODShape> shape;
        std::array<float, 12> frame{};
        float radius = 0;
    };
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
        uint64_t seen = 0, planGeneration = 0;
        SimulationResidencyStatus status = SimulationResidencyStatus::Pending;
        SimulationDemand plan;
        size_t cell = 0, offset = 0, pendingNew = 0;
        std::vector<uint32_t> active, pending;
        std::unordered_set<uint32_t> activeSet, pendingSet;
    };

    SimulationResidencyStats stats;
    std::vector<Group> groups;
    std::vector<uint32_t> leaseCounts;
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
    bool inventoryComplete = false, inventoryInvalid = false;
    double placementPadding = 0;

    void DropLease(uint32_t index)
    {
        if (index < leaseCounts.size() && leaseCounts[index]) --leaseCounts[index];
    }
    void DropPending(Region& r)
    {
        for (auto i : r.pending) if (!r.activeSet.contains(i)) DropLease(i);
        r.pending.clear(); r.pendingSet.clear(); r.pendingNew = 0;
        r.plan = {}; r.cell = r.offset = 0; r.planGeneration = 0;
    }
    void DropRegion(Region& r)
    {
        DropPending(r);
        for (auto i : r.active) DropLease(i);
        r.active.clear(); r.activeSet.clear();
    }
    void CompleteReplacement(Region& r)
    {
        for (auto i : r.active) if (!r.pendingSet.contains(i)) DropLease(i);
        r.active.swap(r.pending); r.activeSet.swap(r.pendingSet);
        r.pending.clear(); r.pendingSet.clear(); r.pendingNew = 0;
        r.status = SimulationResidencyStatus::Ready;
    }
};

} // namespace Poseidon::Streaming
