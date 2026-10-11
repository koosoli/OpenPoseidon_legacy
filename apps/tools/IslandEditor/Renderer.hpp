#pragma once

// The 3D view of the island: the ground in chunks with every cell's texture from one
// atlas, the sea, markers for the objects, and the small map.

#include "GameData.hpp"
#include "Island.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Dx12
{
class Device;
class Texture;
class Buffer;
class VertexDecl;
class VertexShader;
class PixelShader;
} // namespace Dx12

namespace island
{
struct Camera
{
    float pos[3] = {0, 300, 0};
    float yaw = 0;     // radians, 0 = looking north (+z), positive turns east
    float pitch = -0.6f;
    float fovY = 1.0472f; // 60 degrees
    float nearZ = 2.0f, farZ = 90000.0f;

    void Basis(float forward[3], float right[3], float up[3]) const;
    void ViewProj(float aspect, float m[16]) const; // column-major, for mul(m, float4(p, 1))
    void Ray(float mouseX, float mouseY, float width, float height, float dir[3]) const;
    // false when the point is behind the camera
    bool Project(const float world[3], float width, float height, float& sx, float& sy) const;
};

struct ViewOptions
{
    int mode = 0; // 0 textures, 1 heights, 2 slopes
    bool grid = false;
    bool sea = true;
    bool objects = true;
    float contour = 0;        // metres between contour lines, 0 = none
    float sunYaw = 2.2f;      // where the light comes from
    float sunHeight = 0.9f;   // radians above the horizon
    float objectRange = 2500; // markers are drawn this far from the camera
    bool models = true;       // objects as their models where the camera is near them
    float modelRange = 900;   // ... this near
    float modelFine = 260;    // and in full detail this near
};

struct BrushView
{
    bool shown = false;
    float x = 0, z = 0, radius = 0;
    float r = 1, g = 1, b = 1;
    // a line on the ground (the ramp tool)
    bool line = false;
    float lx0 = 0, lz0 = 0, lx1 = 0, lz1 = 0;
};

// A line on top of everything (a tool's guide), in world space.
struct OverlayLine
{
    float a[3], b[3];
    uint32_t color; // 0xAARRGGBB
};

enum class ObjectKind
{
    Building,
    Tree,
    Bush,
    Road,
    Other
};
ObjectKind KindOf(const std::string& model);

class Renderer
{
  public:
    static constexpr int kTile = 128;                 // one texture in the atlas
    static constexpr int kAtlasCols = 32, kAtlasRows = 16;
    static constexpr int kChunk = 32;                 // cells along a chunk

    bool Init(Dx12::Device* device);
    void Shutdown();

    // A (new) island: its textures into the atlas, its ground into chunks.
    void SetIsland(const Island* is, const GameData* game);
    // One slot of the table got another texture.
    void UpdateTexture(int slot);
    void UpdateTextures(const std::vector<int>& slots);
    // Every texture again (the table changed, or the filter did).
    void RefreshTextures();
    // Shows the textures recoloured without changing anything: called with the RGBA pixels
    // of each one as it is put in the atlas.  Empty = as they are.
    void SetTextureFilter(std::function<void(uint8_t* rgba, size_t count)> filter);
    // Rebuilds what Island::TakeDirty reported.
    void TerrainChanged(int x0, int z0, int x1, int z1);

    void Draw(const Camera& cam, const ViewOptions& view, const BrushView& brush, int width, int height,
              int selectedObject, const std::vector<OverlayLine>* overlay = nullptr);

    // for the interface
    uint64_t AtlasHandle() const { return _atlasHandle; }
    void AtlasUV(int slot, float uv0[2], float uv1[2]) const;
    bool TextureMissing(int slot) const { return slot < 0 || slot >= kMaxTextures || _missing[slot]; }
    uint64_t MinimapHandle() const { return _minimapHandle; }
    int MinimapSize() const { return _minimapSize; }
    void UpdateMinimap(const ViewOptions& view);
    int ChunksDrawn() const { return _chunksDrawn; }

