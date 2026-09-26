#pragma once

#include <Poseidon/World/Model/Model.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Poseidon::Streaming
{

enum class ModelSourceCoverage { Partial, FullIR };
enum class ModelBoundsState { Unknown, CompleteSource };
enum class ModelBoundsReason : uint32_t
{
    None = 0,
    IncompleteSource = 1 << 0,
    NoVertices = 1 << 1,
    NonFinite = 1 << 2,
    InconsistentPurpose = 1 << 3,
    UnknownPurpose = 1 << 4,
    InvalidFace = 1 << 5,
    UnresolvedProxy = 1 << 6,
    Animation = 1 << 7,
    InconsistentIndex = 1 << 8,
    InvalidTransform = 1 << 9
};

struct SimulationModelBounds
{
    ModelBoundsState state = ModelBoundsState::Unknown;
    uint32_t reasons = 0;
    std::array<double, 3> min{};
    std::array<double, 3> max{};
    uint32_t authoredSimulationLods = 0;
    bool HasReason(ModelBoundsReason reason) const { return (reasons & uint32_t(reason)) != 0; }
};

// Source metadata only. CompleteSource is NOT collision readiness or a validated engine bound:
// adaptation may recenter coordinates, repair placement matrices, synthesize collision geometry,
// or resolve external models. FullIR must be supplied only by a complete parser result, never a
// visual-LOD/proxy-header read. Unknown records retain no usable box. The producer is worker-safe,
// touches no bank/renderer, and retains no model, proxy, or vertex payload.
// Model IR does not retain the complete skeleton/config animation contract. Consequently even
// CompleteSource is only a source-coordinate union, never proof that posed vertices stay inside
// it. Weighted selections (including ODOL's synthesized bone memberships) refuse conservatively;
// unweighted selection/config-driven motion still requires a separate engine-side pose proof.
inline SimulationModelBounds BuildSimulationModelBounds(const Model::Model& model, ModelSourceCoverage coverage)
{
    SimulationModelBounds out;
    auto refuse = [&out](ModelBoundsReason reason) { out.reasons |= uint32_t(reason); };
    if (coverage != ModelSourceCoverage::FullIR)
        refuse(ModelBoundsReason::IncompleteSource);
    if (model.allowAnimation)
        refuse(ModelBoundsReason::Animation);
    bool any = false;
    for (const auto& lod : model.lodLevels)
    {
        const auto& mesh = lod.mesh;
        if (!std::isfinite(lod.resolution))
            refuse(ModelBoundsReason::NonFinite);
        else if (lod.purpose != Model::ClassifyLodResolution(lod.resolution))
            refuse(ModelBoundsReason::InconsistentPurpose);
        if (lod.purpose == Model::LodPurpose::Unknown)
            refuse(ModelBoundsReason::UnknownPurpose);
        switch (lod.purpose)
        {
            case Model::LodPurpose::Geometry:
            case Model::LodPurpose::ViewGeometry:
            case Model::LodPurpose::FireGeometry:
            case Model::LodPurpose::ViewCargoGeometry:
            case Model::LodPurpose::ViewCommanderGeometry:
            case Model::LodPurpose::ViewPilotGeometry:
            case Model::LodPurpose::ViewGunnerGeometry:
            case Model::LodPurpose::FireGunnerGeometry:
            case Model::LodPurpose::Roadway:
                ++out.authoredSimulationLods;
                break;
            default: break;
        }
        // MLOD proxy selections can precede generated Mesh::proxies; inspect both forms.
        if (!mesh.proxies.empty())
            refuse(ModelBoundsReason::UnresolvedProxy);
        for (const auto& selection : mesh.selections)
        {
            if (selection.name.starts_with("proxy:"))
                refuse(ModelBoundsReason::UnresolvedProxy);
            if (!selection.vertexWeights.empty() || !selection.sourceVertexWeights.empty())
                refuse(ModelBoundsReason::Animation);
        }
        if (!mesh.frames.empty())
            refuse(ModelBoundsReason::Animation);

        // Union every source LOD, including drawable geometry. Adapter-generated box geometry
        // and model recentering can depend on visual/other LOD extents, not only a Geometry LOD.
        for (const auto& vertex : mesh.vertices)
        {
            const auto& p = vertex.position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
                refuse(ModelBoundsReason::NonFinite);
                continue;
            }
            const std::array<double, 3> point{p.x, p.y, p.z};
            if (!any)
            {
                out.min = out.max = point;
                any = true;
            }
            else
                for (int axis = 0; axis < 3; ++axis)
                {
                    out.min[axis] = std::min(out.min[axis], point[axis]);
                    out.max[axis] = std::max(out.max[axis], point[axis]);
                }
        }
        for (const auto& face : mesh.triangles)
            for (uint32_t index : face.indices)
                if (index >= mesh.vertices.size())
                    refuse(ModelBoundsReason::InvalidFace);
        for (const auto& face : mesh.quads)
            for (uint32_t index : face.indices)
                if (index >= mesh.vertices.size())
                    refuse(ModelBoundsReason::InvalidFace);
    }
    // ODOL selectors may fall back from fire/view to physical geometry. Validate membership,
    // not equality with just one purpose. Both wire selectors and canonical indices are checked.
    const int indices[] = {model.geometryIdx, model.geometryFireIdx, model.geometryViewIdx,
                           model.geometryViewPilotIdx, model.geometryViewGunnerIdx,
                           model.geometryViewCommanderIdx, model.geometryViewCargoIdx, model.roadwayIdx,
                           model.geometryLODIndex, model.fireGeometryLODIndex, model.viewGeometryLODIndex,
                           model.viewPilotLODIndex, model.viewGunnerLODIndex, model.viewCommanderLODIndex,
                           model.viewCargoLODIndex, model.roadwayLODIndex};
    for (int index : indices)
    {
        if (index == -1)
            continue;
        if (index < -1 || size_t(index) >= model.lodLevels.size())
        {
            refuse(ModelBoundsReason::InconsistentIndex);
            continue;
        }
        switch (model.lodLevels[index].purpose)
        {
            case Model::LodPurpose::Geometry:
            case Model::LodPurpose::ViewGeometry:
            case Model::LodPurpose::FireGeometry:
            case Model::LodPurpose::ViewCargoGeometry:
            case Model::LodPurpose::ViewCommanderGeometry:
            case Model::LodPurpose::ViewPilotGeometry:
            case Model::LodPurpose::ViewGunnerGeometry:
            case Model::LodPurpose::FireGunnerGeometry:
            case Model::LodPurpose::Roadway: break;
            default: refuse(ModelBoundsReason::InconsistentIndex); break;
        }
    }
    if (!any)
        refuse(ModelBoundsReason::NoVertices);
    if (out.reasons == 0)
        out.state = ModelBoundsState::CompleteSource;
    else
        out.min = out.max = {};
    // authoredSimulationLods==0 never means collision-free: adapters may synthesize geometry.
    return out;
}

