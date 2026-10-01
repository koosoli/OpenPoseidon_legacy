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

// The retail envelope remains the hard coverage bound: only remove coverage.
// A compact hot centre and phase-fixed uneven taper keep the broad six-petal
// source from reading as a uniformly white flower. Clear RGB stays warm for
// filtered edges; no geometry, phase timing or additive energy is introduced.
// This runs once on actual decoded sheets, never as per-frame texture noise.
inline bool WarmStockMuzzleSheet(std::string_view name, int width, int height,
                                std::uint8_t* rgba, std::size_t bytes, bool enabled)
{
    if (!enabled || !rgba || StockMuzzleSheetAt(name, width, height) == StockMuzzleSheet::None ||
        bytes != std::size_t(width) * std::size_t(height) * 4) return false;
    const auto sheet = StockMuzzleSheetAt(name, width, height);
    const float phase = float(name[name.size() - 5] - '1');
    const auto smooth = [](float lo, float hi, float value) {
        const float t = std::clamp((value-lo)/(hi-lo),0.0f,1.0f);
        return t*t*(3.0f-2.0f*t);
    };
    for (std::size_t i = 0; i < bytes; i += 4)
    {
        const float a = float(rgba[i + 3]) / 255.0f;
        const float x = (float((i/4) % std::size_t(width)) + 0.5f) * (2.0f/width)-1.0f;
        const float y = (float((i/4) / std::size_t(width)) + 0.5f) * (2.0f/height)-1.0f;
        float envelope, core;
        if (sheet == StockMuzzleSheet::Front)
        {
            const float radius = std::sqrt(x*x+y*y);
            const float angle = std::atan2(y,x);
            // Integer angular frequencies keep the wrap continuous. The
            // asymmetric low-order shape shortens different authored lobes,
            // rather than drawing six equally long new procedural spokes.
            const float limit = 0.52f + 0.13f*std::sin(angle+phase*1.7f)
                + 0.09f*std::cos(3.0f*angle+phase*0.8f)
                + 0.06f*std::sin(5.0f*angle-phase*0.6f);
            const float strength = 0.62f + 0.26f*std::sin(angle+phase*1.3f);
            core = 1.0f-smooth(0.08f,0.22f,radius);
            const float taper = 1.0f-smooth(limit*0.38f,limit,radius);
            envelope = std::max(core,taper*strength*std::pow(a,0.35f));
        }
        else
        {
            const float radius = std::sqrt(x*x + y*y*2.6f);
            core = 1.0f-smooth(0.10f,0.42f,radius);
            const float limit = 0.90f + 0.05f*std::sin(x*5.0f+phase*1.7f);
            envelope = 1.0f-smooth(limit*0.45f,limit,radius);
        }
        envelope = std::clamp(envelope,0.0f,1.0f);
        const float hot = smooth(0.35f,1.0f,a)*core;
        const float authored = std::max({rgba[i], rgba[i + 1], rgba[i + 2]}) / 255.0f;
        const float brightness = authored * (0.78f + 0.22f * hot);
        const float warm[3] = {1.0f, 0.40f + 0.56f * hot, 0.07f + 0.73f * hot};
        for (int channel = 0; channel < 3; ++channel)
            rgba[i + channel] = std::uint8_t(std::lround(255.0f * brightness * warm[channel]));
        // Rounding cannot add coverage: the multiplier is in [0,1]. Authored
        // clear holes and border texels remain exactly clear in the input sheet.
        rgba[i + 3] = std::uint8_t(std::lround(float(rgba[i + 3])*envelope));
    }
    return true;
}
} // namespace Poseidon::render
