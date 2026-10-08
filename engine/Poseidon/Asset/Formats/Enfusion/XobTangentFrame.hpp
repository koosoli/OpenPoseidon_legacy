#pragma once
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODTangentFrames.hpp>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>

namespace Poseidon::Asset::Formats::Enfusion
{
inline constexpr const char* kXobTangentProducerKey = "xobAuthoredUv0TangentProducer";
inline constexpr const char* kXobTangentProducer = "XOB_TSPACE_UV0_V4";
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
// List A and list B can optimize triangle order independently. Match oriented
// source triangles, retaining list A's geometry and corner order. Ambiguous
// duplicates and degenerate triangles have no uniquely attributable frame.
inline std::vector<uint32_t> XobAuthoredRenderCornerMap(const XobPart& part)
{
    if (part.skinned || part.indices.empty() || part.indices.size() % 3 != 0 ||
        part.renderIndices.size() != part.indices.size()) return {};
    struct Triangle
    {
        std::array<uint32_t,3> key;
        size_t offset;
        unsigned rotation;
    };
    std::vector<Triangle> source, render;
    source.reserve(part.indices.size()/3);
    render.reserve(part.indices.size()/3);
    const auto triangle = [](std::array<uint32_t,3> key, size_t offset) {
        unsigned rotation=0;
        for (unsigned r=1;r<3;++r)
            if (std::array<uint32_t,3>{key[r],key[(r+1)%3],key[(r+2)%3]} <
                std::array<uint32_t,3>{key[rotation],key[(rotation+1)%3],key[(rotation+2)%3]}) rotation=r;
        return Triangle{{key[rotation],key[(rotation+1)%3],key[(rotation+2)%3]},offset,rotation};
    };
    for (size_t t=0;t<part.indices.size();t+=3)
    {
        std::array<uint32_t,3> a{},b{};
        for (unsigned c=0;c<3;++c)
        {
            a[c]=part.indices[t+c];
            const size_t ri=part.renderIndices[t+c];
            if (a[c]>=part.positions.size() || ri>=part.renderSource.size() ||
                part.renderSource[ri]>=part.positions.size()) return {};
            b[c]=part.renderSource[ri];
        }
        source.push_back(triangle(a,t));
        render.push_back(triangle(b,t));
    }
    const auto less=[](const Triangle& a,const Triangle& b) {return a.key<b.key;};
    std::sort(source.begin(),source.end(),less);
    std::sort(render.begin(),render.end(),less);
    for (size_t i=0;i<source.size();++i)
        if (source[i].key!=render[i].key) return {}; // winding/multiplicity must match
    std::vector<uint32_t> result(part.indices.size(),std::numeric_limits<uint32_t>::max());
    for (size_t i=0;i<source.size();++i)
    {
        const auto& a=source[i];
        const auto& b=render[i];
        if ((i>0 && source[i-1].key==a.key) || (i+1<source.size() && source[i+1].key==a.key) ||
            a.key[0]==a.key[1] || a.key[1]==a.key[2] || a.key[0]==a.key[2]) continue;
        for (unsigned c=0;c<3;++c)
            result[a.offset+(a.rotation+c)%3]=part.renderIndices[b.offset+(b.rotation+c)%3];
    }
    return result;
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
