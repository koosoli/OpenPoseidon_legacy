#pragma once

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/SimulationResidency.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <span>
#include <unordered_set>
#include <limits>
#include <algorithm>
#include <cmath>

namespace Poseidon::Streaming
{
// Domain of the existing float-based ObjRadiusRectangle and NearestPoint math.
// Point capsules remain valid for residency, but cannot enter Object::Intersect:
// its NearestPoint divides by the actual float segment length squared.
inline SimulationResidencyStatus SimulationFireBroadphaseStatus(Vector3Par from, Vector3Par to,
    float radius, float invObjectGrid, int objectRange)
{
    using Status = SimulationResidencyStatus;
    if (!std::isfinite(radius) || radius < 0 || radius > EngineBroadphaseSafeMagnitude() ||
        !std::isfinite(invObjectGrid) || invObjectGrid <= 0 || objectRange <= 0) return Status::Invalid;
    for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(from[axis]) || !std::isfinite(to[axis]) ||
            std::abs(double(from[axis])) > EngineBroadphaseSafeMagnitude() ||
            std::abs(double(to[axis])) > EngineBroadphaseSafeMagnitude()) return Status::Invalid;
    const Vector3 delta = to - from;
    const float length2 = delta.SquareSizeInline();
    if (!std::isfinite(length2) || length2 <= 1e-6f) return Status::Invalid;

    // Match legacy float rounding, including its radius+=25 and multiplication
    // before toIntFloor. Checking unrounded double endpoints alone is unsafe.
    float expanded = radius;
    expanded += 25;
    const float coordinates[] = {
        std::min(from.X(), to.X()) - expanded, std::max(from.X(), to.X()) + expanded,
        std::min(from.Z(), to.Z()) - expanded, std::max(from.Z(), to.Z()) + expanded
    };
    int cells[4];
    for (size_t i = 0; i < 4; ++i)
    {
        const float scaled = coordinates[i] * invObjectGrid;
        // Leave rounding headroom near the int conversion limits. Only this
        // wrapper refuses; legacy queries keep their original behaviour.
        if (!std::isfinite(scaled) || double(scaled) < double(std::numeric_limits<int>::min()) + 4096 ||
            double(scaled) > double(std::numeric_limits<int>::max()) - 4096) return Status::Invalid;
        // Legacy floor is nearbyint(x-.5), including its integer-boundary
        // ties-to-even behaviour. Mathematical floor can undercount the scan.
        cells[i] = std::clamp(toIntFloor(scaled), 0, objectRange - 1);
    }
    const uint64_t rectangleCells = uint64_t(cells[1] - cells[0] + 1) * uint64_t(cells[3] - cells[2] + 1);
    // The legacy implementation scans the whole AABB (DDA_OPTIMIZED=0), even
    // if the demand planner's narrow diagonal capsule used very few cells.
    return rectangleCells <= SimulationMaxRegionLeases ? Status::Ready : Status::CapacityExceeded;
}

// Shared transactional merge, used by the owner API and exercised with real
// CollisionBuffers in tests. A refusal NEVER publishes partial contacts. The
// caller must validate complete query coverage, not just these candidate IDs.
template<class LegacyTrace, class TerrainTrace, class Validate, class Budget>
SimulationResidencyStatus AppendSimulationFireContacts(CollisionBuffer& result,
    std::span<Object* const> candidates, Object* with, Object* ignore,
    LegacyTrace&& legacyTrace, TerrainTrace&& terrainTrace, Validate&& validate, Budget&& budget)
{
    using Status = SimulationResidencyStatus;
    auto status = validate();
    if (status != Status::Ready) return status;
    if (candidates.size() > SimulationMaxRegionLeases) return Status::CapacityExceeded;
    if (!budget()) return Status::Pending;

    CollisionBuffer staged;
    legacyTrace(staged);
    if (!budget()) return Status::Pending;
    std::unordered_set<const Object*> contacted;
    for (int i = 0; i < staged.Size(); ++i)
        if (staged[i].object) contacted.insert(staged[i].object.GetRef());
    std::unordered_set<const Object*> visited;
    for (Object* object : candidates)
    {
        if (!budget()) return Status::Pending;
        if (!object) return Status::Unknown;
        if (object == with || object == ignore || !visited.insert(object).second || contacted.contains(object)) continue;
        if (object->GetType() == Temporary || object->GetType() == TypeTempVehicle ||
            (object->GetType() == Network && (!object->GetShape() || object->GetShape()->FindGeometryLevel() < 0))) continue;
        // No origin-grid/25m broadphase filter: membership in the validated
        // query's complete leased inventory supplies the candidate set.
        terrainTrace(staged, *object);
    }
    // Match ObjectCollision's exclusions even when an Intersect returns a child.
    for (int i = staged.Size() - 1; i >= 0; --i)
        if (staged[i].object && (staged[i].object.GetRef() == with || staged[i].object.GetRef() == ignore)) staged.Delete(i);
    status = validate();
    if (status != Status::Ready) return status;
    if (!budget()) return Status::Pending;
    if (staged.Size() > std::numeric_limits<int>::max() - result.Size()) return Status::CapacityExceeded;
    // Reserve once before publishing, rather than allocate midway through the
    // append. No caller prefix is deleted/reordered or deduplicated.
    const int total = result.Size() + staged.Size();
    result.Reserve(total, total);
    for (int i = 0; i < staged.Size(); ++i) result.AddFast(staged[i]);
    return Status::Ready;
}
} // namespace Poseidon::Streaming
