#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace Poseidon
{
// One model job's optional texture inputs. Readers remain responsible for their
// archive/loose-file route and inheritance fold; this helper owns no engine state.
// identity(path) is canonical only for native archive entries, exact for loose
// paths. read(path, material, complete) may return a valid partial material when
// inheritance failed: its available inputs are kept, but it is not deduplicated.
// All dedup state ends with this call, including on cancellation or exception.
template<class MaterialIdentity, class MaterialReader>
std::vector<std::string> CollectObjectStreamTextureNames(const Model::Model& model,
    bool nativeArchiveModel, bool nativeDds, MaterialIdentity&& identity, MaterialReader&& read)
{
    std::vector<std::string> names;
    std::unordered_set<std::string> completedMaterials;
    auto noteName = [&](const std::string& raw)
    {
        if (raw.empty()) return;
        const size_t dot = raw.find_last_of('.');
        if (dot == std::string::npos) return;
        std::string ext = raw.substr(dot);
        for (char& c : ext)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool dds = nativeArchiveModel && nativeDds && (ext == ".edds" || ext == ".dds");
        if (ext != ".paa" && ext != ".pac" && !dds) return;
        std::string key = render::PreparedTextureStore::Key(raw);
        if (std::find(names.begin(), names.end(), key) == names.end())
            names.push_back(std::move(key));
    };
    for (const auto& level : model.lodLevels)
        for (const auto& material : level.mesh.materials)
        {
            // Repeated materials may still carry different face/embedded textures.
            noteName(material.texturePath);
            for (const auto& stage : material.embeddedStages) noteName(stage.texturePath);
            const auto& path = material.materialPath;
            if (!Asset::Material::IsEmatMaterialPath(path)) continue;
            const std::string key = identity(path);
            if (!key.empty() && completedMaterials.count(key)) continue;
            Asset::Material::EmatMaterial emat;
            bool complete = false;
            if (!read(path, emat, complete) || !emat.valid()) continue;
            if (complete && !key.empty()) completedMaterials.insert(key);
            for (const auto& property : emat.properties)
                for (const auto& value : property.values)
                    if (value.isTexture()) noteName(value.text);
            if (nativeArchiveModel)
            {
                // Keep the exact own-colour coverage name. Tint/layer composites
                // remain on the established main-thread path; do not split names.
                std::string colour = emat.TextureOf("BCRMap");
                if (colour.empty()) colour = emat.TextureOf("AlbedoMap");
                const std::string opacity = emat.TextureOf("OpacityMap");
                if (!colour.empty() && !opacity.empty())
                    noteName("enfa|" + opacity + "|" + colour);
            }
        }
    return names;
}
} // namespace Poseidon
