#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
namespace Poseidon::ForestSnowSurface
{
// Renderer-private bits in the EXISTING flags. No buffer/layout/vertex changes.
enum class Atlas : uint32_t { None, StromkyStrip, ForestLeft, CwaNear, AbelNear,
    Foliage, Shrub, PassableStrip, DarkStrip, AbelSides, NoeStrip, BroadleafCrown,
    BroadleafPair, LowerFoliage, PineSplit, SparseBushRight };
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
inline constexpr std::array<TexturePolicy, 57> Textures{{
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
    {"o\\tree\\akat01_krd2.pac", Atlas::None},
    {"o\\tree\\dd_lest01.pac", Atlas::BroadleafCrown},
    {"o\\tree\\dd_lest02.pac", Atlas::Foliage},
    {"o\\tree\\dd_lest03.pac", Atlas::BroadleafPair},
    {"o\\tree\\dd_bush07.pac", Atlas::Shrub},
    {"o\\tree\\dd_bush06.pac", Atlas::SparseBushRight},
    {"o\\tree\\dd_lesd02.pac", Atlas::Foliage},
    {"o\\tree\\dd_lesd03.pac", Atlas::BroadleafPair},
    {"data\\les_jehlic_spod.pac", Atlas::Foliage},
    {"data\\krovi_trat2.pac", Atlas::Shrub},
    {"data\\krovi_trat1.pac", Atlas::Shrub},
    {"data\\krovi_trat3.pac", Atlas::Shrub},
    {"data\\kerik_list_03.pac", Atlas::Shrub},
    {"data\\kmenletokruh.pac", Atlas::None},
    {"data\\n_trs_01.pac", Atlas::Shrub},
    {"data\\smrcicicek.pac", Atlas::LowerFoliage},
    {"data\\afn_strom_04_new2.pac", Atlas::PineSplit},
    {"data\\les_jehlicnan1.pac", Atlas::Foliage},
    {"data\\les_jehlic1.pac", Atlas::Foliage},
    {"data\\les1.pac", Atlas::Foliage},
    {"data\\les_borovice1.pac", Atlas::Foliage},
    {"data\\les_suche_vetvicky.pac", Atlas::None},
    {"data\\krovi4.pac", Atlas::Shrub},
    {"o\\tree\\dd_les_kura03.pac", Atlas::None},
    {"o\\tree\\dd_les_vetve04.pac", Atlas::Foliage},
    {"o\\tree\\dd_les_vetvicky.pac", Atlas::None},
    {"o\\tree\\kmenletokruh.pac", Atlas::None},
    {"o\\tree\\dd_les_lod.pac", Atlas::NoeStrip},
}};
static_assert(Textures.size() <= 64);
struct ModelPolicy
{
    std::string_view path;
    int visualLevels;
    std::array<float,4> resolutions;
    // Exact texture membership per visual LOD; None/bark stays excluded.
    std::array<uint64_t,4> textureMasks;
};
// Actual stock Data3D.pbo + O.pbo model inspect --all: 25 models / 84 visual LODs.
// No filename-prefix, texture-colour or component-connectivity inference.
inline constexpr std::array<ModelPolicy, 25> Models{{
    {"data3d\\les ctverec dark.p3d", 2, {3,20,0,0},
        {0x3ull,0x3ull,0x0ull,0x0ull}},
    {"data3d\\les ctverec mlazi.p3d", 3, {3,15,40,0},
        {0x1fcull,0x61cull,0x400ull,0x0ull}},
    {"data3d\\les ctverec pruchozi_t1.p3d", 3, {2,6,12,0},
        {0x3804ull,0x3004ull,0x1ull,0x0ull}},
    {"data3d\\les ctverec pruchozi_t2.p3d", 3, {2,6,12,0},
        {0x3804ull,0x3004ull,0x1ull,0x0ull}},
    {"data3d\\les ctverec.p3d", 4, {3,10,20,40},
        {0x6004ull,0x8000ull,0x8000ull,0x8000ull}},
    {"data3d\\les trojuhelnik pruchozi.p3d", 4, {2,6,12,40},
        {0x1804ull,0x1004ull,0x10005ull,0x10001ull}},
    {"data3d\\les trojuhelnik.p3d", 4, {3,16,30,60},
        {0x663fcull,0x188000ull,0x188000ull,0x188000ull}},
    {"data3d\\les_su_ctver_mlaz.p3d", 3, {3,15,40,0},
        {0xe000c4ull,0xe000c4ull,0x1000000ull,0x0ull}},
    {"data3d\\les_su_ctver_pruhozi_t1.p3d", 4, {1,7,16,40},
        {0x2200000ull,0x200000ull,0x200000ull,0x4200000ull}},
    {"data3d\\les_su_ctver_pruhozi_t2.p3d", 4, {1,7,16,40},
        {0x2200000ull,0x200000ull,0x200000ull,0x4200000ull}},
    {"data3d\\les_su_ctver_pruhozi.p3d", 4, {1,7,16,40},
        {0x2200000ull,0x200000ull,0x200000ull,0x4200000ull}},
    {"data3d\\les_su_ctver.p3d", 3, {3,16,40,0},
        {0xc000000ull,0xc000000ull,0xc000000ull,0x0ull}},
    {"data3d\\les_su_trojuhelnik.p3d", 4, {1,7,16,40},
        {0x2e000c4ull,0xe000c4ull,0xe000c4ull,0x14200000ull}},
    {"o\\tree\\les ctverec geom.p3d", 2, {1,7,0,0},
        {0x60000000ull,0x180000000ull,0x0ull,0x0ull}},
    {"o\\tree\\les_nw_ctver_pruhozi_t1.p3d", 3, {1,7,10,0},
        {0x660000000ull,0x1e00000000ull,0x1800000000ull,0x0ull}},
    {"o\\tree\\les_nw_ctver_pruhozi_t2.p3d", 3, {1,7,10,0},
        {0x660000000ull,0x1e00000000ull,0x1800000000ull,0x0ull}},
    {"o\\tree\\les_nw_ctver_pruhozi.p3d", 2, {1,7,0,0},
        {0x60000000ull,0x180000000ull,0x0ull,0x0ull}},
    {"o\\tree\\les_nw_ctver.p3d", 3, {3,16,40,0},
        {0xc000000ull,0xc000000ull,0xc000000ull,0x0ull}},
    {"o\\tree\\les_nw_jehl_ctver_pruhozi.p3d", 4, {2,6,12,40},
        {0x7ffe000003800ull,0xfff8000003000ull,0x10000ull,0x10002ull}},
    {"o\\tree\\les_nw_jehl_ctver.p3d", 4, {3,10,20,40},
        {0x3c00000006000ull,0x8000ull,0x8000ull,0x8000ull}},
    {"o\\tree\\les_nw_jehl_mlaz.p3d", 3, {3,15,40,0},
        {0x2d000000001f8ull,0x3900000000618ull,0x400ull,0x0ull}},
    {"o\\tree\\les_nw_jehl_t1.p3d", 4, {2,6,12,40},
        {0xf0000600000000ull,0x70000600000000ull,0x100000000000000ull,0x100000000000000ull}},
    {"o\\tree\\les_nw_jehl_t2.p3d", 4, {2,6,12,40},
        {0xf0000600000000ull,0x30000600000000ull,0x100000000000000ull,0x100000000000000ull}},
    {"o\\tree\\les_nw_jehl_trojuhelnik.p3d", 4, {2,6,12,40},
        {0xf0000600000000ull,0x30000600000000ull,0x100000000000000ull,0x100000000000000ull}},
    {"o\\tree\\les_nw_trojuhelnik.p3d", 3, {1,7,10,0},
        {0x660000000ull,0x1e00000000ull,0x1800000000ull,0x0ull}},
}};
inline const ModelPolicy* FindModel(std::string_view model)
{
    for (const auto& p : Models) if (AssetEquals(model,p.path)) return &p;
    return nullptr;
}
inline bool AuditedModel(std::string_view model) { return FindModel(model) != nullptr; }
inline int SourceVisualIndex(const ModelPolicy& policy, float resolution)
{
    if (!std::isfinite(resolution)) return -1;
    int found = -1;
    for (int source = 0; source < policy.visualLevels; ++source)
        if (policy.resolutions[source] == resolution)
        {
            if (found >= 0) return -1; // ambiguous source resolution cannot prove a tuple
            found = source;
        }
    return found;
}
inline bool AuditedLevel(std::string_view model, int level, float resolution)
{
    const auto* p = FindModel(model);
    // OptimizeShapes deletes a complex prefix / VDecal LODs and compacts slots.
    // A retained slot may shift LEFT only; the unique original resolution still
    // proves its exact source row. No reordering or invented visual level.
    return p && level >= 0 && level <= SourceVisualIndex(*p,resolution);
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
    const int source = SourceVisualIndex(*p,resolution);
    for (size_t i = 0; i < Textures.size(); ++i)
        if ((p->textureMasks[source] & (uint64_t(1) << i)) != 0 && AssetEquals(texture,Textures[i].path))
            return Textures[i].atlas;
    return Atlas::None;
}
inline uint32_t Encode(Atlas atlas) { return uint32_t(atlas) << AtlasShift; }
}
