#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPagePrototype.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/MeshBuild.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ClipShape.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <cstring>
#include <cmath>

namespace Poseidon::GeometryPages
{
// Explicit synchronous owner operation only. The caller holds the chosen object's
// final Shape alive and unchanged throughout this call. No source load, material
// resolution, texture upload, renderer operation, or retained Shape pointer occurs.
// controlledAuthoredRigid is a caller certificate for the privately authored pilot,
// NOT an eligibility classifier for arbitrary retail/config-backed objects.
// It excludes forest/crown transforms, skeletal influences and constructor/config
// deformation/destruction even when GetAllowAnimation happens to be false.
struct ShapeExportSelection
{
    SourceIdentity source;
    int coarseLevel = -1, fineLevel = -1;
    bool controlledAuthoredRigid = false;
};
struct ExportedMesh
{
    std::vector<Position> positions;
    std::vector<SVertex> vertices;
    std::vector<uint32_t> indices, materials;
    MeshInput Input() const
    {
        return {positions, {reinterpret_cast<const uint8_t*>(vertices.data()), vertices.size()*sizeof(SVertex)},
            sizeof(SVertex), indices, materials};
    }
};
struct ShapeExport
{
    SourceIdentity source;
    ExportedMesh coarse, fine;
};
enum class ExportStatus { Exported, Unsupported, Invalid, Capacity, AllocationFailed, WrongOwner };

inline ExportStatus ExportShapePair(const LODShape& shape, const ShapeExportSelection& selection,
                                   ShapeExport& destination)
{
    if (!Foundation::IsMainThread()) return ExportStatus::WrongOwner;
    constexpr size_t byteCap = 32u*1024u*1024u, vertexCap = 65536, triangleCap = 131072;
    if (!selection.controlledAuthoredRigid || shape.GetAllowAnimation() ||
        selection.coarseLevel == selection.fineLevel) return ExportStatus::Unsupported;
    const auto validLevel = [&](int i) { return i >= 0 && i < shape.NLevels() && shape.IsNormalLevel(i) && shape.Level(i); };
    if (!validLevel(selection.coarseLevel) || !validLevel(selection.fineLevel)) return ExportStatus::Invalid;
    const auto& source = selection.source;
    bool hash = false; for (auto b : source.sourceSha256) hash |= b != 0;
    if (!hash || !source.vertexLayout || !source.materialMapping ||
        source.coarseRepresentation != uint32_t(selection.coarseLevel) ||
        source.fineRepresentation != uint32_t(selection.fineLevel)) return ExportStatus::Invalid;
    size_t bytes = 0;
    try
    {
        ShapeExport result; result.source = source;
        auto capture = [&](const Shape& s, ExportedMesh& mesh) -> ExportStatus
        {
            if (s.NProxies() || s.NAnimationPhases()) return ExportStatus::Unsupported;
            if (s.NVertex() <= 0 || size_t(s.NVertex()) > vertexCap || s.NSections() <= 0 || s.NSections() > 16)
                return ExportStatus::Capacity;
            for (int i=0; i<s.NVertex(); ++i)
            {
                if (s.Clip(i) & (ClipLandKeep|ClipLandOn)) return ExportStatus::Unsupported;
                const auto& p=s.Pos(i);
                if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z())) return ExportStatus::Invalid;
            }
            // Check exact section/face coverage and sums BEFORE legacy int CountIndices.
            size_t triangles=0; Offset cursor=s.BeginFaces();
            std::array<size_t,16> sectionTriangles{};
            for (int section=0; section<s.NSections(); ++section)
            {
                const auto& sec=s.GetSection(section);
                if (sec.properties.GetTexture() || sec.properties.Special() || sec.surfMat || sec.material != 0)
                    return ExportStatus::Unsupported;
                if (sec.beg != cursor || sec.end < sec.beg || sec.end > s.EndFaces()) return ExportStatus::Invalid;
                while (cursor < sec.end)
                {
                    const auto& poly=s.Face(cursor);
                    if (poly.N()<3 || poly.N()>MaxPoly) return ExportStatus::Invalid;
                    for (int v=0; v<poly.N(); ++v) if (size_t(poly.GetVertex(v)) >= size_t(s.NVertex())) return ExportStatus::Invalid;
                    const size_t count=size_t(poly.N()-2);
                    if (count > triangleCap-triangles) return ExportStatus::Capacity;
                    triangles += count;
                    sectionTriangles[section] += count;
                    const Offset previous=cursor; s.NextFace(cursor);
                    if (cursor <= previous || cursor > sec.end) return ExportStatus::Invalid;
                }
            }
            if (cursor != s.EndFaces() || !triangles) return ExportStatus::Invalid;
            // Include temporary signed legacy index packing in the combined work cap.
            const size_t required=size_t(s.NVertex())*(sizeof(SVertex)+sizeof(Position))+triangles*28;
            if (required > byteCap-bytes) return ExportStatus::Capacity;
            bytes += required;
            mesh.vertices.resize(s.NVertex()); mesh.positions.resize(s.NVertex());
            mesh.indices.resize(triangles*3); mesh.materials.resize(triangles);
            size_t materialAt=0;
            for (int section=0; section<s.NSections(); ++section)
                for (size_t t=0; t<sectionTriangles[section]; ++t) mesh.materials[materialAt++]=uint32_t(section);
            if (size_t(render::mesh::CountIndices(s)) != mesh.indices.size()) return ExportStatus::Invalid;
            render::mesh::BuildVertices(s,mesh.vertices.data());
            static_assert(sizeof(VertexIndex)==sizeof(uint32_t));
            std::vector<VertexIndex> packedIndices(mesh.indices.size());
            render::mesh::BuildIndices(s,packedIndices.data());
            for (size_t i=0; i<packedIndices.size(); ++i) mesh.indices[i]=uint32_t(packedIndices[i]);
            for (size_t i=0; i<mesh.vertices.size(); ++i)
            {
                const auto& p=mesh.vertices[i].pos;
                mesh.positions[i]={p.X(),p.Y(),p.Z()};
            }
            return ExportStatus::Exported;
        };
        auto status=capture(*shape.Level(selection.coarseLevel),result.coarse);
        if (status!=ExportStatus::Exported) return status;
        status=capture(*shape.Level(selection.fineLevel),result.fine);
        if (status!=ExportStatus::Exported) return status;
        destination=std::move(result); return ExportStatus::Exported;
    }
    catch (const std::bad_alloc&) { return ExportStatus::AllocationFailed; }
}
}
