#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/StaticSourceAudit.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace Poseidon::Streaming
{
enum class StaticSourceEnvelopeState { Unknown, SourceEvidence };
enum class StaticSourceCoverage { Partial, FullCompiledIR };
enum class StaticSourceEnvelopeReason : uint32_t
{
    None = 0, Generation = 1 << 0, Format = 1 << 1, Capacity = 1 << 2,
    NonFinite = 1 << 3, NoVertices = 1 << 4, MissingRole = 1 << 5,
    Proxy = 1 << 6, Animation = 1 << 7, LandConform = 1 << 8,
    InvalidFace = 1 << 9, InconsistentPurpose = 1 << 10, IncompleteSource = 1 << 11
};

// Immutable evidence for one complete, compiled ODOL IR generation. This is
// neither Ready nor a final ObjectPlain/constructor certificate: CfgModels can
// override class, constructors can substitute shapes, and live geometry can
// mutate. No bank, config, renderer, proxy resolver or Object is consulted here.
class StaticSourceEnvelope
{
    StaticSourceEnvelopeState _state = StaticSourceEnvelopeState::Unknown;
    uint32_t _reasons = 0;
    uint64_t _generation = 0, _payloadBytes = 0;
    float _radius = 0;
    friend StaticSourceEnvelope BuildStaticSourceEnvelope(const Model::Model&, uint64_t, StaticSourceCoverage);
public:
    StaticSourceEnvelopeState State() const { return _state; }
    uint64_t SourceGeneration() const { return _generation; }
    uint64_t PayloadBytes() const { return _payloadBytes; }
    bool HasReason(StaticSourceEnvelopeReason reason) const { return (_reasons & uint32_t(reason)) != 0; }
    std::optional<float> RadiusForGeneration(uint64_t generation) const
    {
        if (_state != StaticSourceEnvelopeState::SourceEvidence || !generation || generation != _generation)
            return std::nullopt;
        return _radius;
    }
};

namespace StaticSourceDetail
{
inline constexpr uint64_t MaxPayloadBytes = 4 * 1024 * 1024;
inline constexpr size_t MaxLods = 64;

// Capacity charge includes spare vector/string storage. Every outer collection
// is charged BEFORE walking its children, so an oversized container cannot
// cause an unbounded validation walk. Decoder/allocator scratch is excluded;
// this is a scheduling limit, not an RSS measurement. No payload is retained.
inline std::optional<uint64_t> BoundedPayloadBytes(const Model::Model& model)
{
    uint64_t bytes = 0;
    const auto add = [&](uint64_t n) { if (n > MaxPayloadBytes - bytes) return false; bytes += n; return true; };
    const auto vector = [&](const auto& v)
    {
        using Item = typename std::decay_t<decltype(v)>::value_type;
        return v.capacity() <= (MaxPayloadBytes - bytes) / sizeof(Item) && add(uint64_t(v.capacity()) * sizeof(Item));
    };
    const auto string = [&](const std::string& s)
    { return s.capacity() < MaxPayloadBytes && add(uint64_t(s.capacity()) + 1); };
    if (model.lodLevels.size() > MaxLods || !add(sizeof(model)) || !vector(model.lodLevels) || !vector(model.massArray)) return {};
    for (const auto* s : {&model.sourcePath, &model.sourceFormat, &model.memory, &model.geometry, &model.geometryFire,
         &model.geometryView, &model.geometryViewPilot, &model.geometryViewGunner, &model.geometryViewCommander,
         &model.geometryViewCargo, &model.landContact, &model.roadway, &model.paths, &model.hitpoints, &model.remarks})
        if (!string(*s)) return {};
    if (model.metadata.size() > (MaxPayloadBytes - bytes) / sizeof(decltype(model.metadata)::value_type) ||
        !add(uint64_t(model.metadata.size()) * sizeof(decltype(model.metadata)::value_type))) return {};
    for (const auto& [key, value] : model.metadata) if (!string(key) || !string(value)) return {};
    for (const auto& lod : model.lodLevels)
    {
        const auto& m = lod.mesh;
        if (!string(lod.sourceEncoding) || !string(lod.sourceWinding) || !vector(lod.uvChannels) ||
            !vector(m.vertices) || !vector(m.triangles) || !vector(m.quads) || !vector(m.materials) ||
            !vector(m.selections) || !vector(m.properties) || !vector(m.proxies) || !vector(m.sections) ||
            !vector(m.frames) || !vector(m.vertexMass) || !vector(m.edges.mlodIndices) || !vector(m.edges.vertexIndices)) return {};
        for (const auto& channel : lod.uvChannels) if (!vector(channel.faceVertexUVs)) return {};
        for (const auto& material : m.materials)
        {
            if (!string(material.name) || !string(material.texturePath) || !string(material.materialPath) || !vector(material.embeddedStages)) return {};
            for (const auto& stage : material.embeddedStages) if (!string(stage.texturePath)) return {};
        }
        for (const auto& selection : m.selections)
            if (!string(selection.name) || !vector(selection.vertexIndices) || !vector(selection.vertexWeights) ||
                !vector(selection.sourceVertexWeights) || !vector(selection.triangleIndices) ||
                !vector(selection.faceSelectionOffsets) || !vector(selection.sectionIndices)) return {};
        for (const auto& property : m.properties) if (!string(property.name) || !string(property.value)) return {};
        for (const auto& proxy : m.proxies) if (!string(proxy.name)) return {};
        for (const auto& frame : m.frames) if (!vector(frame.positions)) return {};
    }
    return bytes;
}
}

// The adapter copies ODOL vertices unchanged; its static tail removes/reorders
// LODs and optionally reverses axes. Both preserve containment in this sphere.
// All LODs count, even vertices unused by faces. Explicit physical geometry
// excludes adapter box synthesis. Land-conforming/posed/proxy coordinates need
// additional world-dependent bounds and are refused rather than guessed.
inline StaticSourceEnvelope BuildStaticSourceEnvelope(const Model::Model& model, uint64_t generation, StaticSourceCoverage coverage)
{
    StaticSourceEnvelope out;
    out._generation = generation;
    const auto refuse = [&](StaticSourceEnvelopeReason reason) { out._reasons |= uint32_t(reason); };
    if (!generation) refuse(StaticSourceEnvelopeReason::Generation);
    if (coverage != StaticSourceCoverage::FullCompiledIR) refuse(StaticSourceEnvelopeReason::IncompleteSource);
    if (model.sourceFormat != "ODOL") refuse(StaticSourceEnvelopeReason::Format);
    if (model.sourceFormat == "ODOL" && !Model::HasStaticOdolSourceAudit(model))
        refuse(StaticSourceEnvelopeReason::IncompleteSource);
    const auto payload = StaticSourceDetail::BoundedPayloadBytes(model);
    if (!payload) { refuse(StaticSourceEnvelopeReason::Capacity); return out; }
    out._payloadBytes = *payload;
    if (model.allowAnimation) refuse(StaticSourceEnvelopeReason::Animation);
    if (!std::isfinite(model.boundingSphere.radius) || model.boundingSphere.radius < 0) refuse(StaticSourceEnvelopeReason::NonFinite);
    std::array<double, 3> magnitude{};
    std::array<bool, 3> roles{};
    bool any = false;
    constexpr uint32_t landMask = uint32_t(Model::VertexFlags::LandMask);
    for (const auto& lod : model.lodLevels)
    {
        const auto& mesh = lod.mesh;
        if (!std::isfinite(lod.resolution) || lod.purpose != Model::ClassifyLodResolution(lod.resolution))
            refuse(StaticSourceEnvelopeReason::InconsistentPurpose);
        if (!mesh.vertices.empty() && (!mesh.triangles.empty() || !mesh.quads.empty()))
        {
            if (lod.purpose == Model::LodPurpose::Geometry) roles[0] = true;
            if (lod.purpose == Model::LodPurpose::FireGeometry) roles[1] = true;
            if (lod.purpose == Model::LodPurpose::ViewGeometry) roles[2] = true;
        }
        if (!mesh.proxies.empty()) refuse(StaticSourceEnvelopeReason::Proxy);
        if (!mesh.frames.empty()) refuse(StaticSourceEnvelopeReason::Animation);
        for (const auto& selection : mesh.selections)
        {
            if (selection.name.starts_with("proxy:")) refuse(StaticSourceEnvelopeReason::Proxy);
            if (!selection.vertexWeights.empty() || !selection.sourceVertexWeights.empty()) refuse(StaticSourceEnvelopeReason::Animation);
        }
        for (const auto& vertex : mesh.vertices)
        {
            any = true;
            if (uint32_t(vertex.flags) & landMask) refuse(StaticSourceEnvelopeReason::LandConform);
            const std::array<double, 3> p{vertex.position.x, vertex.position.y, vertex.position.z};
            for (size_t axis = 0; axis < 3; ++axis)
                if (!std::isfinite(p[axis])) refuse(StaticSourceEnvelopeReason::NonFinite);
                else magnitude[axis] = std::max(magnitude[axis], std::abs(p[axis]));
        }
        for (const auto& face : mesh.triangles)
            for (auto index : face.indices) if (index >= mesh.vertices.size()) refuse(StaticSourceEnvelopeReason::InvalidFace);
        for (const auto& face : mesh.quads)
            for (auto index : face.indices) if (index >= mesh.vertices.size()) refuse(StaticSourceEnvelopeReason::InvalidFace);
    }
    if (!any) refuse(StaticSourceEnvelopeReason::NoVertices);
    if (!roles[0] || !roles[1] || !roles[2]) refuse(StaticSourceEnvelopeReason::MissingRole);
    // Hypot avoids intermediate float-square overflow. A small double margin
    // covers evaluation roundoff; nextafter guarantees outward float rounding.
    const double vertexRadius = std::hypot(magnitude[0], magnitude[1], magnitude[2]);
    const double radius = std::max(vertexRadius, double(model.boundingSphere.radius));
    const double expanded = radius * (1 + 16 * std::numeric_limits<double>::epsilon());
    if (!std::isfinite(expanded) || expanded >= std::numeric_limits<float>::max()) refuse(StaticSourceEnvelopeReason::NonFinite);
    if (!out._reasons)
    {
        out._radius = std::nextafter(float(expanded), std::numeric_limits<float>::infinity());
        if (!std::isfinite(out._radius)) { out._radius = 0; refuse(StaticSourceEnvelopeReason::NonFinite); }
        else out._state = StaticSourceEnvelopeState::SourceEvidence;
    }
    return out;
}
}
