#pragma once
#include "EarthCoordinates.hpp"

namespace Poseidon::Earth
{
// Terrarium zero means ocean surface, not measured bathymetry. This proxy is
// ONLY for opaque Earth render meshes / water shading. Never publish it as
// resident gameplay ground or upload it in place of the source heightmap.
// Classifies uncurved elevation in [-5,0.5] m as coastal/ocean presentation.
// This is not a coastline mask: low dry land in that interval can be mistaken
// for sea. Deeper negative terrain and land above 0.5 m remain source-shaped.
constexpr float RenderOceanDepth = 200.0f;
constexpr float RenderOceanMinElevation = -5.0f;
constexpr float RenderOceanMaxElevation = 0.5f;
inline float RenderTerrainHeight(float x, float z, float sourceHeight)
{
    const float seaDatum = -float(CurvatureDrop(x, z));
    const float elevation = sourceHeight - seaDatum;
    return elevation >= RenderOceanMinElevation && elevation <= RenderOceanMaxElevation ? seaDatum - RenderOceanDepth : sourceHeight;
}
}
