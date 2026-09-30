#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

namespace Poseidon::TerrainPuddles
{
inline std::string_view SurfaceLeaf(std::string_view name)
{
    return name.substr(name.find_last_of("/\\") == std::string_view::npos ? 0 : name.find_last_of("/\\") + 1);
}

// Read only the source leaf, never a containing directory called "mud". Unknown
// surfaces are dry. Separators/numeric variants are accepted, with optional
// CamelCase for semantic material identities; "Soiled" is not positive evidence.
inline bool HasSurfaceToken(std::string_view name, std::string_view wanted, bool camelCase = false)
{
    name = SurfaceLeaf(name);
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    for (size_t start = 0; start < name.size();)
    {
        if (!letter(name[start]))
        {
            ++start;
            continue;
        }
        size_t end = start + 1;
        while (end < name.size() && letter(name[end]))
        {
            if (camelCase && upper(name[end]) &&
                (lower(name[end - 1]) || (end + 1 < name.size() && lower(name[end + 1]))))
                break;
            ++end;
        }
        bool equal = end - start == wanted.size();
        for (size_t i = 0; equal && i < wanted.size(); ++i)
        {
            const char c = name[start + i];
            equal = (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) == wanted[i];
        }
        if (equal)
            return true;
        start = end;
    }
    return false;
}

inline bool HasSurfaceFragment(std::string_view name, std::string_view fragment)
{
    name = SurfaceLeaf(name);
    for (size_t start = 0; start + fragment.size() <= name.size(); ++start)
    {
        bool equal = true;
        for (size_t i = 0; equal && i < fragment.size(); ++i)
        {
            const char c = name[start + i];
            equal = (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) == fragment[i];
        }
        if (equal)
            return true;
    }
    return false;
}

inline bool EligibleSurface(std::string_view name)
{
    // A combined mud/rock or dirt/grass source is deliberately not a mud surface.
    for (const auto hard : {"sand",     "gravel", "rock",     "stone",  "grass", "forest",  "asphalt",
                            "concrete", "road",   "pavement", "seabed", "snow",  "water",   "glass",
                            "brick",    "rubble", "wood",     "roof",   "metal", "pebbles", "beach"})
        if (HasSurfaceToken(name, hard))
            return false;
    return HasSurfaceToken(name, "mud") || HasSurfaceToken(name, "dirt") || HasSurfaceToken(name, "soil") ||
           HasSurfaceToken(name, "clay");
}

inline bool EligibleNativeSurface(std::string_view semanticName)
{
    // The palette .emat identity is authoritative. An inherited generic dirt
    // BCRMap cannot turn BeachGrass/Forest/unknown surfaces into semantic mud.
    // Native compounds may be concatenated without CamelCase or underscores.
    for (const auto hard : {"sand",  "gravel", "rock", "stone",    "grass",  "forest",  "asphalt", "concrete",
                            "road",  "trail",  "path", "pavement", "seabed", "snow",    "water",   "glass",
                            "brick", "rubble", "wood", "roof",     "metal",  "pebbles", "beach"})
        if (HasSurfaceFragment(semanticName, hard))
            return false;
    return EligibleSurface(semanticName) || HasSurfaceToken(semanticName, "mud", true) ||
           HasSurfaceToken(semanticName, "dirt", true) || HasSurfaceToken(semanticName, "soil", true) ||
           HasSurfaceToken(semanticName, "clay", true);
}

// Cosmetic accumulation; no gameplay material or water-depth mutation. Uses the
// captured simulation clock, so a pause and duplicate reflection draws are no-ops.
struct Wetness
{
    float value = 0.0f;
    float lastTime = -1.0f;

    void Reset()
    {
        value = 0.0f;
        lastTime = -1.0f;
    }
    float Update(float time, float rain)
    {
        if (!std::isfinite(time) || !std::isfinite(rain))
            return value;
        rain = std::clamp(rain, 0.0f, 1.0f);
        if (lastTime < 0.0f || time < lastTime)
        {
            // A fresh mission starts dry; sustained rain fills it, rather than a
            // first-frame full-map glossy flash. A restarted clock clears history.
            value = 0.0f;
            lastTime = time;
            return value;
        }
        const float dt = time - lastTime;
        lastTime = time;
        const float rate = rain / 30.0f + (1.0f - rain) / 180.0f;
        const float equilibrium = (rain / 30.0f) / rate;
        value = std::clamp(equilibrium + (value - equilibrium) * std::exp(-rate * dt), 0.0f, 1.0f);
        return value;
    }
};
} // namespace Poseidon::TerrainPuddles
