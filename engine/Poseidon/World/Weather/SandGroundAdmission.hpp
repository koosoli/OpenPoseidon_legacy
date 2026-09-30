#pragma once
#include <cmath>
#include <string_view>

namespace Poseidon
{
// Exact authored retail identities, never texture-name/colour/sound guesses.
// SandBuilding is intentionally not part of the terrain sand slice.
inline bool StockSandSurface(std::string_view surfaceClass, std::string_view files,
                             std::string_view sound, std::string_view character)
{
    return sound == "sand" && character.empty() &&
        ((surfaceClass == "Sand" && files == "ps??????") ||
         (surfaceClass == "SandAbel" && files == "pi??????") ||
         (surfaceClass == "SandDark" && files == "pt??????"));
}

// Valid local coordinates only. The contact producer independently proves all
// four actual quadrants in EVERY tile touched by the whole boot/rim support.
// An arbitrary outer-12% ban created multi-metre holes even between pure-sand
// tiles; mixed/hard neighbours remain excluded by that actual source proof.
inline bool SandSourceInterior(float u, float v)
{
    return std::isfinite(u) && std::isfinite(v) &&
        u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f;
}
} // namespace Poseidon
