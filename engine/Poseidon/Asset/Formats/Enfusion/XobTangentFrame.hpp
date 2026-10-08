#pragma once
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODTangentFrames.hpp>
#include <cstdlib>
#include <cstring>

namespace Poseidon::Asset::Formats::Enfusion
{
inline constexpr const char* kXobTangentProducerKey = "xobAuthoredUv0TangentProducer";
inline constexpr const char* kXobTangentProducer = "XOB_TSPACE_UV0_V3";
inline void MarkXobAuthoredTangentProducer(Poseidon::Model::Model& model)
{
    // Container identity controls the modern adapter and streaming routes.
    // Producer provenance is independent and survives the existing blob metadata.
    if (model.sourceFormat == "MLOD") model.metadata[kXobTangentProducerKey] = kXobTangentProducer;
}
inline bool HasXobAuthoredTangentProducer(const Poseidon::Model::Model& model)
{
    const auto producer = model.metadata.find(kXobTangentProducerKey);
    return model.sourceFormat == "MLOD" && producer != model.metadata.end() &&
           producer->second == kXobTangentProducer;
}
inline bool ValidXobAuthoredTangentVertex(const Poseidon::Model::Vertex& vertex)
{
    const auto& n = vertex.normal;
    const float n2 = n.x*n.x + n.y*n.y + n.z*n.z;
    return std::isfinite(n2) && n2 > 1e-8f &&
           MLOD::ValidTangentFrame({vertex.tangent,vertex.binormal,vertex.hasTangentFrame});
}
inline bool HasXobAuthoredTangentFrame(const Poseidon::Model::Model& model, const Poseidon::Model::Mesh& mesh)
{
    if (!HasXobAuthoredTangentProducer(model) || mesh.vertices.empty()) return false;
    for (const auto& vertex : mesh.vertices)
        if (!ValidXobAuthoredTangentVertex(vertex)) return false;
    return true;
}
inline bool XobAuthoredTangentPreviewEnabled()
{
    for (const char* key : {"WGR_REFORGER_TANGENT_PREVIEW", "WGR_REFORGER_NORMAL_PREVIEW",
                            "WGR_REFORGER_LAYER_ORDER_PREVIEW", "WGR_REFORGER_PBR_PREVIEW"})
    {
        const char* value = std::getenv(key);
        if (!value || std::strcmp(value, "1") != 0) return false;
    }
    return true;
}
inline MLOD::TangentFrame XobCornerTangentFrame(const XobPart& part, size_t renderVertex,
                                              size_t sourceVertex, int renderUvWord)
{
    if (part.skinned || renderUvWord != 0 || renderVertex >= part.tangents.size() ||
        renderVertex >= part.bitangents.size() || renderVertex >= part.renderSource.size() ||
        part.renderSource[renderVertex] != sourceVertex) return {};
    const auto& t = part.tangents[renderVertex];
    const auto& b = part.bitangents[renderVertex];
    MLOD::TangentFrame frame{{t.x,t.y,t.z}, {b.x,b.y,b.z}, true};
    return MLOD::ValidTangentFrame(frame) ? frame : MLOD::TangentFrame{};
}
} // namespace Poseidon::Asset::Formats::Enfusion
