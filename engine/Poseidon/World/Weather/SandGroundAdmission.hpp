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

// The whole boot/rim support must separately pass this interior and the four
// actual cached quadrants. This leaves retail neighbouring-tile blends alone.
inline bool SandSourceInterior(float u, float v)
{
    return std::isfinite(u) && std::isfinite(v) &&
        u >= 0.12f && u <= 0.88f && v >= 0.12f && v <= 0.88f;
}
} // namespace Poseidon
