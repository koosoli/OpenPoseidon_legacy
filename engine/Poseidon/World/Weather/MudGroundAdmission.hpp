#pragma once
#include <cmath>
#include <string_view>

namespace Poseidon
{
// Explicit original CWA cultivated-soil identity. A class alias alone, a dirt
// sound alone, or an old all-four-quadrants renderer material flag is not proof.
inline bool StockMudSoil(std::string_view surfaceClass, std::string_view files,
                         std::string_view sound, std::string_view character)
{
    return surfaceClass == "Field" && files == "pol" && sound == "dirt" && character.empty();
}

// Exact stock virtual source identities, normalising only path slash/case and
// an optional leading root slash. Never accept an arbitrary matching basename.
inline bool StockMudSourcePath(std::string_view actual, std::string_view expected)
{
    if (!actual.empty() && (actual.front() == '/' || actual.front() == '\\')) actual.remove_prefix(1);
    if (actual.size() != expected.size()) return false;
    for (size_t i = 0; i < actual.size(); ++i)
    {
        char c = actual[i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
        if (c != expected[i]) return false;
    }
    return true;
}

inline bool StockCultivatedMudSoil(std::string_view texture,
                               std::string_view surfaceClass, std::string_view files,
                               std::string_view sound, std::string_view character,
                               float u, float v)
{
    // Noe palette 8/12 refer to retail O.pbo pole1/pole2: authored cultivated
    // soil, not the surrounding grass. Noe CfgSurfaces omits both; require the
    // unchanged base Default fallback instead of interpreting all Default soil.
    // These actual authored assets remain soil when reused by another OFP map.
    // No map filename whitelist, generic brown ground or substring admission.
    // pole3 lacks the same audited source evidence and is not admitted.
    return (StockMudSourcePath(texture, "o/pole1.paa") || StockMudSourcePath(texture, "o/pole2.paa")) &&
        StockMudSourcePath(surfaceClass, "default") && files == "default" && sound == "normalExt" &&
        character.empty() && std::isfinite(u) && std::isfinite(v) &&
        u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f;
}

// Compatibility name for material setup and harness callers. Authority is the
// exact soil asset/metadata and complete footprint support, not the world name.
inline bool StockNogovaMudSoil(std::string_view world, std::string_view texture,
                               std::string_view surfaceClass, std::string_view files,
                               std::string_view sound, std::string_view character,
                               float u, float v)
{
    (void)world;
    return StockCultivatedMudSoil(texture, surfaceClass, files, sound, character, u, v);
}
} // namespace Poseidon
