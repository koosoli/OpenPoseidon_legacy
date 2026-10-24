#pragma once
#include "EarthTerrainStream.hpp"
#include "include/wgpu_renderer.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace Poseidon
{
// OSM presentation only: optional pre-tiled HTTPS source, no game assets, collision or simulation.
class EarthBuildingStream
{
public:
    static constexpr size_t MaxBytes=8*1024*1024, MaxBuildings=5000, MaxVertices=128, MaxTriangles=400000;
    static constexpr double CellDegrees=.02;
    struct Cell { int latitude,longitude; bool operator==(const Cell&) const=default; };
    struct Ground {
        float x,z,grid;
        double latitude,longitude;
        uint64_t revision,generation;
        std::vector<float> heights;
        // The coarser horizon grid around the near patch (HorizonOffset spacing): buildings beyond the near
        // patch stand on it. At ground level the near patch is only 1.6 km across.
        float centreX=0,centreZ=0,horizonRadius=0;
        std::vector<float> horizonHeights;
        std::vector<uint8_t> horizonValid;
    };
    // u picks the building's colour from the Palette (walls 0-3, pitched roofs 4-6, flat roofs 7); tu/tv are
    // the face's texture coordinates in its Material's tiles and `material` the Material it is drawn with.
    struct Vertex { float x,y,z,nx,ny,nz,u=0,v=.5f,tu=0,tv=0; uint32_t material=0; };
    // The surfaces a building is drawn with when the game's own house textures are available (the renderer
    // falls back to the Palette colour, MaterialPalette, for any it cannot load): two-storey facades for
    // houses, the ones without a door for taller blocks (the tile repeats per two floors; OFP's one-storey
    // window bands carry a dark eave stripe that turned a block into a striped crate), roof tiles, a flat
    // roof. Sizes are one texture tile in metres; a facade's bottom edge stands on the ground.
    enum Material : uint32_t { Plaster,Derelict,Brick,PlainPlaster,BlockA,BlockB,TilesRed,TilesDark,TilesBrown,FlatRoof,
                               MaterialCount };
    static constexpr const char* MaterialTexture[MaterialCount]={
        "data\\hruzdum_backn.pac","data\\dum2_s.pac","data\\cihldum.pac","data\\hruzdum_siden.pac",
        "data\\hruzdum_siden.pac","data\\hruzdum_backn.pac",
        "data\\tasky_p.pac","data\\tasky_tmavsi.pac","data\\hruzdum_strecha.pac","data\\kostel_t_strecha.pac"};
    static constexpr float MaterialTile[MaterialCount][2]={
        {6.4f,6.4f},{6.4f,6.4f},{6.4f,6.4f},{6.4f,6.4f},{6.4f,6.4f},{6.4f,6.4f},
        {4.0f,4.0f},{4.0f,4.0f},{5.0f,5.0f},{8.0f,8.0f}};
    static constexpr int MaterialPalette[MaterialCount]={0,1,2,3,3,0,4,5,6,7};
    // whether a Material takes its game texture at all (FlatRoof: the palette's tones, see Palette)
    static constexpr bool MaterialTextured[MaterialCount]={true,true,true,true,true,true,true,true,true,false};
    static constexpr int PaletteSize=11;
    static constexpr uint8_t Palette[PaletteSize][4]={
        {214,204,184,255},{232,228,218,255},{188,180,168,255},{202,186,160,255}, // plaster, white, grey, sand
        {146,70,50,255},{104,74,58,255},{78,80,86,255},                          // terracotta, brown, slate
        // flat roofs (no game texture: the only one, kostel_t_strecha, is a church's patinated copper, and a
        // city seen from above went blue-green, owner 2026-10-24): gravel grey, concrete, bitumen, light gravel
        {128,126,120,255},{156,153,146,255},{84,84,82,255},{170,162,148,255}};
    static constexpr int FlatRoofTones=4;
    static constexpr float PaletteU(int index){return (index+.5f)/PaletteSize;}
    struct Mesh {
        Cell cell{};
        uint64_t revision=0,generation=0;
        size_t buildings=0,skipped=0,estimatedHeights=0;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::string error;
        // The draw meshes' contents, one per Material, split on the worker (Split) for the textured materials
        // in `partsMask` -- the main thread only creates the meshes. Empty when the mask was not known yet.
        std::array<std::vector<WgrMeshVertex>,MaterialCount> parts;
        std::array<std::vector<uint32_t>,MaterialCount> partIndices;
        uint32_t partsMask=0;
        bool partsReady=false;
    };
    // Split `mesh` by material into parts/partIndices: textured materials (bit m of `textured`) take the
    // facade/roof texture coordinates, the rest the Palette colour.
    static void Split(Mesh& mesh,uint32_t textured);
    // Which Materials have their game texture (from the main thread, once it has tried them): the worker then
    // splits each new mesh before handing it over.
    void SetTexturedMaterials(uint32_t mask){_textured=mask;}
    static Cell CellAt(double latitude,double longitude);
    static std::string FileName(Cell cell);
    static Mesh BuildCell(const std::string& json,const Ground& ground,Cell cell);
    // Cached 3x3 cell window, one shared geometry budget, no network. Checks
    // cancellation between files; cancelled output is never published by Run.
    static Mesh BuildNeighborhood(const std::filesystem::path& cache,const Ground& ground,Cell centre,
                                  const std::function<bool()>& cancelled={});
    explicit EarthBuildingStream(std::filesystem::path cache);
    ~EarthBuildingStream();
    void Request(const EarthTerrainStream::Patch& ground,float cameraX,float cameraZ);
    void Relocate();
    std::unique_ptr<Mesh> TakeReady();
private:
    void Run();
    std::filesystem::path _cache;
    std::thread _worker;
    std::mutex _mutex;
    std::condition_variable _wake;
    bool _stop=false,_pending=false,_requested=false;
    uint64_t _request=0;
    Cell _cell{};
    Ground _ground{};
    std::unique_ptr<Mesh> _ready;
    std::atomic<uint32_t> _textured{UINT32_MAX}; // UINT32_MAX: not known yet
};
}
