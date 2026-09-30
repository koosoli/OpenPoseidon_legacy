#pragma once
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Poseidon::ForestSnowSurface
{
// Renderer-private categories in unused high bits of the EXISTING section flags.
// No public struct, buffer stride, vertex input or crown-centre table changes.
enum class Atlas : uint32_t { None, StromkyStrip, ForestLeft };
inline constexpr uint32_t AtlasShift = 16, AtlasMask = 3u << AtlasShift;
inline constexpr float OwnerProof = 2.0f, FragmentProof = -2.0f;
inline bool AssetEquals(std::string_view value, std::string_view expected)
{
    if (value.size() != expected.size()) return false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        char c = value[i], e = expected[i];
        if (c == '/') c = '\\';
        if (e == '/') e = '\\';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (e >= 'A' && e <= 'Z') e += 'a' - 'A';
        if (c != e) return false;
    }
    return true;
}
inline bool AuditedModel(std::string_view model)
{
    return AssetEquals(model, "data3d\\les ctverec.p3d") ||
        AssetEquals(model, "data3d\\les ctverec pruchozi_t1.p3d");
}
inline bool OwnerAllowed(std::string_view model, bool primary, bool isStatic,
                         bool intact, bool knownForestClass, bool mergedForest,
                         int conformMode, bool auditedLevelLoaded)
{
    // ForestPlain/Forest have only the existing static terrain plane/skew pose.
    // Their MAYANIMATE flag is not a licence for arbitrary animated forest actors.
    return AuditedModel(model) && primary && isStatic && intact && knownForestClass &&
        mergedForest && (conformMode == 0 || conformMode == 1) && auditedLevelLoaded;
}
inline Atlas AdmitAtlas(std::string_view model, int level, float resolution,
                        std::string_view texture, bool retainedCutout,
                        bool unchangedFaceAlbedo, bool ordinaryUnnamedMaterial)
{
    if (!retainedCutout || !unchangedFaceAlbedo || !ordinaryUnnamedMaterial ||
        !std::isfinite(resolution)) return Atlas::None;
    if (AssetEquals(model, "data3d\\les ctverec.p3d") && level == 3 && resolution == 40 &&
        AssetEquals(texture, "data\\stromky2.pac")) return Atlas::StromkyStrip;
    if (AssetEquals(model, "data3d\\les ctverec pruchozi_t1.p3d") && level == 2 && resolution == 12 &&
        AssetEquals(texture, "merged\\00002.paa")) return Atlas::ForestLeft;
    return Atlas::None;
}
inline uint32_t Encode(Atlas atlas) { return uint32_t(atlas) << AtlasShift; }
}
