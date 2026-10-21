#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Poseidon::render
{
// The retail zasleh sheets, by family: zasleh = M16 birdcage hider (front + side),
// zasleh2 = the three-port AK brake (front only), zasleh3 = open muzzles / MGs
// (front fireball + side). Each comes in animation phases .01-.03.
enum class StockMuzzleSheet { None, Front, Side, FrontTrident, FrontBall, SideBall };

// Exact retail paths and sizes only. Addon paths and resized replacements keep
// their authored appearance. No basename or substring admission.
inline StockMuzzleSheet StockMuzzleSheetAt(std::string_view name, int width, int height)
{
    struct Known { std::string_view path; int w, h; StockMuzzleSheet sheet; };
    static constexpr Known known[] = {
        {"data/zasleh_front.01.paa", 128, 128, StockMuzzleSheet::Front},
        {"data/zasleh_side.01.paa", 128, 64, StockMuzzleSheet::Side},
        {"data/zasleh2_front.01.paa", 128, 128, StockMuzzleSheet::FrontTrident},
        {"data/zasleh3_front.01.paa", 128, 128, StockMuzzleSheet::FrontBall},
        {"data/zasleh3_side.01.paa", 128, 64, StockMuzzleSheet::SideBall},
    };
    for (const Known& k : known)
    {
        if (width != k.w || height != k.h || name.size() != k.path.size()) continue;
        bool same = true;
        for (std::size_t i = 0; same && i < name.size(); ++i)
        {
            char ch = name[i];
            if (ch == '\\') ch = '/';
            if (ch >= 'A' && ch <= 'Z') ch = char(ch + ('a' - 'A'));
            if (i == k.path.size() - 5) same = ch >= '1' && ch <= '3'; // the phase digit
            else same = ch == k.path[i];
        }
        if (same) return k.sheet;
    }
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

namespace muzzle_detail
{
inline float Hash(int x, int y, int seed)
{
    std::uint32_t h = std::uint32_t(x) * 374761393u + std::uint32_t(y) * 668265263u + std::uint32_t(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFFu) / float(0xFFFFFF);
}
inline float Noise(float x, float y, int seed)
{
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    const int ix = int(fx0), iy = int(fy0);
    float fx = x - fx0, fy = y - fy0;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed);
    const float c = Hash(ix, iy + 1, seed), d = Hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
}
inline float Fbm(float x, float y, int seed)
{
    float sum = 0, amp = 0.5f, total = 0;
    for (int octave = 0; octave < 4; ++octave)
    {
        sum += amp * Noise(x, y, seed + octave * 17);
        total += amp;
        x = x * 2.03f + 1.7f;
        y = y * 2.03f + 9.2f;
        amp *= 0.5f;
    }
    return sum / total;
}
inline float Smooth(float lo, float hi, float v)
{
    const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float Scalar(int seed, int k) { return Hash(k, seed * 31 + 7, seed); }

struct FlashTexel { float intensity, hot; };

// Looking down the barrel: a white-hot core, gas jets out of the hider's ports
// (5 for the birdcage, 3 for the AK brake) that widen and break into turbulent
// wisps, and for open muzzles a ragged fireball. Phase-seeded: every animation
// frame is a different flash.
inline FlashTexel Front(float x, float y, int seed, int prongs, float prongLength, float coreRadius, float ball)
{
    const float r = std::sqrt(x * x + y * y);
    const float theta = std::atan2(y, x);
    const float warpedTheta = theta + 0.35f * (Fbm(x * 3 + 5, y * 3 + 2, seed + 101) - 0.5f);
    const float warpedR = r * (1.0f + 0.35f * (Fbm(x * 4, y * 4, seed + 202) - 0.5f));
    float flame = 0;
    const float base = Scalar(seed, 0) * 6.2831853f;
    for (int k = 0; k < prongs; ++k)
    {
        const float angle = base + float(k) * 6.2831853f / float(prongs) + (Scalar(seed, k + 1) - 0.5f) * 0.5f;
        const float length = prongLength * (0.65f + 0.5f * Scalar(seed, k + 11));
        const float delta = std::remainder(warpedTheta - angle, 6.2831853f);
        const float width = 0.10f + 0.22f * warpedR / length;
        const float across = std::exp(-(delta / width) * (delta / width));
        const float along = warpedR / length;
        const float radial = (1.0f - Smooth(0.25f, 1.0f, along)) * (0.35f + 0.65f * std::exp(-along * 1.5f));
        const float streak = Fbm(warpedR * 9 + float(k) * 13.1f, delta * 7 + float(k) * 3.3f, seed + 300 + k);
        const float breakup = Smooth(0.25f, 0.75f, streak + 0.35f - 0.45f * along);
        flame = std::max(flame, across * radial * breakup * (0.8f + 0.4f * Scalar(seed, k + 21)));
    }
    if (ball > 0)
    {
        const float lumps = Fbm(x * 5, y * 5, seed + 400);
        const float radius = ball * (0.75f + 0.5f * lumps);
        flame = std::max(flame, (1.0f - Smooth(0.2f * radius, radius, warpedR)) * (0.55f + 0.45f * lumps));
    }
    const float core = std::exp(-(r / coreRadius) * (r / coreRadius));
    const float glow = 0.30f * std::exp(-r / 0.20f); // hot-gas glow round the core
    float intensity = std::clamp(std::max(flame, core) + glow * (1.0f - core), 0.0f, 1.0f);
    intensity *= 1.0f - Smooth(0.82f, 0.98f, r); // nothing reaches the sheet border
    return {intensity, std::clamp(core * 1.1f + 0.35f * intensity * intensity * intensity, 0.0f, 1.0f)};
}

// From the side: a short jet, white at the muzzle, widening into a
// striated orange plume that thins and breaks up within the sheet.
inline FlashTexel Side(float u, float v, int seed, float length, float bulb)
{
    const float reach = length * (0.75f + 0.35f * Scalar(seed, 1));
    const float t = u / reach;
    const float vv = v + 0.5f * (Fbm(u * 5 + 3, v * 2.5f, seed + 500) - 0.5f) * t;
    const float radius = 0.07f + bulb * std::pow(std::sin(std::clamp(t, 0.0f, 1.0f) * 3.14159265f * 0.9f), 0.8f)
        * (0.8f + 0.4f * Scalar(seed, 2));
    float body = (1.0f - Smooth(0.15f * radius, radius, std::fabs(vv))) * (1.0f - Smooth(0.35f, 1.0f, t));
    const float streak = Fbm(u * 4, vv * 9, seed + 600);
    body *= 0.35f + 0.65f * Smooth(0.15f, 0.75f, streak + 0.3f - 0.35f * t);
    const float plume = body * std::exp(-t * 2.6f);
    const float core = std::exp(-((u - 0.10f) / 0.09f) * ((u - 0.10f) / 0.09f) - (v / 0.22f) * (v / 0.22f));
    float intensity = std::clamp(std::max(plume, core), 0.0f, 1.0f);
    intensity *= (1.0f - Smooth(0.85f, 0.98f, std::fabs(v))) * (1.0f - Smooth(0.93f, 0.995f, u)) * Smooth(0.0f, 0.02f, u);
    return {intensity, std::clamp(core * 1.1f + 0.5f * plume * plume * plume, 0.0f, 1.0f)};
}
} // namespace muzzle_detail

// Replaces the retail sheet with a procedural flash of the same family and size.
// The originals are pale hand-painted smudges (a six-petal flower for the M16)
// that read as cartoon decals however they are tinted. Heat is encoded in the
// chroma, blue/red = 0.07 + 0.73*hot, which shader3d's stock_muzzle_flash_radiance
// decodes into HDR radiance: keep the two in step. Green carries only the
// white->yellow->orange->red ramp. Runs once per decoded sheet, never per frame;
// the engine's alpha classification still comes from the authored file.
inline bool WarmStockMuzzleSheet(std::string_view name, int width, int height,
                                std::uint8_t* rgba, std::size_t bytes, bool enabled)
{
    const auto sheet = StockMuzzleSheetAt(name, width, height);
    if (!enabled || !rgba || sheet == StockMuzzleSheet::None ||
        bytes != std::size_t(width) * std::size_t(height) * 4) return false;
    const int phase = name[name.size() - 5] - '0';
    const int seed = phase + 10 * int(sheet);
    for (int py = 0; py < height; ++py)
    {
        for (int px = 0; px < width; ++px)
        {
            const float x = (float(px) + 0.5f) * (2.0f / float(width)) - 1.0f;
            const float y = (float(py) + 0.5f) * (2.0f / float(height)) - 1.0f;
            muzzle_detail::FlashTexel texel{};
            switch (sheet)
            {
            case StockMuzzleSheet::Front: texel = muzzle_detail::Front(x, y, seed, 5, 0.70f, 0.10f, 0.0f); break;
            case StockMuzzleSheet::FrontTrident: texel = muzzle_detail::Front(x, y, seed, 3, 0.90f, 0.11f, 0.25f); break;
            case StockMuzzleSheet::FrontBall: texel = muzzle_detail::Front(x, y, seed, 6, 0.50f, 0.12f, 0.5f); break;
            // The retail side planes are mapped with the muzzle at u = 1 (seen in
            // game: a u = 0 muzzle pointed the hot end away from the rifle).
            case StockMuzzleSheet::Side: texel = muzzle_detail::Side((1.0f - x) * 0.5f, y, seed, 0.7f, 0.42f); break;
            case StockMuzzleSheet::SideBall: texel = muzzle_detail::Side((1.0f - x) * 0.5f, y, seed, 0.85f, 0.6f); break;
            case StockMuzzleSheet::None: break;
            }
            // Steep falloff: at night exposure anything brighter saturates to one flat
            // orange, so the outer flame must sit well below the core to keep a gradient.
            const float brightness = std::pow(texel.intensity, 1.6f);
            const float warm[3] = {1.0f, std::clamp(0.30f + 0.30f * texel.intensity + 0.40f * texel.hot, 0.0f, 1.0f),
                                   0.07f + 0.73f * texel.hot};
            std::uint8_t* out = rgba + (std::size_t(py) * std::size_t(width) + std::size_t(px)) * 4;
            for (int channel = 0; channel < 3; ++channel)
                out[channel] = std::uint8_t(std::lround(255.0f * std::clamp(brightness * warm[channel], 0.0f, 1.0f)));
            out[3] = std::uint8_t(std::lround(255.0f * std::clamp(texel.intensity, 0.0f, 1.0f)));
        }
    }
    return true;
}
} // namespace Poseidon::render
