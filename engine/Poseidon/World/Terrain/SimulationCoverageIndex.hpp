#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Poseidon::Streaming
{

using CoveragePoint = std::array<double, 3>;
struct CoverageEnvelope
{
    CoveragePoint min{}, max{};
};

// Leave ample headroom for the engine's float delta/dot/squared-distance intermediates.
// Extremely large but finite metadata cannot certify a finite rejection sphere when those
// intermediates overflow; queries in that numeric domain must refuse, not report clear.
inline double EngineBroadphaseSafeMagnitude()
{
    return std::sqrt(double(std::numeric_limits<float>::max())) / 64;
}

inline bool ValidCoverageEnvelope(const CoverageEnvelope& box)
{
    for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(box.min[axis]) || !std::isfinite(box.max[axis]) || box.min[axis] > box.max[axis])
            return false;
    return true;
}

// Numeric evidence for the EXISTING engine's parent intersection rejection sphere.
// Inputs must come from a fully adapted shape and the final repaired object frame.
// Two covers both static and animated Object::Intersect guards, including their proxy
// recursion. This is not a physical bound for geometry the engine itself rejects,
// nor proof of collision readiness. No raw IR/header/placement approximation is valid here.
inline std::optional<CoverageEnvelope> BuildEngineBroadphaseEnvelope(
    const CoveragePoint& finalPosition, double finalScale, double adaptedBoundingSphere)
{
    if (!std::isfinite(finalScale) || finalScale <= 0 || !std::isfinite(adaptedBoundingSphere) ||
        adaptedBoundingSphere < 0)
        return std::nullopt;
    const double radius = 2 * finalScale * adaptedBoundingSphere;
    if (!std::isfinite(radius) || radius > EngineBroadphaseSafeMagnitude())
        return std::nullopt;
    CoverageEnvelope box;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(finalPosition[axis]) || std::abs(finalPosition[axis]) > EngineBroadphaseSafeMagnitude())
            return std::nullopt;
        const double magnitude = std::abs(finalPosition[axis]) + radius;
        const double roundoff = 32 * std::numeric_limits<float>::epsilon() * std::max(magnitude, 1.0);
        box.min[axis] = finalPosition[axis] - radius - roundoff;
        box.max[axis] = finalPosition[axis] + radius + roundoff;
    }
    if (!ValidCoverageEnvelope(box))
        return std::nullopt;
    return box;
}

enum class CoverageStatus { Ready, Unknown, Invalid, CapacityExceeded };
struct CoverageLookup
{
    CoverageStatus status = CoverageStatus::Invalid;
    std::vector<uint32_t> groups;
};

// Bounded, owner-thread metadata foundation; not a runtime admission service.
// Reset must declare the COMPLETE model-group inventory for this world/content generation.
// A certified group envelope must enclose ALL its placements' engine broadphase spheres.
// It may precede complete per-placement metadata; an uncertified group blocks globally.
// Ready means coverage metadata is complete for this query, NEVER geometry installed/clear.
class SimulationCoverageIndex
{
    struct Group
    {
        std::optional<CoverageEnvelope> envelope;
        bool coverageKnown = false;
    };
    std::vector<Group> _groups;
    uint64_t _generation = 0;
    CoverageStatus _state = CoverageStatus::Invalid;

