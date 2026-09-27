#pragma once
#include <Poseidon/World/Terrain/StaticSourceEnvelope.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/ModelMemory.hpp>
#include <Poseidon/World/Model/StaticSourceAudit.hpp>
#include <Poseidon/World/Terrain/SimulationCoverageIndex.hpp>
#include <Poseidon/World/Terrain/WorldObjectPlacementFrame.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>

namespace Poseidon::Streaming
{

inline constexpr size_t SimulationMaxModels = 256;
inline constexpr size_t SimulationMaxRegions = 128;
inline constexpr size_t SimulationMaxRegionLeases = 4096;
inline constexpr size_t SimulationMaxLeasedPlacements = 16384;
inline constexpr size_t SimulationMaxWatchedPlacements = 16384;
inline constexpr uint64_t SimulationMaxModelPayload = 4 * 1024 * 1024;
inline constexpr uint64_t SimulationMaxPayloadCharge = 64 * 1024 * 1024;
inline constexpr uint64_t SimulationMaxShapeCharge = 256 * 1024 * 1024;

// Optional modern contact work shares one consumption allowance between joined
// owner Pump boundaries. This is a soft elapsed-work limit: an indivisible call
// can overshoot, and fixed caller ordering provides no actor fairness guarantee.
class SimulationFireCollisionConsumption
{
public:
    static constexpr size_t MaxAttempts = 4;
    static constexpr double MaxMilliseconds = 2.0;
    void Reset() { _attempts = 0; _milliseconds = 0; }
    bool TryBegin()
    {
        if (_attempts >= MaxAttempts || _milliseconds >= MaxMilliseconds) return false;
        ++_attempts;
        return true;
    }
    void Charge(double milliseconds)
    {
        // A broken/negative clock observation fails closed, never lends credit.
        if (!std::isfinite(milliseconds) || milliseconds < 0) _milliseconds = MaxMilliseconds;
        else _milliseconds = std::min(MaxMilliseconds, _milliseconds + milliseconds);
    }
    size_t Attempts() const { return _attempts; }
    double ChargedMilliseconds() const { return _milliseconds; }
private:
    size_t _attempts = 0;
    double _milliseconds = 0;
};

// Owner-only lexical charge, including non-Ready returns and exceptions. Now
// must be nonthrowing and return a steady-clock time_point; no callback escapes.
template<class Now>
class SimulationFireCollisionCharge
{
public:
    SimulationFireCollisionCharge(SimulationFireCollisionConsumption& budget, Now now)
        : _budget(budget), _now(now), _start(_now()) {}
    ~SimulationFireCollisionCharge()
    {
        _budget.Charge(std::chrono::duration<double, std::milli>(_now() - _start).count());
    }
    SimulationFireCollisionCharge(const SimulationFireCollisionCharge&) = delete;
    SimulationFireCollisionCharge& operator=(const SimulationFireCollisionCharge&) = delete;
private:
    SimulationFireCollisionConsumption& _budget;
    Now _now;
    std::chrono::steady_clock::time_point _start;
};

// Explicit Fire interest is renewed only by an accepted owner queue operation.
// Polling, collision validation and automatic actor visits are not heartbeats.
// This bounds abandoned query leases, not the global metadata producer's work.
class SimulationFireHeartbeat
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto Grace = std::chrono::seconds(2);
    void Renew(Clock::time_point now)
    {
        _deadline = now > Clock::time_point::max() - Grace ? Clock::time_point::max() : now + Grace;
        _renewed = true;
    }
    bool Expired(Clock::time_point now) const { return !_renewed || now >= _deadline; }
private:
    Clock::time_point _deadline{};
    bool _renewed = false;
};

// Automatic neighbourhoods age by actually visited registry slots, not owner
// pumps: the shared time budget can permit fewer than the nominal 32 visits.
// This is a grace period, not a proof of complete registry traversal/membership.
inline uint64_t AdvanceSimulationActorVisit(uint64_t visits)
{
    return visits == std::numeric_limits<uint64_t>::max() ? visits : visits + 1;
}

inline bool SimulationAutomaticRegionExpired(bool automatic, bool actorAlive,
    size_t totalActors, uint64_t visits, uint64_t lastVisit)
{
    if (!actorAlive) return true;
    if (!automatic) return false; // Explicit-purpose lifetime is unchanged.
    if (!totalActors) return true;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    if (visits == maximum || visits < lastVisit) return false;
    const uint64_t actors = uint64_t(totalActors);
    const uint64_t batches = actors / 32 + (actors % 32 != 0);
    if (batches > (maximum - 8) / 2) return false;
    const uint64_t pumps = batches * 2 + 8;
    const uint64_t batchSize = std::min(uint64_t(32), actors);
    if (pumps > maximum / batchSize) return false;
    // Preserve the former grace at full batch progress; sparse/zero-progress
    // pumps cannot expire a neighbourhood before its normal revisit.
    return visits - lastVisit > pumps * batchSize;
}