// Requires the caller to supply the FINAL object-local transform, after adapter coordinate
// mapping and ObjectCreate's orientation/scale repair. Raw WRP placement rows are not that proof.
// This transforms source evidence; it does not upgrade CompleteSource to authoritative collision.
inline SimulationModelBounds TransformSimulationModelBounds(const SimulationModelBounds& local,
                                                            const Model::Matrix4x3& finalTransform)
{
    SimulationModelBounds out = local;
    if (local.state != ModelBoundsState::CompleteSource || local.reasons != 0)
    {
        out.state = ModelBoundsState::Unknown;
        out.min = out.max = {};
        return out;
    }
    auto invalid = [&out](ModelBoundsReason reason)
    {
        out.state = ModelBoundsState::Unknown;
        out.reasons |= uint32_t(reason);
        out.min = out.max = {};
        return out;
    };
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(local.min[axis]) || !std::isfinite(local.max[axis]) || local.min[axis] > local.max[axis])
            return invalid(ModelBoundsReason::NonFinite);
        for (int col = 0; col < 4; ++col)
            if (!std::isfinite(finalTransform.m[axis][col]))
                return invalid(ModelBoundsReason::InvalidTransform);
        double low = finalTransform.m[axis][3], high = low;
        double magnitude = std::abs(low);
        for (int col = 0; col < 3; ++col)
        {
            const double a = finalTransform.m[axis][col] * local.min[col];
            const double b = finalTransform.m[axis][col] * local.max[col];
            low += std::min(a, b);
            high += std::max(a, b);
            magnitude += std::max(std::abs(a), std::abs(b));
        }
        if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(magnitude) ||
            magnitude > std::numeric_limits<float>::max())
            return invalid(ModelBoundsReason::NonFinite);
        // Actual engine transforms use float arithmetic. Cover its roundoff as well as ours.
        const double roundoff = 16 * std::numeric_limits<float>::epsilon() * std::max(magnitude, 1.0);
        out.min[axis] = low - roundoff;
        out.max[axis] = high + roundoff;
    }
    return out;
}

} // namespace Poseidon::Streaming