    static bool Overlaps(const CoverageEnvelope& box, const CoveragePoint& from,
                         const CoveragePoint& to, double radius)
    {
        double begin = 0, end = 1;
        // Slab intersection with an outward-rounded L-infinity swept envelope:
        // conservative for a spherical query, including touching corners/endpoints.
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double magnitude = std::max({std::abs(box.min[axis]), std::abs(box.max[axis]),
                std::abs(from[axis]), std::abs(to[axis]), radius, 1.0});
            const double roundoff = 32 * std::numeric_limits<double>::epsilon() * magnitude;
            const double lo = box.min[axis] - radius - roundoff;
            const double hi = box.max[axis] + radius + roundoff;
            // Overflow may enlarge coverage, never exclude a potential group.
            if (!std::isfinite(lo) || !std::isfinite(hi))
                continue;
            const double delta = to[axis] - from[axis];
            if (delta == 0)
            {
                if (from[axis] < lo || from[axis] > hi)
                    return false;
                continue;
            }
            double a = (lo - from[axis]) / delta, b = (hi - from[axis]) / delta;
            if (a > b)
                std::swap(a, b);
            begin = std::max(begin, a);
            end = std::min(end, b);
            if (begin > end)
                return false;
        }
        return true;
    }

  public:
    CoverageStatus Reset(uint64_t generation, size_t groupCount, size_t maxGroups)
    {
        // Generations are monotonic leases, not reusable world labels. Refused stale reset
        // leaves the current inventory untouched; even cancelled leases cannot be recycled.
        if (generation == 0 || generation <= _generation)
            return CoverageStatus::Invalid;
        _groups.clear();
        _generation = generation;
        _state = CoverageStatus::Invalid;
        if (groupCount > maxGroups || groupCount > std::numeric_limits<uint32_t>::max())
            return _state = CoverageStatus::CapacityExceeded;
        _groups.resize(groupCount);
        return _state = CoverageStatus::Ready;
    }

    void Cancel(uint64_t generation)
    {
        if (generation != _generation)
            return;
        _groups.clear();
        _state = CoverageStatus::Invalid;
    }

    // Replacement coalesces updates for one model group. An invalid replacement removes
    // the old certificate: failed or revised metadata must not leave stale Ready coverage.
    // A different generation cannot mutate this inventory. IDs are fixed dense model slots.
    bool Publish(uint64_t generation, uint32_t group, std::optional<CoverageEnvelope> certifiedEnvelope,
                 bool coverageKnown)
    {
        if (_state != CoverageStatus::Ready || generation != _generation || group >= _groups.size())
            return false;
        if (!certifiedEnvelope || !ValidCoverageEnvelope(*certifiedEnvelope))
        {
            _groups[group] = {};
            return !certifiedEnvelope && !coverageKnown;
        }
        _groups[group] = {certifiedEnvelope, coverageKnown};
        return true;
    }

    CoverageLookup Lookup(uint64_t generation, const CoveragePoint& from, const CoveragePoint& to,
                          double queryRadius, size_t maxScannedGroups, size_t maxMatches) const
    {
        CoverageLookup out;
        if (generation != _generation || _state == CoverageStatus::Invalid ||
            !std::isfinite(queryRadius) || queryRadius < 0 || queryRadius > EngineBroadphaseSafeMagnitude())
            return out;
        for (size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(from[axis]) || !std::isfinite(to[axis]) ||
                std::abs(from[axis]) > EngineBroadphaseSafeMagnitude() ||
                std::abs(to[axis]) > EngineBroadphaseSafeMagnitude() ||
                !std::isfinite(to[axis] - from[axis]))
                return out;
        if (_state == CoverageStatus::CapacityExceeded || _groups.size() > maxScannedGroups)
        {
            out.status = CoverageStatus::CapacityExceeded;
            return out;
        }
        out.status = CoverageStatus::Ready;
        for (size_t i = 0; i < _groups.size(); ++i)
        {
            const Group& group = _groups[i];
            const bool overlaps = group.envelope && Overlaps(*group.envelope, from, to, queryRadius);
            if (!group.envelope || (overlaps && !group.coverageKnown))
            {
                out.status = CoverageStatus::Unknown;
                out.groups.clear();
                return out;
            }
            if (!overlaps)
                continue;
            if (out.groups.size() == maxMatches)
            {
                out.status = CoverageStatus::CapacityExceeded;
                out.groups.clear();
                return out;
            }
            out.groups.push_back(static_cast<uint32_t>(i));
        }
        return out;
    }
};

} // namespace Poseidon::Streaming
