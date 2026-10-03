#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Dev/Diag/CaveEditor.hpp>

TEST_CASE("Bounded terrain holes preserve the mountain above a cave", "[cave-editor][sinkhole]")
{
    Landscape::TerrainHoleArea holes[2];
    for (auto& hole : holes)
    {
        hole.n=4;
        hole.x[0]=0; hole.z[0]=0;
        hole.x[1]=4; hole.z[1]=0;
        hole.x[2]=4; hole.z[2]=4;
        hole.x[3]=0; hole.z[3]=4;
        hole.minX=hole.minZ=0;
        hole.maxX=hole.maxZ=4;
        hole.floorY=10;
        hole.ceilingY=13;
    }
    REQUIRE(Landscape::TerrainHoleIndex(holes,1,2,2,11)==0);
    REQUIRE(Landscape::TerrainHoleIndex(holes,1,2,2,13)==0);
    REQUIRE(Landscape::TerrainHoleIndex(holes,1,2,2,13.01f)==-1);
    REQUIRE(Landscape::TerrainHoleIndex(holes,1,5,2,11)==-1);
    // Footprint discovery remains available without claiming height admission.
    REQUIRE(Landscape::TerrainHoleIndex(holes,1,2,2)==0);
    // A bounded first hole must not hide a later legacy unbounded stairwell.
    holes[1].ceilingY=1e20f;
    REQUIRE(Landscape::TerrainHoleIndex(holes,2,2,2,30)==1);
    REQUIRE(Landscape::TerrainHoleIndex(holes,2,2,2,-100)==0);
    holes[0].n=0;
    REQUIRE(Landscape::TerrainHoleIndex(holes,1,2,2,11)==-1);
}

TEST_CASE("Cave mouse wheel reaches sub-grid openings and clamps repeated scrolling", "[cave-editor]")
{
    auto previous=Poseidon::Dev::CaveEditor();
    Poseidon::Dev::CaveEditor()={};
    for (int i=0;i<100;++i) Poseidon::Dev::ResizeEditorCave(-8);
    REQUIRE(Poseidon::Dev::CaveEditor().width==0.1f);
    REQUIRE(Poseidon::Dev::CaveEditor().height==0.1f);
    for (int i=0;i<100;++i) Poseidon::Dev::ResizeEditorCave(8);
    REQUIRE(Poseidon::Dev::CaveEditor().width==12.0f);
    REQUIRE(Poseidon::Dev::CaveEditor().height==8.0f);
    Poseidon::Dev::CaveEditor()=previous;
}
