// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The sea and the ground as the wildlife (fish, birds, reef) sees them, on a finite OFP map and in
// whole-world (Earth) mode. On a map: the landscape's sea level and terrain. In Earth mode: the
// spherical sea datum and the streamed resident ground; elevation the renderer presents as open
// ocean (EarthRenderBathymetry: -5..0.5 m, no bathymetry) counts as sea with a nominal floor
// `EarthSeaFloorDepth` down -- the drawn floor is far deeper, so only things that swim in open
// water use it, nothing that sits on the floor. Where no ground is resident it answers "land, high"
// so nothing spawns there.
#include "EarthCoordinates.hpp"
#include "EarthRenderBathymetry.hpp"
#include <Poseidon/World/Terrain/EarthFrame.hpp>
#include <Poseidon/World/Terrain/EarthStreamingMode.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace Poseidon::Wildlife
{
constexpr float EarthSeaFloorDepth = 14.0f;

inline bool Earth() { return EarthStreaming::Enabled(); }

// How many animals around the camera, as a multiple of the Tidewater numbers the modules were tuned on:
// WGR_WILDLIFE_DENSITY (0.25-4, default 2).
inline float Density()
{
    static const float d = []
    {
        const char* v = std::getenv("WGR_WILDLIFE_DENSITY");
        const float f = v ? float(std::atof(v)) : 2.0f;
        return f > 0 ? std::min(std::max(f, 0.25f), 4.0f) : 2.0f;
    }();
    return d;
}
// `base` scaled by the density, at least one
inline int Scaled(int base) { return std::max(1, int(base * Density() + 0.5f)); }

inline float SeaLevel(float x, float z)
{
    if (Earth())
        return -float(Earth::CurvatureDrop(x, z));
    return GLandscape ? GLandscape->GetSeaLevel() : 0.0f;
}

// Earth mode's open sea at (x, z): resident ground the renderer presents as open ocean (above).
inline bool OpenSea(float x, float z)
{
    if (!Earth())
        return false;
    const auto sample = EarthStreaming::QueryGround(x, z);
    if (!sample)
        return false;
    const float elevation = sample->height - SeaLevel(x, z);
    return elevation >= Earth::RenderOceanMinElevation && elevation <= Earth::RenderOceanMaxElevation;
}

inline float Ground(float x, float z)
{
    if (Earth())
    {
        const float sea = SeaLevel(x, z);
        const auto sample = EarthStreaming::QueryGround(x, z);
        if (!sample)
            return sea + 1000.0f;
        const float elevation = sample->height - sea;
        if (elevation >= Earth::RenderOceanMinElevation && elevation <= Earth::RenderOceanMaxElevation)
            return sea - EarthSeaFloorDepth;
        return sample->height;
    }
    return GLandscape ? GLandscape->SurfaceY(x, z) : 0.0f;
}
// Bare sand at (x, z)? On a map, from the terrain surface's CfgSurfaces class / sound environment
// ("sand", Czech "pisek", "beach"); in Earth mode there is no surface data: any low ground counts.
inline bool Sand(float x, float z)
{
    if (Earth() || !GLandscape)
        return true;
    const SurfaceInfo& s = GLandscape->SurfaceAt(x, z);
    auto has = [](const RStringB& name)
    {
        const char* p = name;
        if (!p)
            return false;
        std::string l(p);
        for (char& c : l)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return l.find("sand") != std::string::npos || l.find("pisek") != std::string::npos ||
               l.find("beach") != std::string::npos;
    };
    return has(s._class) || has(s._soundEnv) || has(s._name);
}
} // namespace Poseidon::Wildlife
