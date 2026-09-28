#pragma once

#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Poseidon::Streaming
{
// Pure first slice: a caller supplies an already-owned physical .rvmat member.
// No VFS, bank, texture store, global preprocessor, or GPU work occurs here.
// An all-or-none result avoids representing partial/unsupported text as ready.
enum class RapStageNamesStatus : uint8_t
{
    Complete,
    NotRap,
    OverLimit,
    Invalid,
    Unsupported
};

struct RapStageNames
{
    RapStageNamesStatus status = RapStageNamesStatus::Invalid;
    std::vector<std::string> names; // canonical virtual .paa/.pac keys, no source lease
    std::string normalMapName; // shader-schema NormalMap slot, not a filename-suffix guess
};

inline RapStageNames ExtractOwnedRapStageNames(std::span<const char> ownedBytes,
                                               std::string_view virtualMaterialPath)
{
    constexpr size_t MaxBytes = 256 * 1024;
    constexpr size_t MaxStages = 16;
    constexpr size_t MaxName = 240;
    if (virtualMaterialPath.size() > MaxName)
        return {RapStageNamesStatus::OverLimit, {}};
    const auto materialPath = Asset::VirtualPath::Parse(virtualMaterialPath);
    if (materialPath.extension() != ".rvmat" || materialPath.looksHostAbsolute())
        return {RapStageNamesStatus::Unsupported, {}};
    if (ownedBytes.size() > MaxBytes)
        return {RapStageNamesStatus::OverLimit, {}};
    if (ownedBytes.size() < 4 || ownedBytes[0] != 0 || ownedBytes[1] != 'r' ||
        ownedBytes[2] != 'a' || ownedBytes[3] != 'P')
        return {RapStageNamesStatus::NotRap, {}}; // text and includes remain on owner fallback
    try
    {
        const std::vector<uint8_t> bytes(ownedBytes.begin(), ownedBytes.end());
        Asset::Config::ArmaRapParseLimits limits;
        limits.maxDepth = 24;
        limits.maxMembers = 64;
        limits.maxArrayValues = 64;
        limits.maxNodes = 512;
        const auto material = Asset::Material::ParseArmaRapMaterial(bytes,
            std::string(virtualMaterialPath), limits);
        if (material.stages.size() > MaxStages)
            return {RapStageNamesStatus::OverLimit, {}};
        RapStageNames result{RapStageNamesStatus::Complete, {}};
        for (const auto& stage : material.stages)
        {
            if (stage.texture.raw.empty() || stage.texture.isProcedural) continue;
            const auto& path = stage.texture.path;
            if (stage.texture.raw.size() > MaxName || path.canonical().size() > MaxName)
                return {RapStageNamesStatus::OverLimit, {}};
            if (path.empty() || path.looksHostAbsolute() ||
                (path.extension() != ".paa" && path.extension() != ".pac"))
                return {RapStageNamesStatus::Unsupported, {}};
            if (std::find(result.names.begin(), result.names.end(), path.canonical()) == result.names.end())
                result.names.push_back(path.canonical());
        }
        const auto translated = Asset::Material::TranslateMaterial(material);
        if (translated.schemaKnown)
        {
            const auto& normal = translated.Slot(Asset::Material::MaterialSlot::NormalMap);
            if (normal.present && !normal.texture.isProcedural)
                result.normalMapName = normal.texture.path.canonical();
        }
        return result;
    }
    catch (...)
    {
        return {RapStageNamesStatus::Invalid, {}};
    }
}
} // namespace Poseidon::Streaming
