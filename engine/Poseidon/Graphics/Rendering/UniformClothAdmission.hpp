#pragma once

#include <Poseidon/Graphics/Rendering/AsciiSearch.hpp>
#include <algorithm>
#include <cmath>
#include <string_view>

namespace Poseidon::render
{
enum class UniformCloth : unsigned char { None, Plain, WestAtlas, EastAtlas };

// Exact ASCII asset identity, accepting the VFS's two separator spellings.
inline bool UniformAssetEquals(std::string_view value, std::string_view expected)
{
    if (value.size() != expected.size()) return false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char a = value[i] == '/' ? '\\' : AsciiLower(value[i]);
        if (a != expected[i]) return false;
    }
    return true;
}

// These stock ODOL7 materials were decoded from CWA Data3D.pbo. Never infer
// cloth from a Person/first-person-body flag: their weapon proxies use that
// same owner. Callers must first establish the owner's primary LOD identity.
inline UniformCloth AdmitUniformCloth(std::string_view model, std::string_view texture)
{
    if (UniformAssetEquals(model, "data3d\\mc vojakw2.p3d") &&
        UniformAssetEquals(texture, "merged\\00007mc_vojakw2.paa"))
        return UniformCloth::WestAtlas;
    if (UniformAssetEquals(model, "data3d\\mc vojake2.p3d") &&
        UniformAssetEquals(texture, "merged\\00008mc_vojake2.paa"))
        return UniformCloth::EastAtlas;
    constexpr std::string_view cloth[] = {
        "data\\g_hrud_p.pac", "data\\g_zada_z.pac", "data\\g_nohy_p.pac", "data\\g_nohy_z.pac",
        "data\\g2_hrud_p.pac", "data\\g2_zada_z.pac", "data\\g2_nohy_p.pac", "data\\g2_nohy_z.pac",
        "data\\e_pilot_hrud_p.pac", "data\\e_pilot_zada_z.pac", "data\\e_pilot_nohy_p.pac", "data\\e_pilot_nohy_z.pac",
        "data\\w_pilot_hrud_p.pac", "data\\w_pilot_zada_z.pac", "data\\w_pilot_nohy_p.pac", "data\\w_pilot_nohy_z.pac",
        "data\\e_tank_hrud_p.pac", "data\\e_tank_zada_z.pac", "data\\e_tank_nohy_p.pac", "data\\e_tank_nohy_z.pac",
        "data\\w_tank_hrud_p.pac", "data\\w_tank_zada_z.pac", "data\\w_tank_nohy_p.pac", "data\\w_tank_nohy_z.pac"
    };
    for (auto name : cloth)
        if (UniformAssetEquals(texture, name)) return UniformCloth::Plain;
    return UniformCloth::None;
}

// The previously unused local-specular w lane contains wetness and a bounded
// atlas-mask category, without changing WgrDraw3D/MaterialUbo layout. Dry is
// always EXACTLY zero. shader3d.wgsl decodes the same ranges (0..1, 2..3, 4..5).
inline float EncodeUniformWetness(UniformCloth cloth, float wetness)
{
    if (cloth == UniformCloth::None || !std::isfinite(wetness) || wetness <= 0.0f) return 0.0f;
    const float w = std::min(wetness, 1.0f);
    if (cloth == UniformCloth::WestAtlas) return 2.0f + w;
    if (cloth == UniformCloth::EastAtlas) return 4.0f + w;
    return w;
}
} // namespace Poseidon::render
