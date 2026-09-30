#pragma once
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
} // namespace Poseidon
