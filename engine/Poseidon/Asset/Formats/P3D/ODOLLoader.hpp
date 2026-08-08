#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/P3D/P3DStructures.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Poseidon::Asset::Formats
{

class ODOLLoader
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
    using Proxy          = Poseidon::Model::Proxy;
    using Section        = Poseidon::Model::Section;
    using Vector3        = Poseidon::Model::Vector3;
    using Vector2        = Poseidon::Model::Vector2;
    using Matrix4x3      = Poseidon::Model::Matrix4x3;
    using VertexFlags    = Poseidon::Model::VertexFlags;
    using FaceFlags      = Poseidon::Model::FaceFlags;
    using RenderHints    = Poseidon::Model::RenderHints;
    using BoundingBox    = Poseidon::Model::BoundingBox;
    using BoundingSphere = Poseidon::Model::BoundingSphere;

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

        // AST-012A: route on the exact revision before any body is read.
        //
        // Dispatching first is what keeps a later revision from being fed to the v7
        // reader, which would not fail cleanly -- it would desynchronise and report
        // whatever the misaligned bytes said, the same failure mode AST-011A found
        // in the MLOD path. The revision is read without consuming it so the chosen
        // parser still sees its own header.
        const P3D::OdolRevisionInfo revision = P3D::PeekOdolRevision(reader);
        // AST-012B: revision 73 has one implemented shape. It gets its own reader
        // and its own converter -- it is never fed to readModel(), whose layout is
        // v7's -- and anything the narrow reader refuses comes back out as the
        // same unsupported-revision failure, carrying the constraint that broke.
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 73)
            return loadOdol73Static(reader, size, sourcePath, revision);
        if (revision.support != P3D::OdolSupport::Parsed)
            throw P3D::UnsupportedOdolRevision(revision);

        auto odolModel = P3D::readModel(reader);
        return convertToModel(odolModel, sourcePath);
    }

  private:
    static Model loadOdol73Static(BinaryReader& reader, int size, const std::string& sourcePath,
                                  const P3D::OdolRevisionInfo& revision)
    {
        try
        {
            return convertOdol73(P3D::ReadOdol73StaticModel(reader, size), sourcePath);
        }
        catch (const P3D::UnsupportedOdolRevision&)
        {
            throw;
        }
        catch (const std::runtime_error& error)
        {
            throw P3D::UnsupportedOdolRevision(revision, error.what());
        }
    }

    static Model convertOdol73(const P3D::Odol73StaticModel& source, const std::string& sourcePath)
    {
        const auto& info = source.directory.model;
        Model model;
        model.sourcePath    = sourcePath;
        model.sourceFormat  = "ODOL";
        model.sourceVersion = 73;
        model.special       = static_cast<float>(info.special);
        model.boundingSphere.radius = info.boundingSphere;
        model.geometrySphere.radius = info.geometrySphere;
        model.boundingBox.min = Vector3(info.bboxMin.x, info.bboxMin.y, info.bboxMin.z);
        model.boundingBox.max = Vector3(info.bboxMax.x, info.bboxMax.y, info.bboxMax.z);
        model.aimingCenter    = Vector3(info.aimingCenter.x, info.aimingCenter.y, info.aimingCenter.z);
        model.color           = info.color;
        model.viewDensity     = info.viewDensity;
        model.remarksFlags    = info.remarks;
        model.andHints        = static_cast<RenderHints>(info.andHints);
        model.orHints         = static_cast<RenderHints>(info.orHints);

        model.lodLevels.reserve(source.lods.size());
        for (size_t i = 0; i < source.lods.size(); ++i)
        {
            LODLevel lod;
            lod.resolution     = info.resolutions[i];
            lod.purpose        = Poseidon::Model::ClassifyLodResolution(lod.resolution);
            lod.sourceEncoding = "ODOL73";
            lod.uvSetCount     = static_cast<int32_t>(source.lods[i].rest.uvSetCount);
            lod.mesh           = convertOdol73Mesh(source.lods[i]);
            model.lodLevels.push_back(std::move(lod));
        }
        return model;
    }

    static Mesh convertOdol73Mesh(const P3D::Odol73StaticLod& source)
    {
        Mesh mesh;
        const auto& rest = source.rest;
        mesh.vertices.reserve(rest.positions.size());
        for (size_t i = 0; i < rest.positions.size(); ++i)
        {
            Vertex vertex;
            vertex.position = Vector3(rest.positions[i].x, rest.positions[i].y, rest.positions[i].z);
            vertex.normal   = Vector3(rest.normals[i].x, rest.normals[i].y, rest.normals[i].z);
            vertex.uv       = Vector2(rest.uv0.uv[i][0], rest.uv0.uv[i][1]);
            vertex.flags    = static_cast<VertexFlags>(rest.clip[i]);
            mesh.vertices.push_back(vertex);
        }

        for (const auto& texture : source.header.textures)
            mesh.materials.emplace_back(texture, texture);
        // The embedded RVMAT a section names is the shader description for the
        // texture that section draws with; recording it against the wrong slot
        // would be worse than leaving it empty.
        for (const auto& section : source.sections)
        {
            if (section.textureIndex < 0 || static_cast<size_t>(section.textureIndex) >= mesh.materials.size())
                continue;
            if (section.materialIndex >= 0 && static_cast<size_t>(section.materialIndex) < source.materials.size())
                mesh.materials[section.textureIndex].materialPath = source.materials[section.materialIndex].name;
        }

        // Section bounds are offsets into the face stream, not face indices, so
        // the offsets are rebuilt and checked against the LOD's own declared
        // face-data size before any face is assigned a material from them.
        const auto offsets = P3D::Odol73FaceStreamOffsets(source.polygons);
        if (offsets.back() != source.polygons.faceDataSize)
            throw std::runtime_error("ODOL 73 face-stream offsets disagree with the LOD's declared face-data size");

        for (size_t face = 0; face < source.polygons.faces.size(); ++face)
        {
            const auto& indices = source.polygons.faces[face];
            uint32_t materialIndex = UINT32_MAX;
            for (const auto& section : source.sections)
                if (offsets[face] >= static_cast<uint32_t>(section.faceLower) &&
                    offsets[face] < static_cast<uint32_t>(section.faceUpper) && section.textureIndex >= 0)
                    materialIndex = static_cast<uint32_t>(section.textureIndex);
            if (indices.size() == 3)
            {
                Triangle triangle;
                for (int i = 0; i < 3; ++i) triangle.indices[i] = indices[i];
                triangle.materialIndex = materialIndex;
                triangle.originalIndex = static_cast<uint32_t>(face);
                mesh.triangles.push_back(triangle);
            }
            else
            {
                Quad quad;
                for (int i = 0; i < 4; ++i) quad.indices[i] = indices[i];
                quad.materialIndex = materialIndex;
                quad.originalIndex = static_cast<uint32_t>(face);
                mesh.quads.push_back(quad);
            }
        }

        mesh.sections.reserve(source.sections.size());
        for (const auto& source_section : source.sections)
        {
            Section section;
            section.materialIndex = source_section.textureIndex >= 0
                                        ? static_cast<uint32_t>(source_section.textureIndex) : UINT32_MAX;
            section.specialMaterial = source_section.materialIndex;
            // Face-stream offsets, as on the v7 path: not triangle indices.
            section.startTriangle = static_cast<uint32_t>(source_section.faceLower);
            section.triangleCount = static_cast<uint32_t>(source_section.faceUpper - source_section.faceLower);
            mesh.sections.push_back(section);
        }

        mesh.boundingBox.min = Vector3(source.header.bboxMin.x, source.header.bboxMin.y, source.header.bboxMin.z);
        mesh.boundingBox.max = Vector3(source.header.bboxMax.x, source.header.bboxMax.y, source.header.bboxMax.z);
        mesh.bCenter  = Vector3(source.header.bboxCenter.x, source.header.bboxCenter.y, source.header.bboxCenter.z);
        mesh.bRadius  = source.header.bboxRadius;
        mesh.orHints  = static_cast<RenderHints>(source.header.orHints);
        mesh.andHints = static_cast<RenderHints>(source.header.andHints);
        mesh.iconColor     = static_cast<uint32_t>(source.endData.colorTop);
        mesh.selectedColor = static_cast<uint32_t>(source.endData.color);
        mesh.special       = static_cast<Poseidon::Model::SpecialFlags>(source.endData.special);
        return mesh;
    }

    static Model convertToModel(const P3D::Model& odol, const std::string& sourcePath)
    {
        Model model;
        model.sourcePath   = sourcePath;
        model.sourceFormat = "ODOL";
        model.sourceVersion = odol.header.version;

        model.lodLevels.reserve(odol.lods.size());
        for (size_t i = 0; i < odol.lods.size(); ++i)
        {
            float resolution = (i < odol.resolutions.size()) ? odol.resolutions[i] : 0.0f;
            model.lodLevels.push_back(convertLOD(odol.lods[i], resolution));
        }

        model.special              = static_cast<float>(odol.special);
        model.boundingSphere.radius = odol.boundingSphere;
        model.geometrySphere.radius = odol.geometrySphere;
        model.boundingCenter = Vector3(odol.boundingCenter.x, odol.boundingCenter.y, odol.boundingCenter.z);
        model.geometryCenter = Vector3(odol.geometryCenter.x, odol.geometryCenter.y, odol.geometryCenter.z);
        model.boundingBox.min = Vector3(odol.minMax[0].x, odol.minMax[0].y, odol.minMax[0].z);
        model.boundingBox.max = Vector3(odol.minMax[1].x, odol.minMax[1].y, odol.minMax[1].z);
        model.aimingCenter   = Vector3(odol.aimingCenter.x, odol.aimingCenter.y, odol.aimingCenter.z);
        model.color          = odol.color;
        model.colorTop       = odol.colorTop;
        model.viewDensity    = odol.viewDensity;
        model.remarksFlags   = odol.remarks;
        model.centerOfMass   = Vector3(odol.centerOfMass.x, odol.centerOfMass.y, odol.centerOfMass.z);
        memcpy(model.invInertia, odol.invInertia, sizeof(model.invInertia));
        model.andHints          = static_cast<RenderHints>(odol.andHints);
        model.orHints           = static_cast<RenderHints>(odol.orHints);
        model.canOcclude        = odol.canOcclude ? 1 : 0;
        model.canBeOccluded     = odol.canBeOccluded ? 1 : 0;
        model.allowAnimation    = odol.allowAnimation ? 1 : 0;
        model.lockAutoCenter    = odol.lockAutoCenter ? 1 : 0;
        model.autoCenterEnabled = odol.autoCenter ? 1 : 0;
        model.mapType           = odol.mapType;

        model.massArray.assign(odol.massArray.begin(), odol.massArray.end());
        model.mass     = odol.mass;
        model.invMass  = odol.invMass;
        model.armor    = odol.armor;
        model.invArmor = odol.invArmor;

        model.memoryIdx                = odol.memory;
        model.geometryIdx              = odol.geometry;
        model.geometryFireIdx          = odol.geometryFire;
        model.geometryViewIdx          = odol.geometryView;
        model.geometryViewPilotIdx     = odol.geometryViewPilot;
        model.geometryViewGunnerIdx    = odol.geometryViewGunner;
        model.geometryViewCommanderIdx = odol.geometryViewCommander;
        model.geometryViewCargoIdx     = odol.geometryViewCargo;
        model.landContactIdx           = odol.landContact;
        model.roadwayIdx               = odol.roadway;
        model.pathsIdx                 = odol.paths;
        model.hitpointsIdx             = odol.hitpoints;

        return model;
    }

    static LODLevel convertLOD(const P3D::CompleteLOD& odolLOD, float resolution)
    {
        LODLevel lod;
        lod.resolution = resolution;
        lod.purpose    = Poseidon::Model::ClassifyLodResolution(resolution);
        lod.mesh       = convertMesh(odolLOD);
        return lod;
    }

    static Mesh convertMesh(const P3D::CompleteLOD& odolLOD)
    {
        Mesh mesh;
        convertGeometry(odolLOD, mesh);
        convertMaterials(odolLOD, mesh);
        convertSections(odolLOD, mesh);
        convertSelections(odolLOD, mesh);
        convertProperties(odolLOD, mesh);
        convertProxies(odolLOD, mesh);
        convertEdges(odolLOD, mesh);
        convertFrames(odolLOD, mesh);
        convertBounds(odolLOD, mesh);
        mesh.iconColor     = odolLOD.endData.colorTop;
        mesh.selectedColor = odolLOD.endData.color;
        mesh.special       = static_cast<Poseidon::Model::SpecialFlags>(odolLOD.endData.special);
        return mesh;
    }

    static void convertGeometry(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        const auto& vtable = odolLOD.vertexTable;
        mesh.vertices.reserve(vtable.points.size());
        for (size_t i = 0; i < vtable.points.size(); ++i)
        {
            Vertex      v;
            const auto& bisPos = vtable.points[i];
            v.position = Vector3(bisPos.x, bisPos.y, bisPos.z);
            if (i < vtable.normals.size())
            {
                const auto& bisNorm = vtable.normals[i];
                v.normal = Vector3(bisNorm.x, bisNorm.y, bisNorm.z);
            }
            else
            {
                v.normal = Vector3(0, 0, 1);
            }
            if (i < vtable.uvCoords.size())
            {
                const auto& bisUV = vtable.uvCoords[i];
                v.uv = Vector2(bisUV.u, bisUV.v);
            }
            else
            {
                v.uv = Vector2(0, 0);
            }
            v.flags = i < vtable.clipFlags.size() ? static_cast<VertexFlags>(vtable.clipFlags[i]) : VertexFlags::None;
            mesh.vertices.push_back(v);
        }

        size_t faceIdx = 0;
        for (const auto& face : odolLOD.faces.faces)
        {
            if (face.vertexCount == 3)
            {
                Triangle tri;
                tri.indices[0]   = face.vertexIndices[0];
                tri.indices[1]   = face.vertexIndices[1];
                tri.indices[2]   = face.vertexIndices[2];
                tri.materialIndex = 0;
                tri.flags        = static_cast<FaceFlags>(face.flags);
                tri.originalIndex = static_cast<uint32_t>(faceIdx);
                mesh.triangles.push_back(tri);
            }
            else if (face.vertexCount == 4)
            {
                Quad quad;
                quad.indices[0]   = face.vertexIndices[0];
                quad.indices[1]   = face.vertexIndices[1];
                quad.indices[2]   = face.vertexIndices[2];
                quad.indices[3]   = face.vertexIndices[3];
                quad.materialIndex = 0;
                quad.flags        = static_cast<FaceFlags>(face.flags);
                quad.originalIndex = static_cast<uint32_t>(faceIdx);
                mesh.quads.push_back(quad);
            }
            faceIdx++;
        }
    }

    static void convertMaterials(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.materials.reserve(odolLOD.textures.texturePaths.size());
        for (const auto& texPath : odolLOD.textures.texturePaths)
        {
            Material mat;
            mat.name        = texPath;
            mat.texturePath = texPath;
            mesh.materials.push_back(mat);
        }

        size_t triIdx = 0, quadIdx = 0;
        for (const auto& face : odolLOD.faces.faces)
        {
            uint32_t matIdx = face.textureIndex >= 0 ? static_cast<uint32_t>(face.textureIndex) : UINT32_MAX;
            if (face.vertexCount == 3)
            {
                if (triIdx < mesh.triangles.size())
                    mesh.triangles[triIdx].materialIndex = matIdx;
                ++triIdx;
            }
            else if (face.vertexCount == 4)
            {
                if (quadIdx < mesh.quads.size())
                    mesh.quads[quadIdx].materialIndex = matIdx;
                ++quadIdx;
            }
        }
    }

    static void convertSections(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.sections.reserve(odolLOD.sections.sections.size());
        for (const auto& sec : odolLOD.sections.sections)
        {
            Section section;
            section.materialIndex   = sec.textureIndex >= 0 ? static_cast<uint32_t>(sec.textureIndex) : UINT32_MAX;
            section.specialMaterial = sec.material;
            // Byte offsets, not face indices; ShapeAdapter uses FindSections instead
            section.startTriangle   = sec.faceIndexLowerBound;
            section.triangleCount   = sec.faceIndexUpperBound - sec.faceIndexLowerBound;
            section.hints           = static_cast<RenderHints>(sec.special);
            mesh.sections.push_back(section);
        }
    }

    static void convertSelections(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.selections.reserve(odolLOD.namedSelections.selections.size());
        for (const auto& sel : odolLOD.namedSelections.selections)
        {
            NamedSelection selection;
            selection.name = sel.name;
            selection.vertexIndices.reserve(sel.vertexIndices.size());
            for (uint16_t idx : sel.vertexIndices)
                selection.vertexIndices.push_back(static_cast<uint32_t>(idx));
            selection.vertexWeights = sel.vertexWeights;
            selection.triangleIndices.reserve(sel.faceIndices.size());
            for (uint16_t idx : sel.faceIndices)
                selection.triangleIndices.push_back(static_cast<uint32_t>(idx));
            selection.faceSelectionOffsets = sel.faceSelectionIndices;
            selection.sectionIndices.reserve(sel.faceSelectionIndices2.size());
            for (uint32_t idx : sel.faceSelectionIndices2)
                selection.sectionIndices.push_back(idx);
            selection.needsSections = sel.needSelection;
            mesh.selections.push_back(selection);
        }
    }

    static void convertProperties(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.properties.reserve(odolLOD.namedProperties.properties.size());
        for (const auto& prop : odolLOD.namedProperties.properties)
        {
            NamedProperty property;
            property.name  = prop.property;
            property.value = prop.value;
            mesh.properties.push_back(property);
        }
    }

    static void convertProxies(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.proxies.reserve(odolLOD.proxies.proxies.size());
        for (const auto& p : odolLOD.proxies.proxies)
        {
            Proxy proxy;
            proxy.name           = p.name;
            proxy.selectionIndex = p.namedSelectionIndex;
            proxy.id             = static_cast<int32_t>(p.sequenceId);
            // ODOL stores [right, up, forward, position]
            proxy.transform.Set(
                p.transform[0].x, p.transform[0].y, p.transform[0].z, p.transform[3].x,
                p.transform[1].x, p.transform[1].y, p.transform[1].z, p.transform[3].y,
                p.transform[2].x, p.transform[2].y, p.transform[2].z, p.transform[3].z);
            mesh.proxies.push_back(proxy);
        }
    }

    static void convertEdges(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.edges.mlodIndices   = odolLOD.edges.mlodIndex.edges;
        mesh.edges.vertexIndices = odolLOD.edges.vertexIndex.edges;
    }

    static void convertFrames(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.frames.reserve(odolLOD.frames.frames.size());
        for (const auto& sourceFrame : odolLOD.frames.frames)
        {
            std::vector<Vector3> positions;
            positions.reserve(sourceFrame.bonePositions.size());
            for (const auto& sourcePos : sourceFrame.bonePositions)
                positions.emplace_back(sourcePos.x, sourcePos.y, sourcePos.z);
            mesh.frames.emplace_back(sourceFrame.frameTime, std::move(positions));
        }
    }

    static void convertBounds(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        const auto& bounds    = odolLOD.bounds;
        mesh.boundingBox.min  = Vector3(bounds.minPos.x, bounds.minPos.y, bounds.minPos.z);
        mesh.boundingBox.max  = Vector3(bounds.maxPos.x, bounds.maxPos.y, bounds.maxPos.z);
        mesh.bCenter          = Vector3(bounds.bCenter.x, bounds.bCenter.y, bounds.bCenter.z);
        mesh.bRadius          = bounds.bRadius;
        mesh.orHints          = static_cast<RenderHints>(bounds.orHints);
        mesh.andHints         = static_cast<RenderHints>(bounds.andHints);
    }
};

} // namespace Poseidon::Asset::Formats
