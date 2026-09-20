#pragma once
#include <Poseidon/World/Model/Model.hpp>
#include <type_traits>

namespace Poseidon::Model
{
// Conservative owned payload estimate, including spare vector capacity and strings.
// Excludes allocator metadata, decoder scratch, textures and converted Shape storage.
// Strings include SSO capacity, so this is not a process-RSS or reclaimable-GPU metric.
inline uint64_t ResidentPayloadBytes(const Model& model)
{
    const auto vectorBytes = [](const auto& v) -> uint64_t {
        return static_cast<uint64_t>(v.capacity()) * sizeof(typename std::decay_t<decltype(v)>::value_type);
    };
    const auto stringBytes = [](const std::string& s) -> uint64_t { return s.capacity() + 1; };
    uint64_t bytes = sizeof(Model) + vectorBytes(model.lodLevels) + vectorBytes(model.massArray);
    for (const auto* s : {&model.sourcePath, &model.sourceFormat, &model.memory, &model.geometry,
            &model.geometryFire, &model.geometryView, &model.geometryViewPilot, &model.geometryViewGunner,
            &model.geometryViewCommander, &model.geometryViewCargo, &model.landContact, &model.roadway,
            &model.paths, &model.hitpoints, &model.remarks})
        bytes += stringBytes(*s);
    for (const auto& [key, value] : model.metadata)
        bytes += sizeof(decltype(model.metadata)::value_type) + stringBytes(key) + stringBytes(value);
    for (const auto& lod : model.lodLevels)
    {
        const auto& mesh = lod.mesh;
        bytes += stringBytes(lod.sourceEncoding) + stringBytes(lod.sourceWinding) + vectorBytes(lod.uvChannels);
        for (const auto& channel : lod.uvChannels) bytes += vectorBytes(channel.faceVertexUVs);
        bytes += vectorBytes(mesh.vertices) + vectorBytes(mesh.triangles) + vectorBytes(mesh.quads) +
                 vectorBytes(mesh.materials) + vectorBytes(mesh.selections) + vectorBytes(mesh.properties) +
                 vectorBytes(mesh.proxies) + vectorBytes(mesh.sections) + vectorBytes(mesh.frames) +
                 vectorBytes(mesh.vertexMass) + vectorBytes(mesh.edges.mlodIndices) + vectorBytes(mesh.edges.vertexIndices);
        for (const auto& material : mesh.materials)
        {
            bytes += stringBytes(material.name) + stringBytes(material.texturePath) +
                     stringBytes(material.materialPath) + vectorBytes(material.embeddedStages);
            for (const auto& stage : material.embeddedStages) bytes += stringBytes(stage.texturePath);
        }
        for (const auto& selection : mesh.selections)
            bytes += stringBytes(selection.name) + vectorBytes(selection.vertexIndices) + vectorBytes(selection.vertexWeights) +
                     vectorBytes(selection.sourceVertexWeights) + vectorBytes(selection.triangleIndices) +
                     vectorBytes(selection.faceSelectionOffsets) + vectorBytes(selection.sectionIndices);
        for (const auto& property : mesh.properties) bytes += stringBytes(property.name) + stringBytes(property.value);
        for (const auto& proxy : mesh.proxies) bytes += stringBytes(proxy.name);
        for (const auto& frame : mesh.frames) bytes += vectorBytes(frame.positions);
    }
    return bytes;
}
}