// Convex swept L-infinity capsule containment, not endpoint-AABB containment.
// Both endpoint boxes must fit; convexity then covers the complete requested
// sweep. Conservative roundoff shrinks available clearance. Identical centre
// lines (including reversed endpoints/points) nest exactly for any smaller
// radius, so they do not require numerical clearance erosion. Fire callers
// should queue/status using double(actualEngineFloatRadius): a decimal double
// such as .1 can be smaller than the engine's converted float .1f.
inline bool SimulationCapsuleContains(const CoveragePoint& from, const CoveragePoint& to, double radius,
    const CoveragePoint& queryFrom, const CoveragePoint& queryTo, double queryRadius)
{
    if (!std::isfinite(radius) || radius < 0 || radius > EngineBroadphaseSafeMagnitude() || !std::isfinite(queryRadius) || queryRadius < 0 || queryRadius > radius)
        return false;
    double magnitude = std::max({radius, queryRadius, 1.0});
    for (size_t axis = 0; axis < 3; ++axis)
    {
        for (double value : {from[axis], to[axis], queryFrom[axis], queryTo[axis]})
        {
            if (!std::isfinite(value) || std::abs(value) > EngineBroadphaseSafeMagnitude()) return false;
            magnitude = std::max(magnitude, std::abs(value));
        }
    }
    if ((from == queryFrom && to == queryTo) || (from == queryTo && to == queryFrom))
        return true;
    const double margin = 32 * std::numeric_limits<float>::epsilon() * magnitude;
    const double available = radius - queryRadius - margin;
    if (available < 0) return false;
    const auto containsPoint = [&](const CoveragePoint& point)
    {
        double begin = 0, end = 1;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double delta = to[axis] - from[axis];
            const double low = point[axis] - available, high = point[axis] + available;
            if (delta == 0)
            {
                if (from[axis] < low || from[axis] > high) return false;
                continue;
            }
            double a = (low - from[axis]) / delta, b = (high - from[axis]) / delta;
            if (a > b) std::swap(a, b);
            begin = std::max(begin, a); end = std::min(end, b);
            if (begin > end) return false;
        }
        return true;
    };
    return containsPoint(queryFrom) && containsPoint(queryTo);
}

enum class SimulationFactoryRoute { Plain, Road, Unsupported };

// Exact case-sensitive branches of NewObject, not a filename/asset whitelist.
// Even a current config type miss is unsupported: the type bank has no epoch.
inline SimulationFactoryRoute SimulationRoute(std::string_view className)
{
    if (className == "road") return SimulationFactoryRoute::Road;
    if (className == "forest" || className == "house" || className == "vehicle" ||
        className == "church" || className == "streetlamp")
        return SimulationFactoryRoute::Unsupported;
    return SimulationFactoryRoute::Plain;
}

inline Matrix4 SimulationPlacementFrame(const std::array<float, 12>& rows)
{
    Matrix4 out;
    out.SetDirectionAside(Vector3(rows[0], rows[1], rows[2]));
    out.SetDirectionUp(Vector3(rows[3], rows[4], rows[5]));
    out.SetDirection(Vector3(rows[6], rows[7], rows[8]));
    out.SetPosition(Vector3(rows[9], rows[10], rows[11]));
    return out;
}

inline std::array<float, 12> SimulationFrameSample(const Matrix4& frame)
{
    return {frame.DirectionAside().X(), frame.DirectionAside().Y(), frame.DirectionAside().Z(),
        frame.DirectionUp().X(), frame.DirectionUp().Y(), frame.DirectionUp().Z(),
        frame.Direction().X(), frame.Direction().Y(), frame.Direction().Z(),
        frame.Position().X(), frame.Position().Y(), frame.Position().Z()};
}

inline bool ValidSimulationFrame(const Matrix4& frame)
{
    const auto f = SimulationFrameSample(frame);
    for (float value : f) if (!std::isfinite(value)) return false;
    const double det = double(f[0]) * (double(f[4]) * f[8] - double(f[5]) * f[7]) -
        double(f[3]) * (double(f[1]) * f[8] - double(f[2]) * f[7]) +
        double(f[6]) * (double(f[1]) * f[5] - double(f[2]) * f[4]);
    return std::isfinite(det) && det != 0 && std::isfinite(frame.Scale()) && frame.Scale() > 0;
}

