// Standalone CPU policy test; scripts/Test-TerrainPuddles.ps1 compiles without the game/PCH.
#include "../../../../engine/WgpuRenderer/TerrainPuddles.hpp"
#include "../../../../engine/Poseidon/World/Entities/Infantry/UniformWetness.hpp"

#include <cassert>
#include <limits>

using namespace Poseidon::TerrainPuddles;

int main()
{
    // Actual terrain is always a receiver; semantic classification is separate
    // soft-soil metadata, never a grass/rock/unknown-map puddle veto.
    static_assert(GroundReceiver == 1u && SoftSoil == 2u && (GroundReceiver & SoftSoil) == 0u);
    for (const auto name : {"Grass", "Rock", "Default", "noe/pole1.paa", "Road"})
    {
        const unsigned flags = GroundReceiver | (EligibleSurface(name) ? SoftSoil : 0u);
        assert((flags & GroundReceiver) != 0u && (flags & SoftSoil) == 0u);
    }
    // Render capture supplies the same finite liquid rain as uniform wetting.
    // Falling snow alone must not accumulate or animate ground water.
    Wetness snowOnly, liquidParticles;
    snowOnly.Update(0.0f, Poseidon::UniformWettingDensity(0.0f, 1.0f, true));
    assert(snowOnly.Update(60.0f, Poseidon::UniformWettingDensity(0.0f, 1.0f, true)) == 0.0f);
    liquidParticles.Update(0.0f, Poseidon::UniformWettingDensity(0.0f, 0.6f, false));
    assert(liquidParticles.Update(60.0f, Poseidon::UniformWettingDensity(0.0f, 0.6f, false)) > 0.0f);

    for (const auto name : {"Mud", "Soil", "Clay", "Dirt", "Terrains/Common/Surfaces/Dirt_01_BCR.edds",
                            "ca\\chernarus\\data\\mud2_detail_co.paa", "SoIl03"})
        assert(EligibleSurface(name));
    for (const auto name :
         {"", "Grass", "Sand", "Road", "Rock", "dirt_road_01", "mud_rock_01", "mud/dirt_grass_bcr.edds",
          "mud/Grass_01_BCR.edds", "SoiledGrass", "DirtAndRock", "mud/asphalt.edds", "mud/seabed.edds",
          "dirt/mystery.paa", "forest_soil_01", "mud_glass_bcr.edds", "brick_dirt_01", "clay_pebbles_01"})
        assert(!EligibleSurface(name));

    // Native semantic palette identity overrides any inherited render texture.
    // The installed integration fixture exposed BeachGrass_01 backed by dirt BCR.
    for (const auto semantic :
         {"Terrains/Common/Surfaces/Dirt_01.emat", "Dirt_02.emat", "Dirt_03.emat", "MudWet_01.emat", "WetMud_01.emat",
          "SoilDark_01.emat", "Clay_01_Base.emat", "Grass/Forest/Beach/Mud_01.emat"})
        assert(EligibleNativeSurface(semantic));
    for (const auto semantic :
         {"Terrains/Common/Surfaces/BeachGrass_01.emat", "Grass_03.emat", "GrassWild_01.emat",
          "ForestConiferous_01_Base.emat", "ForestDeciduous_02.emat", "Pebbles_02.emat", "SeaBed_01.emat",
          "mud/Unknown_01.emat", "beachgrass_mud_01.emat", "MudGrass_01.emat", "DirtAndRock.emat", "SandySoil_01.emat",
          "RoadDirt_01.emat", "TrailDirt_01.emat", "DirtPath_01.emat", "mud/Soiled_01.emat"})
        assert(!EligibleNativeSurface(semantic));
    // A generic inherited BCR has no native soft-soil classification authority.
    assert(EligibleSurface("Dirt_01_BCR.edds"));
    assert(!EligibleNativeSurface("BeachGrass_01.emat"));
    assert(!EligibleNativeSurface("Unknown_01.emat"));

    // Exact actual CWA catalogue identity, not an inferred Czech texture alias or
    // a blanket Field/sound=dirt rule. Conflicting/missing metadata stays dry.
    assert(EligibleLegacySurface("Field", "pol", "dirt"));
    assert(EligibleLegacySurface("Field", "pol", "dirt", ""));
    for (const auto character : {"Grass", "GrassDense", "Crops", "FieldCharacter", "None", "unknown", " "})
        assert(!EligibleLegacySurface("Field", "pol", "dirt", character));
    assert(!EligibleSurface("Field"));
    assert(!EligibleNativeSurface("Field"));
    for (const auto semantic : {"", "Default", "field", "FieldGrass", "Field_Mod", "Village", "MudBuilding",
                                "Grass", "Forest", "Sand", "Roadway", "Cesta", "Asfalt", "Runway"})
        assert(!EligibleLegacySurface(semantic, "pol", "dirt"));
    for (const auto pattern : {"", "POL", "pol*", "pol??????", "data/pol", "pol.paa", "bah", "hlinasterk*"})
        assert(!EligibleLegacySurface("Field", pattern, "dirt"));
    for (const auto sound : {"", "Dirt", "normalExt", "grass", "rock", "gravel", "road", "forest"})
        assert(!EligibleLegacySurface("Field", "pol", sound));
    assert(!EligibleLegacySurface("Village", "bah", "normalExt"));
    assert(!EligibleLegacySurface("MudBuilding", "hlinasterk*", "rock"));
    assert(!EligibleLegacySurface("Grass", "tn??????", "normalExt"));
    assert(!EligibleLegacySurface("Roadway", "silnice*", "road"));
    assert(EligibleLegacySurface("Mud", "custom-mud", "mud")); // previous explicit mud admission
    assert(EligibleLegacySurface("Mud", "custom-mud", "mud", "custom-character")); // only stock exception narrowed

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
    Wetness shade, sun, wind;
    for (auto* sample : {&shade, &sun, &wind}) {
        sample->Update(0.0f, 1.0f);
        sample->Update(60.0f, 1.0f);
    }
    shade.Update(120.0f, 0.0f);
    sun.Update(120.0f, 0.0f, 1.0f);
    wind.Update(120.0f, 0.0f, 0.0f, 1.0f);
    assert(sun.value < wind.value && wind.value < shade.value);
    const float sunPaused = sun.value;
    assert(sun.Update(120.0f, 0.0f, 1.0f, 1.0f) == sunPaused);
    Wetness sunSingle, sunStepped;
    sunSingle.Update(0.0f, 0.8f);
    sunStepped.Update(0.0f, 0.8f);
    sunSingle.Update(90.0f, 0.8f, 0.7f, 0.4f);
    for (int frame = 1; frame <= 5400; ++frame)
        sunStepped.Update(frame / 60.0f, 0.8f, 0.7f, 0.4f);
    assert(std::abs(sunSingle.value - sunStepped.value) < 0.00005f);
    return 0;
}
