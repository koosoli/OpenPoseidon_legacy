#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
namespace Poseidon::ForestSnowSurface
{
// Renderer-private bits in the EXISTING flags. No buffer/layout/vertex changes.
enum class Atlas : uint32_t { None, StromkyStrip, ForestLeft, CwaNear, AbelNear,
    Foliage, Shrub, PassableStrip, DarkStrip, AbelSides };
inline constexpr uint32_t AtlasShift = 16, AtlasMask = 255u << AtlasShift;
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
struct TexturePolicy { std::string_view path; Atlas atlas; };
inline constexpr std::array<TexturePolicy, 29> Textures{{
    {"merged\\00002.paa", Atlas::ForestLeft},
    {"data\\les_dark_new.pac", Atlas::DarkStrip},
    {"merged\\00001&krovi4.paa", Atlas::CwaNear},
    {"data\\les_borovice2.pac", Atlas::Foliage},
    {"data\\les_jehlic2.pac", Atlas::Foliage},
    {"data\\les_jehlicnan2.pac", Atlas::Foliage},
    {"data\\krovi1.pac", Atlas::Shrub},
    {"data\\krovi6.pac", Atlas::Shrub},
    {"data\\krovi4_start.pac", Atlas::Shrub},
    {"data\\les2.pac", Atlas::Foliage},
    {"data\\okrajlesa.pac", Atlas::Foliage},
    {"data\\kmen1_les.pac", Atlas::None},
    {"data\\kmen2_les.pac", Atlas::None},
    {"data\\kmen_borovice.pac", Atlas::None},
    {"data\\lesni_kmen_smrku.pac", Atlas::None},
    {"data\\stromky2.pac", Atlas::StromkyStrip},
    {"data\\les_pruchozi_lod.pac", Atlas::PassableStrip},
    {"data\\kerik_bobul_02.pac", Atlas::Shrub},
    {"data\\krovi4_konec.pac", Atlas::Shrub},
    {"data\\stromky.pac", Atlas::StromkyStrip},
    {"data\\okrajtroj.pac", Atlas::Foliage},
    {"merged\\00011&str_fikovnik.paa", Atlas::AbelNear},
    {"data\\abel_krovi1b.pac", Atlas::Shrub},
    {"data\\krovi7.pac", Atlas::Shrub},
    {"data\\abelmlazi.pac", Atlas::Shrub},
    {"data\\n_strom_01_kura.pac", Atlas::None},
    {"data\\lesabeltop.pac", Atlas::Foliage},
    {"data\\lesabelnew.pac", Atlas::AbelSides},
    {"data\\abelpruchlod.pac", Atlas::Shrub},
}};
struct ModelPolicy
{
    std::string_view path;
    int visualLevels;
    std::array<float,4> resolutions;
    // Exact texture membership per visual LOD; None/bark stays excluded.
    std::array<uint32_t,4> textureMasks;
};
// Actual stock Data3D.pbo model inspect --all: 13 models / 45 visual LODs.
// No filename-prefix, texture-colour or component-connectivity inference.
inline constexpr std::array<ModelPolicy, 13> Models{{
    {"data3d\\les ctverec dark.p3d", 2, {3,20,0,0},
        {0x3u,0x3u,0x0u,0x0u}},
    {"data3d\\les ctverec mlazi.p3d", 3, {3,15,40,0},
        {0x1fcu,0x61cu,0x400u,0x0u}},
    {"data3d\\les ctverec pruchozi_t1.p3d", 3, {2,6,12,0},
        {0x3804u,0x3004u,0x1u,0x0u}},
    {"data3d\\les ctverec pruchozi_t2.p3d", 3, {2,6,12,0},
        {0x3804u,0x3004u,0x1u,0x0u}},
    {"data3d\\les ctverec.p3d", 4, {3,10,20,40},
        {0x6004u,0x8000u,0x8000u,0x8000u}},
    {"data3d\\les trojuhelnik pruchozi.p3d", 4, {2,6,12,40},
        {0x1804u,0x1004u,0x10005u,0x10001u}},
    {"data3d\\les trojuhelnik.p3d", 4, {3,16,30,60},
        {0x663fcu,0x188000u,0x188000u,0x188000u}},
    {"data3d\\les_su_ctver_mlaz.p3d", 3, {3,15,40,0},
        {0xe000c4u,0xe000c4u,0x1000000u,0x0u}},
    {"data3d\\les_su_ctver_pruhozi_t1.p3d", 4, {1,7,16,40},
        {0x2200000u,0x200000u,0x200000u,0x4200000u}},
    {"data3d\\les_su_ctver_pruhozi_t2.p3d", 4, {1,7,16,40},
        {0x2200000u,0x200000u,0x200000u,0x4200000u}},
    {"data3d\\les_su_ctver_pruhozi.p3d", 4, {1,7,16,40},
        {0x2200000u,0x200000u,0x200000u,0x4200000u}},
    {"data3d\\les_su_ctver.p3d", 3, {3,16,40,0},
        {0xc000000u,0xc000000u,0xc000000u,0x0u}},
    {"data3d\\les_su_trojuhelnik.p3d", 4, {1,7,16,40},
        {0x2e000c4u,0xe000c4u,0xe000c4u,0x14200000u}},
}};
inline const ModelPolicy* FindModel(std::string_view model)
{
    for (const auto& p : Models) if (AssetEquals(model,p.path)) return &p;
    return nullptr;
}
inline bool AuditedModel(std::string_view model) { return FindModel(model) != nullptr; }
inline bool AuditedLevel(std::string_view model, int level, float resolution)
{
    const auto* p = FindModel(model);
    return p && level >= 0 && level < p->visualLevels &&
        std::isfinite(resolution) && p->resolutions[level] == resolution;
}
inline bool OwnerAllowed(std::string_view model, bool primary, bool isStatic,
                         bool intact, bool knownForestClass, bool mergedForest,
                         int conformMode, bool auditedLevelLoaded)
{
    // MAYANIMATE here is only the exact ForestPlain/Forest terrain plane/skew.
    return AuditedModel(model) && primary && isStatic && intact && knownForestClass &&
        mergedForest && (conformMode == 0 || conformMode == 1) && auditedLevelLoaded;
}
inline Atlas AdmitAtlas(std::string_view model, int level, float resolution,
                        std::string_view texture, bool retainedCutout,
                        bool unchangedFaceAlbedo, bool ordinaryUnnamedMaterial)
{
    if (!retainedCutout || !unchangedFaceAlbedo || !ordinaryUnnamedMaterial ||
        !AuditedLevel(model,level,resolution)) return Atlas::None;
    const auto* p = FindModel(model);
    for (size_t i = 0; i < Textures.size(); ++i)
        if ((p->textureMasks[level] & (1u << i)) != 0 && AssetEquals(texture,Textures[i].path))
            return Textures[i].atlas;
    return Atlas::None;
}
inline uint32_t Encode(Atlas atlas) { return uint32_t(atlas) << AtlasShift; }
}