// All eligible factory routes retain this shape and use non-forest repair.
// Validate every possible Primary/Network x keep-up case, then use the maximum
// exact engine scalar. No duplicated normalization or guessed scale factor.
inline std::optional<double> SimulationPlacementMaximumScale(const std::array<float, 12>& rows)
{
    const Matrix4 raw = SimulationPlacementFrame(rows);
    if (!ValidSimulationFrame(raw)) return std::nullopt;
    double maximum = 0;
    for (bool network : {false, true})
        for (bool keepUp : {false, true})
        {
            const auto repaired = RepairWorldObjectPlacementFrame(raw, false, network, keepUp);
            if (!ValidSimulationFrame(repaired)) return std::nullopt;
            maximum = std::max(maximum, double(repaired.Scale()));
        }
    return maximum;
}

// Pre-tail refusal is essential: CPU-only rendering policy does not bound
// synchronous recursive proxy loading. Binary Component selection memberships
// in MLOD are ordinary static convex components, not skinning weights.
inline bool SimulationSourceAdmissible(const Model::Model& model)
{
    if ((model.sourceFormat != "MLOD" && model.sourceFormat != "ODOL") ||
        model.allowAnimation || model.lodLevels.empty() || model.lodLevels.size() > MAX_LOD_LEVELS)
        return false;
    if (model.sourceFormat == "ODOL" && !Model::HasStaticOdolSourceAudit(model)) return false;
    for (const auto& lod : model.lodLevels)
    {
        if (!lod.mesh.proxies.empty() || !lod.mesh.frames.empty()) return false;
        for (const auto& selection : lod.mesh.selections)
        {
            if (selection.name.starts_with("proxy:") || !selection.vertexWeights.empty()) return false;
            for (auto weight : selection.sourceVertexWeights) if (weight != 1) return false;
        }
        for (const auto& v : lod.mesh.vertices)
            if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z))
                return false;
    }
    return true;
}

inline bool SimulationShapeAdmissible(LODShapeWithShadow& shape)
{
    if (shape.QueryPolicyRevision() == 0 || shape.GetAllowAnimation() ||
        SimulationRoute(static_cast<const char*>(shape.GetPropertyClass())) == SimulationFactoryRoute::Unsupported ||
        !std::isfinite(shape.BoundingSphere()) || shape.BoundingSphere() < 0)
        return false;
    for (int i = 0; i < shape.NLevels(); ++i)
        if (auto* level = shape.Level(i); level && (level->NProxies() || level->NAnimationPhases())) return false;
    const std::array<int, 3> roles = {shape.FindGeometryLevel(), shape.FindFireGeometryLevel(), shape.FindViewGeometryLevel()};
    const std::array<Model::LodPurpose, 3> purposes = {Model::LodPurpose::Geometry,
        Model::LodPurpose::FireGeometry, Model::LodPurpose::ViewGeometry};
    for (size_t i = 0; i < roles.size(); ++i)
    {
        const int role = roles[i];
        // Engine fallback aliases are usable for legacy queries, but this first
        // coverage contract intentionally requires authored, explicit roles.
        if (role < 0 || role >= shape.NLevels() || Model::ClassifyLodResolution(shape.Resolution(role)) != purposes[i]) return false;
        auto* level = shape.Level(role);
        auto* components = shape.GetConvexComponents(role);
        if (!level || level->NPos() == 0 || level->NFaces() == 0 || !components || components->Size() == 0)
            return false;
    }
    return true;
}

// Source evidence bounds ONLY the fresh static empty-class ODOL adapter path.
// Callers must separately bind the exact parse/converted job and refuse bank
// aliases, probe live config in this owner operation, and reconcile placement.
inline bool PositiveColdShapeMatchesSource(LODShapeWithShadow& shape,
    const StaticSourceEnvelope& source, uint64_t generation)
{
    const auto radius = source.RadiusForGeneration(generation);
    return radius && *shape.GetPropertyClass() == 0 && SimulationShapeAdmissible(shape) &&
        shape.BoundingSphere() <= *radius;
}

// Visible owned element bytes only, not capacity/RSS: no public parent-owned
// capacity estimator exists for all Shape buffers. Actual spare allocation,
// materials, textures, convex scratch and decoder scratch are NOT measured.
inline uint64_t SimulationShapeElementBytes(const LODShapeWithShadow& shape)
{
    uint64_t bytes = sizeof(LODShapeWithShadow);
    for (int i = 0; i < shape.NLevels(); ++i)
        if (auto* level = shape.Level(i))
            bytes += sizeof(Shape) + uint64_t(level->NPos()) * sizeof(V3) + level->FacesRawSize();
    return bytes;
}

} // namespace Poseidon::Streaming
