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

namespace Poseidon::Wildlife
{
constexpr float EarthSeaFloorDepth = 14.0f;

inline bool Earth() { return EarthStreaming::Enabled(); }

inline float SeaLevel(float x, float z)
{
    if (Earth())
        return -float(Earth::CurvatureDrop(x, z));
    return GLandscape ? GLandscape->GetSeaLevel() : 0.0f;
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
} // namespace Poseidon::Wildlife
