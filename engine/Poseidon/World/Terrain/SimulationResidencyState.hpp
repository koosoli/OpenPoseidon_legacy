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
    struct Region
    {
        OLink<Object> actor;
        CoveragePoint from{}, to{};
        double radius = 0;
        std::array<float, 12> actorFrame{};
        std::array<float, 3> actorSpeed{};
        float actorRadius = 0;
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
    std::unordered_map<const Object*, Region> regions;
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
