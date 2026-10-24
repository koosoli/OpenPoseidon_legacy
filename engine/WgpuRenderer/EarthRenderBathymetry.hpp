#pragma once
#include "EarthCoordinates.hpp"

namespace Poseidon::Earth
{
// The ground under the sea is the real sea floor: EarthTerrainStream::WithSeaDepth takes Terrarium's bathymetry
// where its fine zooms fill the open sea with 0 m. Until 2026-10-24 the renderer drew a stand-in floor 200 m down
// wherever the ground lay in -5..0.5 m (which also sank low dry land), and the camera, the boat and the fish each
// worked round the 0 m "ground". Only the globe's overview sphere still sits this far under the sea datum, as an
// opaque background beneath the water where no streamed ground reaches.
constexpr float RenderOceanDepth = 200.0f;
}
