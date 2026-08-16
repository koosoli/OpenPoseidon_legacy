#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelComputation.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODStructures.hpp>
#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Core/Data3D.h>
#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace Poseidon::Asset::Formats
{

class MLODLoader
{
  public:
    using Model          = Poseidon::Model::Model;
    using LODLevel       = Poseidon::Model::LODLevel;
    using Mesh           = Poseidon::Model::Mesh;
    using Vertex         = Poseidon::Model::Vertex;
    using Triangle       = Poseidon::Model::Triangle;
    using Quad           = Poseidon::Model::Quad;
    using Material       = Poseidon::Model::Material;
    using NamedSelection = Poseidon::Model::NamedSelection;
    using NamedProperty  = Poseidon::Model::NamedProperty;
    using Vector3        = Poseidon::Model::Vector3;
    using Vector2        = Poseidon::Model::Vector2;
    using VertexFlags    = Poseidon::Model::VertexFlags;
    using FaceFlags      = Poseidon::Model::FaceFlags;

    static Model load(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("Failed to open file: " + filePath);
        auto          size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(static_cast<size_t>(size));
        file.read(buffer.data(), size);
        return loadFromBuffer(buffer.data(), static_cast<int>(buffer.size()), filePath);
    }

    static Model loadFromBuffer(const char* data, int size, const std::string& sourcePath)
    {
        QIStream     stream(const_cast<char*>(data), size);
        BinaryReader reader(stream);

        Model model;
        model.sourceFormat = "MLOD";
        model.sourcePath   = sourcePath;

        auto header        = MLOD::readHeader(reader);
        model.sourceVersion = header.versionMajor * 10 + header.versionMinor;

        for (uint32_t lodIdx = 0; lodIdx < header.lodCount; ++lodIdx)
            model.lodLevels.push_back(loadLOD(reader, lodIdx));

        return model;
    }

  private:
    // Per-LOD dispatch (AST-011A).
    //
    // Every LOD is routed on its own signature rather than on an assumption made
    // once for the file: MLOD containers are not required to be homogeneous, and
    // the two SP3X models AST-007 found sit in the same corpus as 1,657 P3DM ones.
    //
    // A recognised-but-unimplemented P3DM LOD stops here with UnsupportedLodFormat
    // instead of being skipped. It cannot be skipped: P3DM face records are
    // variable-length (two NUL-terminated strings each), so the end of the block
    // is only knowable by parsing it, which is AST-011B. Guessing a length would
    // resynchronise on arbitrary bytes and yield a model that loads and is wrong,
    // which is worse than one that refuses.
    static LODLevel loadLOD(BinaryReader& reader, uint32_t lodIndex)
    {
        const MLOD::LodSignature signature = MLOD::peekLodSignature(reader);
        switch (signature)
        {
            case MLOD::LodSignature::SP3X:
                return loadSP3XLOD(reader);
            case MLOD::LodSignature::P3DM:
                return loadP3DMLOD(reader);
            default:
                throw MLOD::UnsupportedLodFormat(
                    signature, lodIndex,
                    "expected 'SP3X' or 'P3DM', found '" + MLOD::peekLodSignatureBytes(reader) + "'");
        }
    }

    static LODLevel loadSP3XLOD(BinaryReader& reader)
    {
        LODLevel lod;
        auto     sp3xHeader  = MLOD::readSP3XHeader(reader);
        auto     vertexTable = MLOD::readVertexTable(reader, sp3xHeader.nPos);
        auto     normalTable = MLOD::readNormalTable(reader, sp3xHeader.nNorm);
        auto     faceTable   = MLOD::readFaceTable(reader, sp3xHeader.nFace);
        auto     taggData    = MLOD::readTAGGSection(reader, sp3xHeader.nPos, sp3xHeader.nFace);

        auto vertexToPoint = convertGeometry(lod.mesh, vertexTable, normalTable, faceTable, taggData);
        convertMaterials(lod.mesh, faceTable);
        convertSelections(lod.mesh, taggData.namedSelections, vertexTable, faceTable, vertexToPoint);
        convertProperties(lod.mesh, taggData.namedProperties);
        convertMass(lod.mesh, taggData, vertexToPoint);
        sortVerticesByPointIndex(lod.mesh, vertexToPoint);
        // AST-018: MLOD encodes proxies as three-vertex "proxy:" selections.
        // Extraction existed but nothing called it, so every MLOD model reached
        // the IR with no proxies at all -- no weapons, no crew, no cargo. Must
        // run after the vertex sort, which is what fixes the indices the
        // selections refer to.
        lod.mesh.proxies = Poseidon::Model::ModelComputation::generateProxiesFromSelections(
            lod.mesh.vertices, lod.mesh.selections);
        lod.resolution     = taggData.resolution;
        lod.purpose        = Poseidon::Model::ClassifyLodResolution(lod.resolution);
        lod.sourceEncoding = "SP3X";
        // AST-018. Established, not measured: reversing SP3X index order on load
        // is what the original engine's own loader does. Nothing in this repo has
        // rendered it to confirm the result looks right.
        lod.basis.winding            = Poseidon::Model::SourceWinding::ClockwiseReversedOnLoad;
        lod.basis.windingConfidence  = Poseidon::Model::BasisConfidence::Established;
        lod.basis.normals            = Poseidon::Model::NormalOrientation::StoredPerVertex;
        lod.basis.normalsConfidence  = Poseidon::Model::BasisConfidence::Established;
        lod.basis.tangents           = Poseidon::Model::TangentHandedness::NotSupplied;
        lod.basis.tangentsConfidence = Poseidon::Model::BasisConfidence::Established;
        lod.basis.origin             = Poseidon::Model::SourceOrigin::ModelSpace;
        lod.basis.originConfidence   = Poseidon::Model::BasisConfidence::Established;
        lod.sourceWinding  = Poseidon::Model::DescribeWinding(lod.basis.winding, lod.basis.windingConfidence);

        return lod;
    }

    // AST-011B. Deliberately the same conversion as SP3X after the read: the two
    // encodings differ in how the tables are stored, not in what they mean, so a
    // second converter would be a second place for the winding and vertex-merge
    // rules to drift apart.
    static LODLevel loadP3DMLOD(BinaryReader& reader)
    {
        LODLevel lod;
        auto     header = MLOD::readP3DMHeader(reader);
        // Note nNorm, not nPos: P3DM normal count is independent of the vertex
        // count, and reusing nPos here would truncate or overrun the normal table.
        auto     vertexTable = MLOD::readVertexTable(reader, header.nPos);
        auto     normalTable = MLOD::readNormalTable(reader, header.nNorm);
        auto     faceTable   = MLOD::readP3DMFaceTable(reader, header.nFace);
        // #UVSet# payloads are sized by the actual face-vertex total (face.n each),
        // not by the four slots a face record reserves, so the tagg reader has to
        // be told it -- it cannot derive it from the point or face counts alone.
        int32_t  totalFaceVertices = 0;
        for (const auto& face : faceTable.faces)
            totalFaceVertices += face.n;
        auto     taggData    = MLOD::readP3DMTAGGSection(reader, header.nPos, header.nFace, totalFaceVertices);

        convertUVChannels(lod, faceTable, taggData);
        auto vertexToPoint = convertGeometry(lod.mesh, vertexTable, normalTable, faceTable, taggData);
        convertMaterials(lod.mesh, faceTable);
        convertSelections(lod.mesh, taggData.namedSelections, vertexTable, faceTable, vertexToPoint);
        convertProperties(lod.mesh, taggData.namedProperties);
        convertMass(lod.mesh, taggData, vertexToPoint);
        sortVerticesByPointIndex(lod.mesh, vertexToPoint);
        // AST-018: MLOD encodes proxies as three-vertex "proxy:" selections.
        // Extraction existed but nothing called it, so every MLOD model reached
        // the IR with no proxies at all -- no weapons, no crew, no cargo. Must
        // run after the vertex sort, which is what fixes the indices the
        // selections refer to.
        lod.mesh.proxies = Poseidon::Model::ModelComputation::generateProxiesFromSelections(
            lod.mesh.vertices, lod.mesh.selections);
        lod.resolution     = taggData.resolution;
        lod.purpose        = Poseidon::Model::ClassifyLodResolution(lod.resolution);
        lod.uvSetCount     = taggData.uvSetCount;
        lod.sourceEncoding = "P3DM";
        // Recorded rather than asserted. The SP3X path reverses winding on load
        // and the same reversal is applied here, but that P3DM uses the identical
        // convention is an assumption -- no P3DM model has been rendered yet, so
        // nothing has confirmed it. The Assumed confidence is the point: a later
        // visual check can contradict it in one place instead of hunting for an
        // implicit swap buried in the converter.
        lod.basis.winding            = Poseidon::Model::SourceWinding::ClockwiseReversedOnLoad;
        lod.basis.windingConfidence  = Poseidon::Model::BasisConfidence::Assumed;
        lod.basis.normals            = Poseidon::Model::NormalOrientation::StoredPerVertex;
        lod.basis.normalsConfidence  = Poseidon::Model::BasisConfidence::Assumed;
        lod.basis.tangents           = Poseidon::Model::TangentHandedness::NotSupplied;
        lod.basis.tangentsConfidence = Poseidon::Model::BasisConfidence::Established;
        lod.basis.origin             = Poseidon::Model::SourceOrigin::ModelSpace;
        lod.basis.originConfidence   = Poseidon::Model::BasisConfidence::Assumed;
        lod.sourceWinding  = Poseidon::Model::DescribeWinding(lod.basis.winding, lod.basis.windingConfidence);

        return lod;
    }

    // AST-011C. Keeps every decoded channel on the LOD in source order, so a
    // material's uvSource can name one of them later. Channel 1 is additionally
    // promoted onto Vertex::uv1 by convertGeometry; the rest stay here rather than
    // being dropped for want of a vertex slot to put them in.
    static void convertUVChannels(LODLevel& lod, const MLOD::FaceTable& faceTable, const MLOD::TAGGData& taggData)
    {
        (void)faceTable;
        for (const auto& set : taggData.uvSets)
        {
            if (!set.decoded)
                continue;
            Poseidon::Model::UVChannel channel;
            channel.id = set.id;
            channel.faceVertexUVs.reserve(set.uv.size() / 2);
            for (size_t i = 0; i + 1 < set.uv.size(); i += 2)
                channel.faceVertexUVs.push_back(Vector2(set.uv[i], set.uv[i + 1]));
            lod.uvChannels.push_back(std::move(channel));
        }
    }

    // Converts raw POINT_* flags from data3d.h to ClipFlags (matches Shape.cpp:410-504)
    static uint32_t convertPointFlagsToClipFlags(uint32_t pointFlags)
    {
        const uint32_t allFlags = (POINT_ONLAND | POINT_UNDERLAND | POINT_ABOVELAND | POINT_KEEPLAND |
                                   POINT_DECAL | POINT_VDECAL | POINT_NOLIGHT | POINT_FULLLIGHT |
                                   POINT_HALFLIGHT | POINT_AMBIENT | POINT_NOFOG | POINT_SKYFOG |
                                   POINT_USER_MASK | POINT_SPECIAL_MASK);
        if (pointFlags & ~allFlags)
            pointFlags = 0;

        ClipFlags hints = ClipAll;
        if (!(pointFlags & allFlags))
            return static_cast<uint32_t>(hints);

        if (pointFlags & POINT_ONLAND)
            hints |= ClipLandOn;
        else if (pointFlags & POINT_UNDERLAND)
            hints |= ClipLandUnder;
        else if (pointFlags & POINT_ABOVELAND)
            hints |= ClipLandAbove;
        else if (pointFlags & POINT_KEEPLAND)
            hints |= ClipLandKeep;

        if (pointFlags & POINT_DECAL)
            hints |= ClipDecalNormal;
        else if (pointFlags & POINT_VDECAL)
            hints |= ClipDecalVertical;

        // Explicit cast: MaterialSection is named enum, ClipUserStep is unnamed — arithmetic deprecated in C++20
        if (pointFlags & POINT_NOLIGHT)
            hints |= static_cast<uint32_t>(MSShining) * ClipUserStep;
        else if (pointFlags & POINT_FULLLIGHT)
            hints |= static_cast<uint32_t>(MSFullLighted) * ClipUserStep;
        else if (pointFlags & POINT_HALFLIGHT)
            hints |= static_cast<uint32_t>(MSHalfLighted) * ClipUserStep;
        else if (pointFlags & POINT_AMBIENT)
            hints |= static_cast<uint32_t>(MSInShadow) * ClipUserStep;

        if (pointFlags & POINT_NOFOG)
            hints |= ClipFogDisable;
        else if (pointFlags & POINT_SKYFOG)
            hints |= ClipFogSky;

        if (pointFlags & POINT_USER_MASK)
        {
            int user = (pointFlags & POINT_USER_MASK) / POINT_USER_STEP;
            hints |= user * ClipUserStep;
        }

        return static_cast<uint32_t>(hints);
    }

    static std::vector<int32_t> convertGeometry(Mesh& mesh,
                                                 const MLOD::VertexTable& vertexTable,
                                                 const MLOD::NormalTable& normalTable,
                                                 const MLOD::FaceTable&   faceTable,
                                                 const MLOD::TAGGData&    taggData)
    {
        if (faceTable.faces.empty() && !vertexTable.points.empty())
        {
            mesh.vertices.reserve(vertexTable.points.size());
            for (const auto& point : vertexTable.points)
            {
                Vertex v;
                v.position = Vector3(point.position.x, point.position.y, point.position.z);
                v.flags    = static_cast<VertexFlags>(convertPointFlagsToClipFlags(static_cast<uint32_t>(point.flags)));
                mesh.vertices.push_back(v);
            }
            std::vector<int32_t> vertexToPoint(mesh.vertices.size());
            for (size_t i = 0; i < mesh.vertices.size(); ++i)
                vertexToPoint[i] = static_cast<int32_t>(i);
            return vertexToPoint;
        }

        std::vector<std::vector<uint32_t>> pointToVertices(vertexTable.points.size());

        auto verticesEqual = [](const Vertex& a, const Vertex& b) -> bool
        {
            // Match old VertexTable::AddVertex precision (squared-distance)
            constexpr float precPos2  = 0.005f * 0.005f;
            constexpr float precNorm2 = 0.05f * 0.05f;
            constexpr float precUV    = 0.005f;
            if (a.flags != b.flags)
                return false;
            float dx = a.position.x - b.position.x, dy = a.position.y - b.position.y,
                  dz = a.position.z - b.position.z;
            if (dx * dx + dy * dy + dz * dz > precPos2)
                return false;
            float dnx = a.normal.x - b.normal.x, dny = a.normal.y - b.normal.y,
                  dnz = a.normal.z - b.normal.z;
            if (dnx * dnx + dny * dny + dnz * dnz > precNorm2)
                return false;
            if (std::abs(a.uv.u - b.uv.u) > precUV)
                return false;
            if (std::abs(a.uv.v - b.uv.v) > precUV)
                return false;
            // AST-011C: the second channel participates in identity. Two vertices
            // agreeing on uv0 but differing on uv1 are different vertices, and
            // merging them would discard the channel a material's uvSource selects.
            // SP3X leaves uv1 zero throughout, so this never splits an SP3X mesh.
            if (std::abs(a.uv1.u - b.uv1.u) > precUV)
                return false;
            if (std::abs(a.uv1.v - b.uv1.v) > precUV)
                return false;
            return true;
        };

        mesh.vertices.reserve(faceTable.faces.size() * 3);
        mesh.triangles.reserve(faceTable.faces.size());

        // Channel 1 if the LOD carried one. Channel 0 duplicates the per-face UVs
        // already read from the face records, so promoting it would change nothing.
        const std::vector<float>* uv1Source = nullptr;
        if (taggData.uvSets.size() > 1 && taggData.uvSets[1].decoded)
            uv1Source = &taggData.uvSets[1].uv;
        size_t faceVertexCursor = 0;

        uint32_t faceIndex = 0;
        for (const auto& face : faceTable.faces)
        {
            if (face.n == 3)
            {
                Triangle tri;
                for (int i = 0; i < 3; ++i)
                {
                    Vertex  v;
                    int32_t pointIndex = face.vs[i].point;
                    if (pointIndex >= 0 && pointIndex < static_cast<int32_t>(vertexTable.points.size()))
                    {
                        const auto& pt = vertexTable.points[pointIndex];
                        v.position = Vector3(pt.position.x, pt.position.y, pt.position.z);
                        v.flags = static_cast<VertexFlags>(convertPointFlagsToClipFlags(static_cast<uint32_t>(pt.flags)));
                    }
                    if (face.vs[i].normal >= 0 && face.vs[i].normal < static_cast<int32_t>(normalTable.normals.size()))
                    {
                        const auto& n = normalTable.normals[face.vs[i].normal];
                        v.normal = Vector3(n.x, n.y, n.z);
                    }
                    v.uv = Vector2(face.vs[i].mapU, face.vs[i].mapV);
                    if (uv1Source)
                    {
                        const size_t at = (faceVertexCursor + static_cast<size_t>(i)) * 2;
                        if (at + 1 < uv1Source->size())
                            v.uv1 = Vector2((*uv1Source)[at], (*uv1Source)[at + 1]);
                    }

                    uint32_t foundIndex = UINT32_MAX;
                    if (pointIndex >= 0 && pointIndex < static_cast<int32_t>(pointToVertices.size()))
                    {
                        for (uint32_t vi : pointToVertices[pointIndex])
                        {
                            if (verticesEqual(mesh.vertices[vi], v))
                            {
                                foundIndex = vi;
                                break;
                            }
                        }
                    }
                    if (foundIndex != UINT32_MAX)
                    {
                        tri.indices[i] = foundIndex;
                    }
                    else
                    {
                        tri.indices[i] = static_cast<uint32_t>(mesh.vertices.size());
                        if (pointIndex >= 0 && pointIndex < static_cast<int32_t>(pointToVertices.size()))
                            pointToVertices[pointIndex].push_back(tri.indices[i]);
                        mesh.vertices.push_back(v);
                    }
                }
                tri.flags         = static_cast<FaceFlags>(static_cast<uint32_t>(face.flags));
                tri.originalIndex = faceIndex++;
                faceVertexCursor += 3;
                std::swap(tri.indices[0], tri.indices[1]); // reverse winding
                mesh.triangles.push_back(tri);
            }
            else if (face.n == 4)
            {
                Quad quad;
                for (int i = 0; i < 4; ++i)
                {
                    Vertex  v;
                    int32_t pointIndex = face.vs[i].point;
                    if (pointIndex >= 0 && pointIndex < static_cast<int32_t>(vertexTable.points.size()))
                    {
                        const auto& pt = vertexTable.points[pointIndex];
                        v.position = Vector3(pt.position.x, pt.position.y, pt.position.z);
                        v.flags = static_cast<VertexFlags>(convertPointFlagsToClipFlags(static_cast<uint32_t>(pt.flags)));
                    }
                    if (face.vs[i].normal >= 0 && face.vs[i].normal < static_cast<int32_t>(normalTable.normals.size()))
                    {
                        const auto& n = normalTable.normals[face.vs[i].normal];
                        v.normal = Vector3(n.x, n.y, n.z);
                    }
                    v.uv = Vector2(face.vs[i].mapU, face.vs[i].mapV);
                    if (uv1Source)
                    {
                        const size_t at = (faceVertexCursor + static_cast<size_t>(i)) * 2;
                        if (at + 1 < uv1Source->size())
                            v.uv1 = Vector2((*uv1Source)[at], (*uv1Source)[at + 1]);
                    }

                    uint32_t foundIndex = UINT32_MAX;
                    if (pointIndex >= 0 && pointIndex < static_cast<int32_t>(pointToVertices.size()))
                    {
                        for (uint32_t vi : pointToVertices[pointIndex])
                        {
                            if (verticesEqual(mesh.vertices[vi], v))
                            {
                                foundIndex = vi;
                                break;
                            }
                        }
                    }
                    if (foundIndex != UINT32_MAX)
                    {
                        quad.indices[i] = foundIndex;
                    }
                    else
                    {
                        quad.indices[i] = static_cast<uint32_t>(mesh.vertices.size());
                        if (pointIndex >= 0 && pointIndex < static_cast<int32_t>(pointToVertices.size()))
                            pointToVertices[pointIndex].push_back(quad.indices[i]);
                        mesh.vertices.push_back(v);
                    }
                }
                quad.flags         = static_cast<FaceFlags>(static_cast<uint32_t>(face.flags));
                quad.originalIndex = faceIndex++;
                faceVertexCursor += 4;
                std::swap(quad.indices[0], quad.indices[1]); // reverse winding
                std::swap(quad.indices[2], quad.indices[3]);
                mesh.quads.push_back(quad);
            }
        }

        std::vector<int32_t> vertexToPoint(mesh.vertices.size(), -1);
        for (int32_t pointIdx = 0; pointIdx < static_cast<int32_t>(pointToVertices.size()); ++pointIdx)
        {
            for (uint32_t vertexIdx : pointToVertices[pointIdx])
            {
                if (vertexIdx < vertexToPoint.size())
                    vertexToPoint[vertexIdx] = pointIdx;
            }
        }

        return vertexToPoint;
    }

    static void convertMaterials(Mesh& mesh, const MLOD::FaceTable& faceTable)
    {
        // Keyed on the texture AND material pair, not the texture alone.
        //
        // SP3X faces name only a texture, so for them this partitions exactly as
        // before. P3DM faces name both, and the common Arma case is several faces
        // sharing one texture (often none at all) while pointing at different
        // RVMATs -- the Arma 2 worker head, for instance, has no face texture and
        // two distinct materials. Keying on the texture would merge those into a
        // single "#default#" material and silently discard the distinction this
        // ticket exists to preserve.
        auto keyOf = [](const MLOD::DataFaceEx& face)
        { return face.textureName + '|' + face.materialName; };

        std::unordered_map<std::string, uint32_t> materialMap;
        for (const auto& face : faceTable.faces)
        {
            const std::string key = keyOf(face);
            if (materialMap.find(key) != materialMap.end())
                continue;

            Material mat;
            // Prefer the texture for the display name so SP3X models keep the
            // names they had; fall back to the RVMAT when there is no texture.
            if (!face.textureName.empty())
                mat.name = face.textureName;
            else if (!face.materialName.empty())
                mat.name = face.materialName;
            else
                mat.name = "#default#";
            mat.texturePath  = face.textureName;
            mat.materialPath = face.materialName;

            materialMap[key] = static_cast<uint32_t>(mesh.materials.size());
            mesh.materials.push_back(mat);
        }

        size_t triIdx = 0, quadIdx = 0;
        for (const auto& face : faceTable.faces)
        {
            const uint32_t materialIdx = materialMap[keyOf(face)];
            if (face.n == 3)
            {
                if (triIdx < mesh.triangles.size())
                    mesh.triangles[triIdx].materialIndex = materialIdx;
                triIdx++;
            }
            else if (face.n == 4)
            {
                if (quadIdx < mesh.quads.size())
                    mesh.quads[quadIdx].materialIndex = materialIdx;
                quadIdx++;
            }
        }
    }

    static void convertSelections(Mesh& mesh,
                                  const std::vector<MLOD::TAGGNamedSelection>& selections,
                                  const MLOD::VertexTable&                     vertexTable,
                                  const MLOD::FaceTable&                       faceTable,
                                  const std::vector<int32_t>&                  vertexToPoint)
    {
        std::vector<std::vector<uint32_t>> pointToVertices(vertexTable.points.size());
        for (size_t vertIdx = 0; vertIdx < vertexToPoint.size(); ++vertIdx)
        {
            int32_t pointIdx = vertexToPoint[vertIdx];
            if (pointIdx >= 0 && pointIdx < static_cast<int32_t>(pointToVertices.size()))
                pointToVertices[pointIdx].push_back(static_cast<uint32_t>(vertIdx));
        }

        for (const auto& sel : selections)
        {
            NamedSelection selection;
            selection.name = sel.name;
            for (size_t pointIdx = 0; pointIdx < sel.pointWeights.size(); ++pointIdx)
            {
                if (sel.pointWeights[pointIdx] > 0 && pointIdx < pointToVertices.size())
                {
                    for (uint32_t vertIdx : pointToVertices[pointIdx])
                    {
                        selection.vertexIndices.push_back(vertIdx);
                        // AST-018: keep the source byte. It used to be read only
                        // as a membership test, which turned every soft
                        // selection into a hard one and discarded the rest.
                        // Undecoded on purpose -- see NamedSelection.
                        selection.sourceVertexWeights.push_back(sel.pointWeights[pointIdx]);
                    }
                }
            }
            // COL-001: SOURCE FACE INDICES -- the face's position in the file's
            // mixed triangle/quad stream, the same numbering Triangle::originalIndex
            // and Quad::originalIndex carry, and the same unit the ODOL loaders
            // emit. The previous encoding numbered a triangulated stream (a quad
            // counted twice) that nothing produced: convertGeometry keeps quads as
            // quads, and ShapeAdapter mapped the numbers as triangle-array
            // indices. Every quad in a selection therefore landed on the wrong
            // face or on none, and a geometry LOD's ComponentXX -- six quads for
            // a box -- reached InitConvexComponents with fewer than four faces
            // and was dropped: no components, no collision on any MLOD model.
            //
            // The counter mirrors convertGeometry exactly: only faces with three or
            // four vertices are emitted there, so only those advance the index.
            uint32_t sourceFace = 0;
            for (size_t faceIdx = 0; faceIdx < faceTable.faces.size(); ++faceIdx)
            {
                const auto& face = faceTable.faces[faceIdx];
                if (face.n != 3 && face.n != 4)
                    continue;
                if (faceIdx < sel.faceFlags.size() && sel.faceFlags[faceIdx])
                    selection.triangleIndices.push_back(sourceFace);
                ++sourceFace;
            }
            mesh.selections.push_back(selection);
        }
    }

    static void convertProperties(Mesh& mesh, const std::vector<MLOD::TAGGNamedProperty>& properties)
    {
        for (const auto& prop : properties)
        {
            NamedProperty namedProp;
            namedProp.name  = prop.property;
            namedProp.value = prop.value;
            mesh.properties.push_back(namedProp);
        }
    }

    // COL-001: the `#Mass#` tagg, per point in the source, becomes per-vertex mass
    // in the mesh. A point that convertGeometry split into several vertices has
    // its mass divided evenly among them, so the total and the centre of mass
    // that LODShape::CalculateMass derives are the source's. Must run BEFORE
    // sortVerticesByPointIndex, which reorders the vector alongside the vertices.
    static void convertMass(Mesh& mesh, const MLOD::TAGGData& taggData, const std::vector<int32_t>& vertexToPoint)
    {
        mesh.vertexMass.clear();
        if (!taggData.hasMass || mesh.vertices.empty())
            return;
        const auto& perPoint = taggData.mass.massPerPoint;
        std::vector<uint32_t> verticesPerPoint(perPoint.size(), 0);
        for (size_t v = 0; v < mesh.vertices.size() && v < vertexToPoint.size(); ++v)
        {
            const int32_t point = vertexToPoint[v];
            if (point >= 0 && static_cast<size_t>(point) < verticesPerPoint.size())
                ++verticesPerPoint[static_cast<size_t>(point)];
        }
        mesh.vertexMass.assign(mesh.vertices.size(), 0.0f);
        bool any = false;
        for (size_t v = 0; v < mesh.vertices.size() && v < vertexToPoint.size(); ++v)
        {
            const int32_t point = vertexToPoint[v];
            if (point < 0 || static_cast<size_t>(point) >= perPoint.size())
                continue;
            const uint32_t share = verticesPerPoint[static_cast<size_t>(point)];
            mesh.vertexMass[v] = share > 0 ? perPoint[static_cast<size_t>(point)] / static_cast<float>(share) : 0.0f;
            any = any || mesh.vertexMass[v] != 0.0f;
        }
        if (!any)
            mesh.vertexMass.clear();
    }

    static void sortVerticesByPointIndex(Mesh& mesh, const std::vector<int32_t>& vertexToPoint)
    {
        if (mesh.vertices.empty() || vertexToPoint.empty())
            return;

        struct SortEntry
        {
            uint32_t    originalIndex;
            int32_t     pointIndex;
            VertexFlags flags;
        };

        std::vector<SortEntry> sortEntries;
        sortEntries.reserve(mesh.vertices.size());
        for (uint32_t i = 0; i < mesh.vertices.size(); ++i)
            sortEntries.push_back({i, vertexToPoint[i], mesh.vertices[i].flags});

        // Old loader sorts by material flags first, then point index (Shape.cpp:2266 SortVertices)
        // ClipLightMask = 0xF0000, ClipUserMask = 0xFF00000
        constexpr uint32_t priorityMask = 0xFFF0000;
        std::sort(sortEntries.begin(), sortEntries.end(),
                  [priorityMask](const SortEntry& a, const SortEntry& b)
                  {
                      uint32_t priorA = static_cast<uint32_t>(a.flags) & priorityMask;
                      uint32_t priorB = static_cast<uint32_t>(b.flags) & priorityMask;
                      if (priorA != priorB)
                          return priorA < priorB;
                      if (a.pointIndex != b.pointIndex)
                          return a.pointIndex < b.pointIndex;
                      return a.originalIndex < b.originalIndex;
                  });

        std::vector<Vertex> sortedVertices;
        sortedVertices.reserve(mesh.vertices.size());
        for (const auto& entry : sortEntries)
            sortedVertices.push_back(mesh.vertices[entry.originalIndex]);
        mesh.vertices = std::move(sortedVertices);
        if (mesh.vertexMass.size() == sortEntries.size())
        {
            std::vector<float> sortedMass;
            sortedMass.reserve(sortEntries.size());
            for (const auto& entry : sortEntries)
                sortedMass.push_back(mesh.vertexMass[entry.originalIndex]);
            mesh.vertexMass = std::move(sortedMass);
        }

        std::vector<uint32_t> invSort(sortEntries.size());
        for (uint32_t i = 0; i < sortEntries.size(); ++i)
            invSort[sortEntries[i].originalIndex] = i;

        for (auto& tri : mesh.triangles)
            for (int i = 0; i < 3; ++i)
                tri.indices[i] = invSort[tri.indices[i]];
        for (auto& quad : mesh.quads)
            for (int i = 0; i < 4; ++i)
                quad.indices[i] = invSort[quad.indices[i]];
        for (auto& selection : mesh.selections)
            for (auto& vertIdx : selection.vertexIndices)
                vertIdx = invSort[vertIdx];
    }
};

} // namespace Poseidon::Asset::Formats
