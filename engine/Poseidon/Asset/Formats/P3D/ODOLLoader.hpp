#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/P3D/P3DStructures.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
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
        if (revision.support != P3D::OdolSupport::Parsed)
            throw P3D::UnsupportedOdolRevision(revision);

        auto odolModel = P3D::readModel(reader);
        return convertToModel(odolModel, sourcePath);
    }

  private:
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
