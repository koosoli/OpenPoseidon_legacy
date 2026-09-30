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

inline bool StockNogovaMudSoil(std::string_view world, std::string_view texture,
                               std::string_view surfaceClass, std::string_view files,
                               std::string_view sound, std::string_view character,
                               float u, float v)
{
    // Noe palette 8/12 refer to retail O.pbo pole1/pole2: authored cultivated
    // soil, not the surrounding grass. Noe CfgSurfaces omits both; require the
    // unchanged base Default fallback instead of interpreting all Default soil.
    // pole3 is not referenced by the retail Noe palette and is not admitted.
    return StockMudSourcePath(world, "noe/noe.wrp") &&
        (StockMudSourcePath(texture, "o/pole1.paa") || StockMudSourcePath(texture, "o/pole2.paa")) &&
        StockMudSourcePath(surfaceClass, "default") && files == "default" && sound == "normalExt" &&
        character.empty() && std::isfinite(u) && std::isfinite(v) &&
        u >= 0.12f && u <= 0.88f && v >= 0.12f && v <= 0.88f;
}
} // namespace Poseidon
