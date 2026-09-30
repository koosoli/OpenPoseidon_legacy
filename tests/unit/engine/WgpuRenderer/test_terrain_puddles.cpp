// Standalone CPU policy test; scripts/Test-TerrainPuddles.ps1 compiles without the game/PCH.
#include "../../../../engine/WgpuRenderer/TerrainPuddles.hpp"

#include <cassert>
#include <limits>

using namespace Poseidon::TerrainPuddles;

int main()
{
    for (const auto name : {"Mud", "Soil", "Clay", "Dirt", "Terrains/Common/Surfaces/Dirt_01_BCR.edds",
                            "ca\\chernarus\\data\\mud2_detail_co.paa", "SoIl03"})
        assert(EligibleSurface(name));
    for (const auto name :
         {"", "Grass", "Sand", "Road", "Rock", "dirt_road_01", "mud_rock_01", "mud/dirt_grass_bcr.edds",
          "mud/Grass_01_BCR.edds", "SoiledGrass", "DirtAndRock", "mud/asphalt.edds", "mud/seabed.edds",
          "dirt/mystery.paa", "forest_soil_01", "mud_glass_bcr.edds", "brick_dirt_01", "clay_pebbles_01"})
        assert(!EligibleSurface(name));

    Wetness wet;
    assert(wet.Update(0.0f, 1.0f) == 0.0f);
    const float filled = wet.Update(30.0f, 1.0f);
    assert(filled > 0.63f && filled < 0.64f);
    assert(wet.Update(30.0f, 0.0f) == filled); // paused/duplicate terrain or reflection draw
    const float dried = wet.Update(210.0f, 0.0f);
    assert(dried > 0.23f && dried < 0.24f);
    assert(wet.Update(210.0f, std::numeric_limits<float>::quiet_NaN()) == dried);
    assert(wet.Update(std::numeric_limits<float>::infinity(), 1.0f) == dried);
    assert(wet.Update(1.0f, 1.0f) == 0.0f); // restart/load clock reset
    wet.Update(31.0f, 1.0f);
    wet.Reset();
    assert(wet.value == 0.0f && wet.lastTime == -1.0f); // map reset

    Wetness single, stepped;
    single.Update(0.0f, 0.6f);
    stepped.Update(0.0f, 0.6f);
    single.Update(60.0f, 0.6f);
    for (int frame = 1; frame <= 3600; ++frame)
        stepped.Update(frame / 60.0f, 0.6f);
    assert(std::abs(single.value - stepped.value) < 0.00005f); // frame-rate independent
    assert(single.value > 0.0f && single.value < 1.0f);
    return 0;
}
