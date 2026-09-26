#pragma once

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/ModelMemory.hpp>
#include <Poseidon/World/Terrain/SimulationCoverageIndex.hpp>
#include <Poseidon/World/Terrain/WorldObjectPlacementFrame.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace Poseidon::Streaming
{

inline constexpr size_t SimulationMaxModels = 256;
inline constexpr size_t SimulationMaxPlacements = 1000000;
inline constexpr size_t SimulationMaxRegions = 128;
inline constexpr size_t SimulationMaxRegionLeases = 4096;
inline constexpr size_t SimulationMaxLeasedPlacements = 16384;
inline constexpr uint64_t SimulationMaxModelPayload = 4 * 1024 * 1024;
inline constexpr uint64_t SimulationMaxPayloadCharge = 64 * 1024 * 1024;
inline constexpr uint64_t SimulationMaxShapeCharge = 256 * 1024 * 1024;

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