    // the objects drawn as their models in the last frame
    struct ModelStats
    {
        int areas = 0, draws = 0, triangles = 0, textures = 0, waiting = 0;
        // since the island was opened: what building and reading cost
        int builds = 0, texturesRead = 0;
        float buildMs = 0, textureMs = 0;
    };
    const ModelStats& Models() const { return _modelStats; }
    // the area (of 200 m) that holds a place: what of it is built
    struct AreaInfo
    {
        bool valid = false, dirty = false;
        int built = -1, triangles = 0, objects = 0;
        unsigned builds = 0;
        float minY = 0, maxY = 0;
    };
    AreaInfo Area(float x, float z) const;
    int AreaCount() const { return static_cast<int>(_objChunks.size()); }
    AreaInfo AreaAt(int index) const;
    // the lowest point of an object's model as it was built into its area
    bool BakedBase(int object, float& y) const;
    bool DrawnAsModel(int object) const
    {
        return object >= 0 && object < static_cast<int>(_objAsModel.size()) && _objAsModel[object] != 0;
    }

  private:
    struct Chunk
    {
        Dx12::Buffer* vb = nullptr;
        int cx = 0, cz = 0, cells = 0;
        float minY = 0, maxY = 0;
        bool dirty = true;
    };
    void BuildChunk(Chunk& c);

    // ---- objects as their models: baked by the area, near the camera
    static constexpr int kObjCells = 4; // cells along an area: 200 m
    struct ModelTex
    {
        std::string name;
        Dx12::Texture* tex = nullptr;
        bool tried = false;
    };
    struct ModelBatch
    {
        int texture = 0;
        int baseVertex = 0;
        unsigned vertices = 0, startIndex = 0, triangles = 0;
        bool surface = false; // lies on the ground (a road): drawn just in front of it
    };
    struct ObjChunk
    {
        std::vector<int> objects;   // of the island, standing in this area
        std::vector<uint8_t> baked; // which of them the batches hold
        std::vector<float> base;    // the lowest point of each as built
        uint64_t hash = 0;          // of what they are and where
        int built = -1;             // -1 not, 0 in full detail, 1 coarse
        bool dirty = false;         // built from what has changed since
        Dx12::Buffer* vb = nullptr;
        Dx12::Buffer* ib = nullptr;
        std::vector<ModelBatch> batches;
        float minY = 0, maxY = 0;
        int triangles = 0;
        unsigned builds = 0;        // times it was built
    };
    void BinObjects();
    int BuildObjChunk(ObjChunk& c, int lod);
    void ReleaseObjChunk(ObjChunk& c);
    void ReleaseModels(bool textures);
    int ModelTexture(const std::string& name);
    void LoadModelTexture(ModelTex& t);
    void DrawModels(const Camera& cam, const ViewOptions& view, const float fwd[3], const float vp[16]);
    void BuildTile(int slot);
    void UploadAtlas();
    void ReleaseChunks();

    Dx12::Device* _dev = nullptr;
    const Island* _is = nullptr;
    const GameData* _game = nullptr;

    Dx12::VertexShader* _vsTerrain = nullptr;
    Dx12::PixelShader* _psTerrain = nullptr;
    Dx12::VertexShader* _vsLine = nullptr;
    Dx12::PixelShader* _psLine = nullptr;
    Dx12::PixelShader* _psModel = nullptr;
    Dx12::Texture* _grey = nullptr;
    std::vector<ObjChunk> _objChunks;
    int _objPerSide = 0;
    unsigned _objRevision = 0;
    bool _objBinned = false, _modelsShown = false;
    std::vector<uint8_t> _objAsModel;
    std::vector<ModelTex> _modelTex;
    std::map<std::string, int> _modelTexIndex;
    ModelStats _modelStats;
    Dx12::VertexDecl* _declTerrain = nullptr;
    Dx12::VertexDecl* _declLine = nullptr;
    Dx12::Buffer* _ib = nullptr;
    int _ibCells = 0;

    std::vector<Chunk> _chunks;
    int _chunkCells = kChunk, _chunksPerSide = 0;
    int _chunksDrawn = 0;

    Dx12::Texture* _atlas = nullptr;
    uint64_t _atlasHandle = 0;
    std::vector<std::vector<uint8_t>> _atlasMips; // BGRA, level 0 first
    bool _missing[kMaxTextures] = {};
    std::function<void(uint8_t*, size_t)> _filter;
    float _average[kMaxTextures][3] = {};

    Dx12::Texture* _minimap = nullptr;
    uint64_t _minimapHandle = 0;
    int _minimapSize = 0;
};
} // namespace island
