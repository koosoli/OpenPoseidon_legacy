#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Poseidon::render
{
enum class StockMuzzleSheet { None, Front, Side };

// Audited retail M16 sheets. Other effects, addon paths and resized replacements
// retain their authored appearance. No basename or substring admission.
inline StockMuzzleSheet StockMuzzleSheetAt(std::string_view name, int width, int height)
{
    constexpr std::string_view front = "data/zasleh_front.01.paa";
    constexpr std::string_view side = "data/zasleh_side.01.paa";
    const auto matches = [&](std::string_view expected) {
        if (name.size() != expected.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            char ch = name[i];
            if (ch == '\\') ch = '/';
            if (ch >= 'A' && ch <= 'Z') ch = char(ch + ('a' - 'A'));
            if (i == expected.size() - 5)
            {
                if (ch < '1' || ch > '3') return false;
            }
            else if (ch != expected[i]) return false;
        }
        return true;
    };
    if (width == 128 && height == 128 && matches(front)) return StockMuzzleSheet::Front;
    if (width == 128 && height == 64 && matches(side)) return StockMuzzleSheet::Side;
    return StockMuzzleSheet::None;
}

inline bool StockMuzzleAppearanceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_MUZZLE_FLASH_APPEARANCE");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

inline bool StockMuzzleDrawAdmitted(StockMuzzleSheet sheet, bool animated, bool noShadow,
                                   bool orderedAlpha, bool blend)
{
    return sheet != StockMuzzleSheet::None && animated && noShadow && orderedAlpha && blend;
}

// Alpha is the actual authored hot core/lobe envelope, not a new radial sprite.
// Preserve every alpha byte. Warm clear texels' RGB too: the original greenish
// RGB otherwise bleeds through bilinear filtering at the soft edge. No new
// coverage is created. RGB remains sRGB; the shader decodes once.
inline bool WarmStockMuzzleSheet(std::string_view name, int width, int height,
                                std::uint8_t* rgba, std::size_t bytes, bool enabled)
{
    if (!enabled || !rgba || StockMuzzleSheetAt(name, width, height) == StockMuzzleSheet::None ||
        bytes != std::size_t(width) * std::size_t(height) * 4) return false;
    for (std::size_t i = 0; i < bytes; i += 4)
    {
        const float a = float(rgba[i + 3]) / 255.0f;
        const float hot = a * a * (3.0f - 2.0f * a);
        const float authored = std::max({rgba[i], rgba[i + 1], rgba[i + 2]}) / 255.0f;
        const float brightness = authored * (0.78f + 0.22f * hot);
        const float warm[3] = {1.0f, 0.40f + 0.56f * hot, 0.07f + 0.73f * hot};
        for (int channel = 0; channel < 3; ++channel)
            rgba[i + channel] = std::uint8_t(std::lround(255.0f * brightness * warm[channel]));
    }
    return true;
}
} // namespace Poseidon::render
